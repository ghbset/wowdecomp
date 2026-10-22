#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include "Engine/Source/Base/FrameProfile.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/D3d11Util.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/FfShaders.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/GxFrameLatency.h"
#include "Engine/Source/Os/Error.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Services/TextureBlob.h"
#include "Engine/Source/Storm/H/storm.h"

// Phase status (see README.md): device, swap chain, back buffer, readable depth, clear/present/resize, buffer pools,
// textures, draw submission, render-state objects, cbuffers, input layouts, fixed function and translated shaders
// are real. What is left is marked TODO(phase N).

typedef HRESULT (WINAPI* CreateDXGIFactory1Proc)(REFIID riid, void** factory);

// cbuffer b0 per stage: the 256 float4 registers. The device block b1 is separate (shader-xlate README):
// int4 i[16], float4 alphaRef, float4 clipPlane[6].
static const uint32_t kConstantBufferBytes = 256 * 16;
static const uint32_t kDeviceConstantBytes = 23 * 16;

static const uint32_t s_gxAttribToD3dAttribSize[7] = { 4, 4, 4, 8, 12, 4, 4 };

// Colour formats: 565 is widened (D3D11.0 has no 16-bit swap chain formats). Index = CGxFormat::Format.
const DXGI_FORMAT CGxDeviceD3d11::s_GxFormatToDxgiColor[CGxFormat::Formats_Last] = {
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM
};

// DECOMP: OMSetDepthStencilState's stencil ref. Not a member: wxl reads fixed offsets of the device (x86 +0x397C).
static UINT s_stencilRef = 0;

// Depth formats as { texture (typeless), DSV, SRV }. Index = CGxFormat::Format (D16, D24X8, D24S8, D32 at 4..7).
const DXGI_FORMAT CGxDeviceD3d11::s_GxFormatToDxgiDepth[CGxFormat::Formats_Last][3] = {
    { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
    { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
    { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
    { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
    { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM },
    { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
    { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
    { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT }
};

// Same memory layouts as the D3D9 formats, so Blit copies texels unchanged. The 16-bit BGRA formats need D3D 11.1
// (Windows 8+); where they are missing, ITexCreate falls back through s_tolerableTexFmtMapping like D3D9 does.
const DXGI_FORMAT CGxDeviceD3d11::s_GxTexFmtToDxgiFmt[GxTexFormats_Last] = {
    DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM,
    DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM,
    DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT
};

// Constant like 4.3.4's (0x00BD67E8 is read-only there): a failed create falls back for that one texture only.
// 16-bit formats widen to Argb8888 (B8G8R8A8) like 4.3.4 widens them to 8888: native B5G6R5/B5G5R5A1 are optional
// and cannot always take GENERATE_MIPS, and stock Blit has no 1555/565 -> 8888. ConvertTexels16To8888 expands them.
const EGxTexFormat CGxDeviceD3d11::s_GxTexFormatToUse[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Abgr8888, GxTex_Argb8888, GxTex_Argb8888, GxTex_Argb8888, GxTex_Argb8888, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8, GxTex_Abgr16161616F };

// Unreached for 16-bit formats now that s_GxTexFormatToUse widens them.
const EGxTexFormat CGxDeviceD3d11::s_tolerableTexFmtMapping[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Argb1555, GxTex_Rgb565, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8, GxTex_Abgr16161616F };

// Triangle fans have no D3D11 topology: Draw() rewrites them as lists.
const D3D11_PRIMITIVE_TOPOLOGY CGxDeviceD3d11::s_primitiveConversion[GxPrims_Last] = {
    D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
};



// d3d11DumpFrame 1: log every draw of the next frame to gx.log, one line each, then the CVar resets to 0.
static bool s_dumpFrame;
static uint32_t s_dumpDraw;

static CVar* D3d11DumpFrameCVar() {
    static CVar* cvar;

    if (!cvar) {
        cvar = CVar::Register("d3d11DumpFrame", "D3D11: 1 logs every draw of the next frame to gx.log, then resets to 0", 0, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    }

    return cvar;
}

static const char* DumpShaderName(const void* shader, bool ff) {
    if (ff || !shader) {
        return "FF";
    }

    const char* name = static_cast<const CGxShader*>(shader)->m_key.m_str;
    return name ? name : "?";
}


// Set when IDestroyD3d's final device Release leaves references: DeviceDestroy then keeps d3d11/dxgi loaded, since
// unloading them under a live device (and its driver/DXVK threads) can hang or crash the exit.
static bool s_deviceLeaked;


CGxDevice* CreateCGxDeviceD3d11() {
    void* mem = SMemAlloc(sizeof(CGxDeviceD3d11), __FILE__, __LINE__, 0);

    if (!mem) {
        return nullptr;
    }

    return new (mem) CGxDeviceD3d11();
}

CGxDeviceD3d11::CGxDeviceD3d11() {
    this->m_api = GxApi_D3d11;
    this->m_hwnd = nullptr;
    this->m_hwndClass = 0;
    this->m_ownhwnd = 0;
    this->m_d3dLib = nullptr;
    this->m_dxgiLib = nullptr;
    this->m_d3dCreateDevice = nullptr;
    this->m_dxgiFactory = nullptr;
    this->m_d3dDevice = nullptr;
    this->m_d3dContext = nullptr;
    this->m_swapChain = nullptr;
    this->m_featureLevel = D3D_FEATURE_LEVEL_10_0;
    this->m_backBuffer = nullptr;
    this->m_backBufferRtv = nullptr;
    this->m_depthTexture = nullptr;
    this->m_depthDsv = nullptr;
    this->m_depthSrv = nullptr;
    memset(this->m_rtDepthTexture, 0, sizeof(this->m_rtDepthTexture));
    memset(this->m_rtDepthDsv, 0, sizeof(this->m_rtDepthDsv));
    this->m_rtDepthNext = 0;
    this->m_resolveTexture = nullptr;
    this->m_sceneDepthCopy = nullptr;
    this->m_constantBuffers[0] = nullptr;
    this->m_constantBuffers[1] = nullptr;
    this->m_resolveValid = false;
    this->m_sceneDepthFresh = false;
    memset(this->m_cbBytes, 0, sizeof(this->m_cbBytes));
    memset(this->m_cbSent, 0, sizeof(this->m_cbSent));
    this->m_a2cVar = nullptr;
    memset(this->m_gpuConstants, 0, sizeof(this->m_gpuConstants));
    this->m_ffConstantBuffers[0] = nullptr;
    this->m_ffConstantBuffers[1] = nullptr;
    this->m_devConstantBuffer = nullptr;
    this->m_reserved397C = nullptr;
    memset(this->m_reservedD3d9Tail, 0, sizeof(this->m_reservedD3d9Tail));
    memset(this->m_ffVs, 0, sizeof(this->m_ffVs));
    memset(this->m_ffPs, 0, sizeof(this->m_ffPs));
    this->m_fanIndexBuffer = nullptr;
    this->m_zeroVertexBuf = nullptr;
    this->m_blitVs = nullptr;
    this->m_blitPs = nullptr;
    this->m_blitDepthPs[0] = nullptr;
    this->m_blitDepthPs[1] = nullptr;
    this->m_blitConstantBuffer = nullptr;
    this->m_backBufferSrv = nullptr;
    this->m_fanIndexBufferSize = 0;
    this->m_curRtv = nullptr;
    this->m_curRtvSamples = 1;
    this->m_curDsv = nullptr;
    this->m_savedDsv = nullptr;
    memset(this->m_curVertexBuf, 0, sizeof(this->m_curVertexBuf));
    memset(this->m_curVertexOfs, 0, sizeof(this->m_curVertexOfs));
    memset(this->m_curVertexStride, 0, sizeof(this->m_curVertexStride));
    this->m_curIndexBuf = nullptr;
    this->m_curTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    this->m_caps.m_colorFormat = GxCF_argb;
    this->m_caps.unk_0018 = 1;
    this->DeviceCreatePools();
    this->DeviceCreateStreamBufs();
}

CGxDeviceD3d11::~CGxDeviceD3d11() {
}

// ---------------------------------------------------------------------------------------------------------------
// Library, window

// 4.3.4 ILoadD3dLib (0x00852C60): d3d11.dll + dxgi.dll are loaded at run time, so Wow.exe gains no import and the
// D3D9 path is untouched.
int32_t CGxDeviceD3d11::ILoadD3dLib(HMODULE& d3dLib, HMODULE& dxgiLib, PFN_D3D11_CREATE_DEVICE& createDevice, IDXGIFactory1*& factory) {
    d3dLib = LoadLibraryA("d3d11.dll");
    dxgiLib = LoadLibraryA("dxgi.dll");

    if (!d3dLib || !dxgiLib) {
        D3d11Log("CGxDeviceD3d11::ILoadD3dLib(): unable to load d3d11.dll/dxgi.dll");
        CGxDeviceD3d11::IUnloadD3dLib(d3dLib, dxgiLib, factory);
        return 0;
    }

    createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3dLib, "D3D11CreateDevice"));
    CreateDXGIFactory1Proc createFactory = reinterpret_cast<CreateDXGIFactory1Proc>(GetProcAddress(dxgiLib, "CreateDXGIFactory1"));

    if (!createDevice || !createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        D3d11Log("CGxDeviceD3d11::ILoadD3dLib(): unable to GetProcAddress()/CreateDXGIFactory1()");
        CGxDeviceD3d11::IUnloadD3dLib(d3dLib, dxgiLib, factory);
        return 0;
    }

    return 1;
}

void CGxDeviceD3d11::IUnloadD3dLib(HMODULE& d3dLib, HMODULE& dxgiLib, IDXGIFactory1*& factory) {
    SafeRelease(factory);

    if (d3dLib) {
        FreeLibrary(d3dLib);
        d3dLib = nullptr;
    }

    if (dxgiLib) {
        FreeLibrary(dxgiLib);
        dxgiLib = nullptr;
    }
}

static ATOM WindowClassCreateD3d11() {
    HINSTANCE instance = GetModuleHandleA(nullptr);

    WNDCLASSEXA windowClass;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = CGxDeviceD3d11::WindowProcD3d11;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = "GxWindowClassD3d11";
    windowClass.hIcon = static_cast<HICON>(LoadImageA(instance, "BlizzardIcon.ico", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    windowClass.hCursor = LoadCursorA(instance, "BlizzardCursor.cur");

    if (!windowClass.hCursor) {
        windowClass.hCursor = LoadCursorA(instance, IDC_ARROW);
    }

    return RegisterClassExA(&windowClass);
}

// Same as CGxDeviceD3d::ICreateWindow apart from the class name.
int32_t CGxDeviceD3d11::ICreateWindow(CGxFormat& format) {
    HINSTANCE instance = GetModuleHandleA(nullptr);
    DWORD style;

    if (!format.m_window) {
        style = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_SYSMENU;
    } else if (format.m_maximize == 1) {
        style = WS_POPUP | WS_VISIBLE;
    } else if (format.m_maximize == 2) {
        style = WS_POPUP;
    } else {
        style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    }

    float aspectRatio = 0.0f;

    if (format.m_aspect && format.m_size.x && format.m_size.y) {
        aspectRatio = static_cast<float>(format.m_size.x) / format.m_size.y;
    }

    RECT clientRect = { 0, 0, format.m_size.x, format.m_size.y };
    AdjustWindowRectEx(&clientRect, style, FALSE, 0);

    CGxDevice::s_clientAdjust.x = clientRect.right - format.m_size.x - clientRect.left;
    CGxDevice::s_clientAdjust.y = clientRect.bottom - format.m_size.y - clientRect.top;

    if (format.m_window) {
        if (format.m_maximize == 1) {
            format.m_pos.x = 0;
            format.m_pos.y = 0;
            format.m_size.x = GetSystemMetrics(SM_CXSCREEN);
            format.m_size.y = GetSystemMetrics(SM_CYSCREEN);
        } else {
            RECT windowRect = { 0, 0, format.m_size.x, format.m_size.y };

            if (CGxDevice::AdjustWindowRectToWorkArea(aspectRatio, &CGxDevice::s_clientAdjust, &windowRect)) {
                format.m_pos.x = windowRect.left;
                format.m_pos.y = windowRect.top;
                format.m_size.x = windowRect.right - windowRect.left;
                format.m_size.y = windowRect.bottom - windowRect.top;
            } else {
                format.m_pos.x = 0;
                format.m_pos.y = 0;
            }
        }
    }

    int32_t width = format.m_size.x ? format.m_size.x : CW_USEDEFAULT;
    int32_t height = format.m_size.y ? format.m_size.y : CW_USEDEFAULT;

    if (format.m_window && format.m_maximize != 1 && format.m_size.x && format.m_size.y) {
        width += CGxDevice::s_clientAdjust.x;
        height += CGxDevice::s_clientAdjust.y;
    }

    this->m_hwnd = CreateWindowExA(WS_EX_APPWINDOW, "GxWindowClassD3d11", "World of Warcraft", style, format.m_pos.x, format.m_pos.y, width, height, nullptr, nullptr, instance, this);

    if (this->m_hwnd && format.m_maximize != 2) {
        ShowWindow(this->m_hwnd, SW_SHOWNORMAL);
    }

    return this->m_hwnd != nullptr;
}

// Same as CGxDeviceD3d::WindowProcD3d, except WM_SETCURSOR: there is no hardware cursor in D3D11.
LRESULT CALLBACK CGxDeviceD3d11::WindowProcD3d11(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    CGxDeviceD3d11* device = reinterpret_cast<CGxDeviceD3d11*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE:
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTA*>(lparam)->lpCreateParams));
            return 0;

        case WM_DESTROY:
            device->DeviceWM(GxWM_Destroy, 0, 0);
            return 0;

        case WM_SIZE: {
            CRect rect;
            rect.minY = 0.0f;
            rect.minX = 0.0f;
            rect.maxY = HIWORD(lparam);
            rect.maxX = LOWORD(lparam);

            uint32_t resizeType = 0;

            if (wparam == SIZE_MINIMIZED) {
                resizeType = 1;
            } else if (wparam == SIZE_MAXHIDE) {
                resizeType = 2;
            }

            device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), resizeType);
            break;
        }

        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) {
                if (!device->IDevIsWindowed()) {
                    CRect rect;
                    rect.minY = 0.0f;
                    rect.minX = 0.0f;
                    rect.maxY = 0.0f;
                    rect.maxX = 0.0f;
                    device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 1);
                }
            } else if (LOWORD(wparam) == WA_ACTIVE && !device->IDevIsWindowed()) {
                CRect rect;
                rect.minY = 0.0f;
                rect.minX = 0.0f;
                rect.maxY = 0.0f;
                rect.maxX = 0.0f;
                GxCapsWindowSizeInScreenCoords(rect);
                device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 3);
            }

            break;

        case WM_SETFOCUS:
            device->DeviceWM(GxWM_SetFocus, 0, 0);
            return 0;

        case WM_KILLFOCUS:
            device->DeviceWM(GxWM_KillFocus, 0, 0);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_ERASEBKGND:
            return 0;

        case WM_SETCURSOR:
            if (!device) {
                return DefWindowProcA(hwnd, message, wparam, lparam);
            }

            if (device->m_d3dDevice && LOWORD(lparam) == HTCLIENT) {
                SetCursor(nullptr);
                return 1;
            }

            break;

        case WM_DISPLAYCHANGE:
            if (!device->IDevIsWindowed()) {
                CRect rect;
                rect.minY = 0.0f;
                rect.minX = 0.0f;
                rect.maxY = HIWORD(lparam);
                rect.maxX = LOWORD(lparam);
                device->DeviceWM(GxWM_DisplayChange, reinterpret_cast<uintptr_t>(&rect), 0);
            }

            break;

        case WM_SYSCOMMAND:
            if (wparam == SC_SCREENSAVE || wparam == SC_MONITORPOWER) {
                return 0;
            }

            break;

        case WM_SIZING: {
            RECT* sizingRect = reinterpret_cast<RECT*>(lparam);

            CiRect rect;
            rect.minX = sizingRect->left;
            rect.minY = sizingRect->top;
            rect.maxX = sizingRect->right;
            rect.maxY = sizingRect->bottom;

            if (rect.maxX - rect.minX <= 0 || rect.maxY - rect.minY <= 0) {
                return 0;
            }

            switch (wparam) {
                case WMSZ_LEFT:
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    break;

                case WMSZ_RIGHT:
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    break;

                case WMSZ_TOP:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    break;

                case WMSZ_TOPLEFT:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    break;

                case WMSZ_TOPRIGHT:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                case WMSZ_BOTTOM:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                case WMSZ_BOTTOMLEFT:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    break;

                case WMSZ_BOTTOMRIGHT:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                default:
                    break;
            }

            sizingRect->left = rect.minX;
            sizingRect->bottom = rect.maxY;
            sizingRect->top = rect.minY;
            sizingRect->right = rect.maxX;

            return 1;
        }

        default:
            break;
    }

    if (device && device->m_windowProc) {
        return device->m_windowProc(hwnd, message, wparam, lparam);
    }

    return DefWindowProcA(hwnd, message, wparam, lparam);
}

// 4.3.4: 0x00852310/- (x86 only). DXGI drops a fullscreen swap chain to windowed on focus loss; nothing else brings
// it back. Only acts on a change: SetFullscreenState sends WM_SIZE, whose handler calls back in here with focus 1.
void CGxDeviceD3d11::ISetWindowFocus(int32_t focus) {
    int32_t changed = this->m_windowFocus != focus;
    this->m_windowFocus = focus;

    if (focus && !this->m_format.m_window) {
        RECT rect;
        GetWindowRect(this->m_hwnd, &rect);
        ClipCursor(&rect);
    }

    if (!changed || this->m_format.m_window || !this->m_swapChain) {
        return;
    }

    this->m_swapChain->SetFullscreenState(focus ? TRUE : FALSE, nullptr);

    if (!focus) {
        ShowWindow(this->m_hwnd, SW_MINIMIZE);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Device lifetime

int32_t CGxDeviceD3d11::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;

    HDC dc = GetDC(nullptr);

    if (GetDeviceGammaRamp(dc, &this->m_systemGammaRamp) && &this->m_gammaRamp != &this->m_systemGammaRamp) {
        this->m_gammaRamp = this->m_systemGammaRamp;
    }

    ReleaseDC(nullptr, dc);

    this->m_hwndClass = WindowClassCreateD3d11();

    if (!this->m_hwndClass) {
        char* error = OsGetLastErrorStr();
        D3d11Log("CGxDeviceD3d11::DeviceCreate(): WindowClassCreate() failed: %s", error);
        OsFreeLastErrorStr(error);
        this->DeviceDestroy();
        return 0;
    }

    if (this->ICreateD3d() && CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

int32_t CGxDeviceD3d11::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_ownhwnd = 0;
    CGxDevice::DeviceCreate(window, format);
    this->m_hwnd = static_cast<HWND>(window);

    if (this->ICreateD3d() && this->ICreateD3dDevice(format) && CGxDevice::DeviceCreate(window, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

int32_t CGxDeviceD3d11::ICreateD3d() {
    if (this->m_dxgiFactory) {
        return 1;
    }

    if (CGxDeviceD3d11::ILoadD3dLib(this->m_d3dLib, this->m_dxgiLib, this->m_d3dCreateDevice, this->m_dxgiFactory)) {
        return 1;
    }

    CGxDeviceD3d11::IUnloadD3dLib(this->m_d3dLib, this->m_dxgiLib, this->m_dxgiFactory);
    return 0;
}

int32_t CGxDeviceD3d11::DeviceSetFormat(const CGxFormat& format) {
    D3d11Log("CGxDeviceD3d11::DeviceSetFormat():");
    this->Log(format);
    D3d11Log("CGxDeviceD3d11::DeviceSetFormat(): MSAA %u -> %u (quality %g -> %g)", this->m_format.m_multisampleCount, format.m_multisampleCount, static_cast<double>(this->m_format.m_multisampleQuality), static_cast<double>(format.m_multisampleQuality));

    // Every format change (MSAA, size, windowed/fullscreen, vsync, back-buffer format) only touches the swap chain
    // and what follows its size or sample count. Keep the device: D3D9 managed textures survive a Reset and the client
    // never re-supplies their texels, so a device recreate left them black/white. The adapter is always 0, so only a
    // removed device needs the full recreate.
    if (this->m_d3dDevice && this->m_context && this->m_d3dDevice->GetDeviceRemovedReason() == S_OK) {
        D3d11Log("CGxDeviceD3d11::DeviceSetFormat(): swap chain rebuild, device kept");

        // Render targets may follow the window size; their owners rebuild them on the regen callback (as on resize).
        this->ITexForceRecreation(0);
        this->IReleaseBackBuffer();
        ID3D11ShaderResourceView* noSrvs[16] = {};
        this->m_d3dContext->PSSetShaderResources(0, 16, noSrvs);
        memset(this->m_curSrv, 0, sizeof(this->m_curSrv));
        this->m_d3dContext->Flush();
        this->ILeaveFullscreen();
        SafeRelease(this->m_swapChain);

        if (this->m_hwnd) {
            ShowWindow(this->m_hwnd, SW_HIDE);
            DestroyWindow(this->m_hwnd);
        }

        this->m_hwnd = nullptr;
        this->m_format = format;

        CGxFormat createFormat = format;

        if (this->ICreateWindow(createFormat) && this->ICreateSwapChain(createFormat) && CGxDevice::DeviceSetFormat(createFormat)) {
            this->ISetCaps(createFormat);
            this->m_viewportDirty = 1;
            this->m_scissorRectDirty = 1;
            this->ISetWindowFocus(1);
            return 1;
        }

        D3d11Log("CGxDeviceD3d11::DeviceSetFormat(): swap chain rebuild failed, trying a full device recreate");
    } else {
        D3d11Log("CGxDeviceD3d11::DeviceSetFormat(): full device recreate");
    }

    if (this->m_hwnd) {
        ShowWindow(this->m_hwnd, SW_HIDE);
    }

    this->IDestroyD3d();

    if (this->m_hwnd) {
        DestroyWindow(this->m_hwnd);
    }

    this->m_hwnd = nullptr;
    this->m_format = format;

    CGxFormat createFormat = format;

    if (this->ICreateWindow(createFormat) && this->ICreateD3dDevice(createFormat) && CGxDevice::DeviceSetFormat(createFormat)) {
        this->ISetWindowFocus(1);
        return 1;
    }

    D3d11Log("CGxDeviceD3d11::DeviceSetFormat(): unable to set format!");
    this->IDestroyD3d();

    if (this->m_hwnd) {
        DestroyWindow(this->m_hwnd);
    }

    this->m_hwnd = nullptr;

    return 0;
}

void CGxDeviceD3d11::DeviceDestroy() {
    D3d11Log("CGxDeviceD3d11::DeviceDestroy(): begin");

    if (this->m_sceneDepthCopy) {
        this->TexDestroy(this->m_sceneDepthCopy);
        this->m_sceneDepthCopy = nullptr;
    }

    CGxDevice::DeviceDestroy();
    D3d11Log("CGxDeviceD3d11::DeviceDestroy(): CGxDevice::DeviceDestroy done");

    // Not in 4.3.4's order: leave fullscreen while the window still exists. DXGI drives the mode change through the
    // focus window's messages, so doing it after DestroyWindow (as IDestroyD3d would) can block forever.
    this->ILeaveFullscreen();

    if (this->m_hwnd && this->m_ownhwnd) {
        D3d11Log("CGxDeviceD3d11::DeviceDestroy(): DestroyWindow");
        DestroyWindow(this->m_hwnd);
        this->m_hwnd = nullptr;
    }

    if (this->m_hwndClass) {
        UnregisterClassA(reinterpret_cast<const char*>(static_cast<uintptr_t>(this->m_hwndClass)), GetModuleHandleA(nullptr));
        this->m_hwndClass = 0;
    }

    D3d11Log("CGxDeviceD3d11::DeviceDestroy(): window gone");
    this->IDestroyD3d();

    // d3d11/dxgi stay loaded, as in 4.3.4 (its DeviceDestroy never calls IUnloadD3dLib): the OS unloads them at
    // exit, and FreeLibrary here could pull code out from under a hook (e.g. a plugin on Present) or a driver thread.
    D3d11Log("CGxDeviceD3d11::DeviceDestroy(): release factory, d3d11/dxgi stay loaded%s", s_deviceLeaked ? " (device still referenced)" : "");
    SafeRelease(this->m_dxgiFactory);
    D3d11Log("CGxDeviceD3d11::DeviceDestroy(): end");
}

void CGxDeviceD3d11::ILeaveFullscreen() {
    BOOL fullscreen = FALSE;

    if (this->m_swapChain && SUCCEEDED(this->m_swapChain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen) {
        D3d11Log("CGxDeviceD3d11: SetFullscreenState(FALSE) begin");
        HRESULT hr = this->m_swapChain->SetFullscreenState(FALSE, nullptr);
        D3d11Log("CGxDeviceD3d11: SetFullscreenState(FALSE) end: 0x%08X", static_cast<uint32_t>(hr));
    }
}

// Swap chain + back buffer views + depth on the current device and window. DeviceSetFormat rebuilds just this when
// the device survives, so textures, shaders and states keep their contents.
// CUSTOM (Badgermilk0 port): performance diagnostics. The device whose GPU FrameProfile's section switches time
// (D3d11GpuTimer.h)
static CGxDeviceD3d11* s_gpuTimedDevice;

static void GpuTimerMark(FrameProfileSection section) {
    CGxDeviceD3d11* device = s_gpuTimedDevice;

    if (device && device->m_context && device->m_d3dDevice && device->m_d3dContext) {
        device->m_gpuTimer.Mark(device->m_d3dDevice, device->m_d3dContext, section);
    }
}

int32_t CGxDeviceD3d11::ICreateSwapChain(const CGxFormat& format) {
    DXGI_SWAP_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));

    if (format.m_window) {
        const CRect& window = this->DeviceCurWindow();
        desc.BufferDesc.Width = static_cast<UINT>(window.maxX);
        desc.BufferDesc.Height = static_cast<UINT>(window.maxY);
    } else {
        desc.BufferDesc.Width = format.m_size.x;
        desc.BufferDesc.Height = format.m_size.y;
        desc.BufferDesc.RefreshRate.Numerator = format.m_refreshRate;
        desc.BufferDesc.RefreshRate.Denominator = format.m_refreshRate ? 1 : 0;
    }

    desc.BufferDesc.Format = s_GxFormatToDxgiColor[format.m_colorFormat];

    uint32_t samples = format.m_multisampleCount > 1 ? format.m_multisampleCount : 1;
    UINT quality = 0;

    if (samples > 1) {
        UINT colorLevels = 0;
        UINT depthLevels = 0;
        this->m_d3dDevice->CheckMultisampleQualityLevels(desc.BufferDesc.Format, samples, &colorLevels);
        this->m_d3dDevice->CheckMultisampleQualityLevels(s_GxFormatToDxgiDepth[format.m_depthFormat][1], samples, &depthLevels);
        UINT levels = colorLevels < depthLevels ? colorLevels : depthLevels;

        if (levels) {
            quality = static_cast<UINT>((levels - 1) * format.m_multisampleQuality);
        } else {
            samples = 1;
        }
    }

    desc.SampleDesc.Count = samples;
    desc.SampleDesc.Quality = quality;
    // Single-sampled back buffers are also shader inputs (4.3.4 BufferUsage 0x30).
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | (samples == 1 ? DXGI_USAGE_SHADER_INPUT : 0);
    desc.BufferCount = format.m_vsync && format.m_backBufferCount ? format.m_backBufferCount : 1;
    desc.OutputWindow = this->m_hwnd;
    desc.Windowed = format.m_window ? TRUE : FALSE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    if (FAILED(this->m_dxgiFactory->CreateSwapChain(this->m_d3dDevice, &desc, &this->m_swapChain))) {
        D3d11Log("CGxDeviceD3d11: CreateSwapChain failed");
        this->m_swapChain = nullptr;
        return 0;
    }

    this->m_dxgiFactory->MakeWindowAssociation(this->m_hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER); // 4.3.4: 0x00853EE0/0x140583C20

    if (format.m_fixLag) {
        IDXGIDevice1* dxgiDevice = nullptr;

        if (SUCCEEDED(this->m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDevice)))) {
            // CUSTOM (Badgermilk0 port): gxMaxFrameLatency frames (0 waits in ScenePresent, DXGI's minimum is 1)
            this->m_frameLatencyApplied = g_gxMaxFrameLatency;
            dxgiDevice->SetMaximumFrameLatency(g_gxMaxFrameLatency ? g_gxMaxFrameLatency : 1);
            dxgiDevice->Release();
        }
    }

    // CUSTOM (Badgermilk0 port): performance diagnostics (D3d11GpuTimer.h)
    s_gpuTimedDevice = this;
    g_frameProfileGpuMark = GpuTimerMark;

    return this->ICreateBackBuffer();
}

// 4.3.4 FUN_00853EE0: adapter 0 from the factory, FL {11_0, 10_1, 10_0}, single-threaded; one dynamic cbuffer per
// stage bound once at b0.
int32_t CGxDeviceD3d11::ICreateD3dDevice(const CGxFormat& format) {
    IDXGIAdapter1* adapter = nullptr;

    if (FAILED(this->m_dxgiFactory->EnumAdapters1(0, &adapter))) {
        D3d11Log("CGxDeviceD3d11: no adapter");
        return 0;
    }

    static const D3D_FEATURE_LEVEL s_levels[3] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = this->m_d3dCreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED, s_levels, 3, D3D11_SDK_VERSION, &this->m_d3dDevice, &this->m_featureLevel, &this->m_d3dContext);
    adapter->Release();

    if (FAILED(hr)) {
        D3d11Log("CGxDeviceD3d11: D3D11CreateDevice failed: 0x%08X", static_cast<uint32_t>(hr));
        this->m_d3dDevice = nullptr;
        this->m_d3dContext = nullptr;
        return 0;
    }

    D3d11Log("CGxDeviceD3d11: feature level 0x%X", static_cast<uint32_t>(this->m_featureLevel));

    if (!this->ICreateSwapChain(format)) {
        return 0;
    }

    // b0 for app and FF stages (4 KB each, the size every shader declares), b1 shared by both stages. Zero-filled so
    // registers nobody set read 0, as on D3D9.
    static const float s_zeroConstants[256 * 4] = {};
    this->m_a2cVar = CVar::Register("gxAlphaToCoverage", "Vulkan/D3D11: 1 smooths alpha-tested cut-outs (leaves, grass, fences) with alpha-to-coverage under MSAA (gxMultisample > 1)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    ID3D11Buffer** cbuffers[5] = { &this->m_constantBuffers[0], &this->m_constantBuffers[1], &this->m_ffConstantBuffers[0], &this->m_ffConstantBuffers[1], &this->m_devConstantBuffer };

    for (uint32_t i = 0; i < 5; i++) {
        D3D11_BUFFER_DESC cbDesc;
        memset(&cbDesc, 0, sizeof(cbDesc));
        cbDesc.ByteWidth = i == 4 ? kDeviceConstantBytes : kConstantBufferBytes;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        // The app register files start from the mirror, so a recreated device keeps what the app last set.
        D3D11_SUBRESOURCE_DATA init = { i < 2 ? this->m_gpuConstants[i] : s_zeroConstants, 0, 0 };

        if (FAILED(this->m_d3dDevice->CreateBuffer(&cbDesc, &init, cbuffers[i]))) {
            return 0;
        }

        if (i < 2) {
            this->m_cbSent[i] = kConstantBufferBytes;
        }
    }

    this->m_d3dContext->VSSetConstantBuffers(1, 1, &this->m_devConstantBuffer);
    this->m_d3dContext->PSSetConstantBuffers(1, 1, &this->m_devConstantBuffer);

    D3D11_BUFFER_DESC zeroDesc;
    memset(&zeroDesc, 0, sizeof(zeroDesc));
    zeroDesc.ByteWidth = 64;
    zeroDesc.Usage = D3D11_USAGE_IMMUTABLE;
    zeroDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA zeroInit = { s_zeroConstants, 0, 0 };

    if (FAILED(this->m_d3dDevice->CreateBuffer(&zeroDesc, &zeroInit, &this->m_zeroVertexBuf))) {
        return 0;
    }

    // Slot 15 is never used by a vertex format (8 streams at most) and nothing else rebinds it.
    UINT zeroStride = 0;
    UINT zeroOffset = 0;
    this->m_d3dContext->IASetVertexBuffers(15, 1, &this->m_zeroVertexBuf, &zeroStride, &zeroOffset);

    this->m_context = 1;
    this->IStateReset();

    this->ISetCaps(format);
    this->Log(this->m_caps);

    this->DeviceSetGamma(this->m_gammaRamp);
    this->ICursorCreate(format);

    // Shaders that outlived the previous device (IDestroyD3d released their objects) are rebuilt from m_code.
    for (uint32_t target = GxSh_Vertex; target <= GxSh_Pixel; target++) {
        TSHashTable<CGxShader, HASHKEY_STRI>& list = this->m_shaderList[target];

        for (CGxShader* shader = list.m_fulllist.Head(); shader; shader = list.m_fulllist.Next(shader)) {
            if (shader->m_loaded && !shader->m_apiSpecific) {
                this->IShaderCreate(shader);
            }
        }
    }

    g_theGxDevicePtr->ShaderCreate(CGxDevice::s_uiVertexShader, GxSh_Vertex, "Shaders\\Vertex", "UI", 2);
    g_theGxDevicePtr->ShaderCreate(&CGxDevice::s_uiPixelShader, GxSh_Pixel, "Shaders\\Pixel", "UI", 1);

    return 1;
}

void CGxDeviceD3d11::IDestroyD3d() {
    D3d11Log("CGxDeviceD3d11::IDestroyD3d(): begin (device %p, swap chain %p)", this->m_d3dDevice, this->m_swapChain);

    // 4.3.4 order: ClearState + Flush first, so nothing the releases below drop is still bound or queued.
    if (this->m_d3dContext) {
        this->m_d3dContext->ClearState();
        this->m_d3dContext->Flush();
        D3d11Log("CGxDeviceD3d11::IDestroyD3d(): ClearState + Flush done");
    }

    this->ILeaveFullscreen();

    for (uint32_t i = 0; i < 2; i++) {
        if (CGxDevice::s_uiVertexShader[i]) {
            g_theGxDevicePtr->ShaderDestroy(CGxDevice::s_uiVertexShader[i]);
        }
    }

    if (CGxDevice::s_uiPixelShader) {
        g_theGxDevicePtr->ShaderDestroy(CGxDevice::s_uiPixelShader);
    }

    // Every other shader object too (4.3.4 IReleaseD3dShaders(1)). Left alive, each held a device reference, so at
    // quit the device never reached 0 and d3d11/dxgi were unloaded under a live device and its worker threads.
    for (uint32_t target = GxSh_Vertex; target <= GxSh_Pixel; target++) {
        TSHashTable<CGxShader, HASHKEY_STRI>& list = this->m_shaderList[target];

        for (CGxShader* shader = list.m_fulllist.Head(); shader; shader = list.m_fulllist.Next(shader)) {
            SafeRelease(reinterpret_cast<IUnknown*&>(shader->m_apiSpecific));
            shader->m_valid = 0;
        }
    }

    this->m_curVs = nullptr;
    this->m_curPs = nullptr;
    D3d11Log("CGxDeviceD3d11::IDestroyD3d(): shaders released");
    this->ICursorDestroy();
    this->ITexForceRecreation(1);
    D3d11Log("CGxDeviceD3d11::IDestroyD3d(): cursor + textures released");
    this->IReleaseD3dPools(1);
    this->IReleaseD3dQueries();
    D3d11Log("CGxDeviceD3d11::IDestroyD3d(): pools + queries released");

    if (this->m_d3dContext) {
        this->m_d3dContext->ClearState();
    }

    this->IReleaseBackBuffer();
    SafeRelease(this->m_constantBuffers[0]);
    SafeRelease(this->m_constantBuffers[1]);
    this->IStateRelease();
    SafeRelease(this->m_fanIndexBuffer);
    SafeRelease(this->m_zeroVertexBuf);
    this->m_fanIndexBufferSize = 0;
    D3d11Log("CGxDeviceD3d11::IDestroyD3d(): back buffer, constant buffers, state objects released");

    SafeRelease(this->m_swapChain);
    SafeRelease(this->m_frameLatencyQuery);   // CUSTOM (Badgermilk0 port)
    this->m_gpuTimer.Release();               // CUSTOM (Badgermilk0 port)

    if (s_gpuTimedDevice == this) {
        s_gpuTimedDevice = nullptr;
        g_frameProfileGpuMark = nullptr;
    }

    D3d11Log("CGxDeviceD3d11::IDestroyD3d(): swap chain released");

    if (this->m_d3dContext) {
        this->m_d3dContext->Flush();
    }

    SafeRelease(this->m_d3dContext);
    D3d11Log("CGxDeviceD3d11::IDestroyD3d(): context released");

    if (this->m_d3dDevice) {
        ULONG refs = this->m_d3dDevice->Release();
        this->m_d3dDevice = nullptr;
        s_deviceLeaked = refs != 0;
        D3d11Log("CGxDeviceD3d11::IDestroyD3d(): device released (%u references left)", static_cast<uint32_t>(refs));
    }

    memset(this->m_curVertexBuf, 0, sizeof(this->m_curVertexBuf));
    this->m_curIndexBuf = nullptr;
    this->m_curTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    this->m_context = 0;
}

void CGxDeviceD3d11::ISetCaps(const CGxFormat& format) {
    uint32_t maxTextureSize = this->m_featureLevel >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;

    this->m_caps.m_numTmus = 8;
    this->m_caps.m_pixelCenterOnEdge = 1;
    this->m_caps.m_texelCenterOnEdge = 1;
    this->m_caps.m_texMaxSize[GxTex_2d] = maxTextureSize;
    this->m_caps.m_texMaxSize[GxTex_CubeMap] = maxTextureSize;
    this->m_caps.m_texMaxSize[GxTex_Rectangle] = maxTextureSize;
    this->m_caps.m_texMaxSize[GxTex_NonPow2] = maxTextureSize;
    this->m_caps.m_maxIndex = 0x00FFFFFF;
    this->m_caps.m_texFilterTrilinear = 1;
    this->m_caps.m_texFilterAnisotropic = 1;
    this->m_caps.m_maxTexAnisotropy = 16;
    this->m_caps.m_depthBias = 1;
    this->m_caps.m_numStreams = 16;
    // Stream offsets are always supported: buffers bind at their byte offset and draws use base vertex 0.
    this->m_caps.m_streamOffset = 1;

    for (uint32_t i = 0; i < GxShTargets_Last; i++) {
        this->m_caps.m_shaderTargets[i] = 0;
        this->m_caps.m_shaderConstants[i] = 0;
    }

    // Report the SM3 profiles (vs_3_0 / ps_3_0) so IShaderLoad picks the SM3 .bls files, which phase 4 translates.
    this->m_caps.m_shaderTargets[GxSh_Vertex] = 3;
    this->m_caps.m_shaderTargets[GxSh_Pixel] = 4;
    this->m_caps.m_shaderConstants[GxSh_Vertex] = 256;

    if (format.m_fixedFunction[GxSh_Pixel] != -1 && format.m_fixedFunction[GxSh_Pixel] <= this->m_caps.m_shaderTargets[GxSh_Pixel]) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = format.m_fixedFunction[GxSh_Pixel];
    }

    if (format.m_fixedFunction[GxSh_Vertex] != -1 && format.m_fixedFunction[GxSh_Vertex] <= this->m_caps.m_shaderTargets[GxSh_Vertex]) {
        this->m_caps.m_shaderTargets[GxSh_Vertex] = format.m_fixedFunction[GxSh_Vertex];
    }

    for (uint32_t i = 0; i < GxTexCapsFormats; i++) {
        UINT support = 0;
        this->m_caps.m_texFmt[i] = i && SUCCEEDED(this->m_d3dDevice->CheckFormatSupport(s_GxTexFmtToDxgiFmt[i], &support)) && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D);
    }

    this->m_caps.m_generateMipMaps = 1;

    static const EGxTexFormat s_rttFormats[4] = { GxTex_Argb8888, GxTex_Rgb565, GxTex_Gr1616F, GxTex_R32F };

    for (uint32_t i = 0; i < 4; i++) {
        UINT support = 0;
        this->m_caps.m_rttFormat[s_rttFormats[i]] = SUCCEEDED(this->m_d3dDevice->CheckFormatSupport(s_GxTexFmtToDxgiFmt[s_rttFormats[i]], &support)) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);
    }

    // VFOG: R16G16B16A16_FLOAT, a render target the fog samples bilinearly.
    UINT halfSupport = 0;
    const UINT halfNeed = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    this->m_caps.m_rttAbgr16161616F = SUCCEEDED(this->m_d3dDevice->CheckFormatSupport(s_GxTexFmtToDxgiFmt[GxTex_Abgr16161616F], &halfSupport)) && (halfSupport & halfNeed) == halfNeed;

    this->m_caps.m_rttFormat[GxTex_D24X8] = 1;

    this->m_caps.m_colorWrite = 1;
    this->m_caps.m_hwCursor = 0;
    // Every VS (translated and FF) writes SV_ClipDistance0/1 from the planes in cb1.
    this->m_caps.m_maxClipPlanes = 6;
    this->m_caps.m_texTarget[GxTex_2d] = 1;
    this->m_caps.m_texTarget[GxTex_CubeMap] = 1;
    this->m_caps.m_texTarget[GxTex_Rectangle] = 0;
    this->m_caps.m_texTarget[GxTex_NonPow2] = 1;
    this->m_caps.m_texNonPow2 = 0;
    this->m_caps.m_occlusionQuery = 1;
    // No point sprites or point size in D3D11.
    this->m_caps.m_pointParameters = 0;
    this->m_caps.m_pointScaleMax = 1.0f;
    this->m_caps.m_pointSprite = 0;
    this->m_caps.m_blendFactor = 1;
    this->m_caps.m_shaderAlphaRef = 1;
    this->m_caps.m_stencilBits = s_GxFormatToDxgiDepth[format.m_depthFormat][1] == DXGI_FORMAT_D24_UNORM_S8_UINT ? 8 : 0;  // DECOMP

    for (uint32_t i = 0; i < 3; i++) {
        this->m_caps.unused_0118[i] = 0;
    }

    this->m_caps.unk_0134 = this->m_caps.m_shaderTargets[GxSh_Pixel] != 4;
    this->m_caps.unk_0138 = this->m_caps.m_shaderTargets[GxSh_Pixel] != 4;
}

// ---------------------------------------------------------------------------------------------------------------
// Back buffer, depth, render targets

// Depth is typeless + DSV + SRV (PLAN contract). An MSAA depth is only sampleable from FL 10_1; at 10_0 it gets no
// SRV. TODO(phase 5): resolve MSAA depth into a single-sampled copy for readers (an improvement on 4.3.4).
int32_t CGxDeviceD3d11::ICreateDepth(uint32_t width, uint32_t height, uint32_t samples, ID3D11Texture2D*& texture, ID3D11DepthStencilView*& dsv, ID3D11ShaderResourceView** srv) {
    const DXGI_FORMAT* formats = s_GxFormatToDxgiDepth[this->m_format.m_depthFormat];
    bool readable = srv && (samples == 1 || this->m_featureLevel >= D3D_FEATURE_LEVEL_10_1);

    D3D11_TEXTURE2D_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = formats[0];
    desc.SampleDesc.Count = samples;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | (readable ? D3D11_BIND_SHADER_RESOURCE : 0);

    if (samples > 1) {
        DXGI_SWAP_CHAIN_DESC swapDesc;
        this->m_swapChain->GetDesc(&swapDesc);
        desc.SampleDesc.Quality = swapDesc.SampleDesc.Quality;
    }

    if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &texture))) {
        texture = nullptr;
        return 0;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
    memset(&dsvDesc, 0, sizeof(dsvDesc));
    dsvDesc.Format = formats[1];
    dsvDesc.ViewDimension = samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;

    if (FAILED(this->m_d3dDevice->CreateDepthStencilView(texture, &dsvDesc, &dsv))) {
        dsv = nullptr;
        return 0;
    }

    if (readable) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        memset(&srvDesc, 0, sizeof(srvDesc));
        srvDesc.Format = formats[2];

        if (samples > 1) {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
        }

        if (FAILED(this->m_d3dDevice->CreateShaderResourceView(texture, &srvDesc, srv))) {
            *srv = nullptr;
        }
    }

    return 1;
}

int32_t CGxDeviceD3d11::ICreateBackBuffer() {
    if (FAILED(this->m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&this->m_backBuffer)))) {
        this->m_backBuffer = nullptr;
        return 0;
    }

    if (FAILED(this->m_d3dDevice->CreateRenderTargetView(this->m_backBuffer, nullptr, &this->m_backBufferRtv))) {
        this->m_backBufferRtv = nullptr;
        return 0;
    }

    D3D11_TEXTURE2D_DESC desc;
    this->m_backBuffer->GetDesc(&desc);

    if (!this->ICreateDepth(desc.Width, desc.Height, desc.SampleDesc.Count, this->m_depthTexture, this->m_depthDsv, &this->m_depthSrv)) {
        return 0;
    }

    this->IBindRenderTargets();
    this->m_viewportDirty = 1;

    return 1;
}

void CGxDeviceD3d11::IReleaseBackBuffer() {
    if (this->m_d3dContext) {
        this->m_d3dContext->OMSetRenderTargets(0, nullptr, nullptr);
    }

    this->m_curRtv = nullptr;
    this->m_curRtvSamples = 1;
    this->m_curDsv = nullptr;
    SafeRelease(this->m_savedDsv);
    SafeRelease(this->m_backBufferRtv);
    SafeRelease(this->m_backBuffer);
    SafeRelease(this->m_depthSrv);
    SafeRelease(this->m_depthDsv);
    SafeRelease(this->m_depthTexture);
    for (int i = 0; i < kRtDepthSlots; i++) {
        SafeRelease(this->m_rtDepthDsv[i]);
        SafeRelease(this->m_rtDepthTexture[i]);
    }
    SafeRelease(this->m_resolveTexture);
    SafeRelease(this->m_backBufferSrv);
    SafeRelease(this->m_normalMsRtv);
    SafeRelease(this->m_normalMs);
}

// Colour = texture target or back buffer. Depth = depth target, else the back buffer's depth, or for a colour
// texture target a scratch depth of exactly its size (CGxDeviceD3d::ICreateDepthStencilSurface's job).
void CGxDeviceD3d11::IBindRenderTargets() {
    ID3D11RenderTargetView* rtv = this->m_backBufferRtv;
    ID3D11DepthStencilView* dsv = this->m_depthDsv;

    // A colour texture target without a view (failed create, plane past its levels, dropped by a recreate) binds no
    // colour and no back-buffer depth, rather than letting the offscreen pass draw onto the screen.
    if (this->m_textureTarget[GxBuffers_Color].m_texture && !this->m_textureTarget[GxBuffers_Color].m_apiSpecific) {
        D3d11LogOnce("CGxDeviceD3d11: colour target %ux%u plane %u has no view: its draws are dropped", this->m_textureTarget[GxBuffers_Color].m_texture->m_width, this->m_textureTarget[GxBuffers_Color].m_texture->m_height, this->m_textureTarget[GxBuffers_Color].m_plane);
        rtv = nullptr;
        dsv = nullptr;
    } else if (this->m_textureTarget[GxBuffers_Color].m_apiSpecific) {
        rtv = static_cast<ID3D11RenderTargetView*>(this->m_textureTarget[GxBuffers_Color].m_apiSpecific);

        if (!this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
            CGxTex* color = this->m_textureTarget[GxBuffers_Color].m_texture;
            // RTT MIPS: the bound level's extent (CGxDevice::DeviceSetRenderTarget), D3D11 wants an exact-size DSV.
            uint32_t level = color->m_target == GxTex_CubeMap ? 0 : this->m_textureTarget[GxBuffers_Color].m_plane;
            uint32_t width = color->m_width >> level ? color->m_width >> level : 1;
            uint32_t height = color->m_height >> level ? color->m_height >> level : 1;
            int slot = -1;

            // One scratch depth per target size, so FFX and vfog ping-pong no longer recreates one every frame.
            for (int i = 0; i < kRtDepthSlots && slot < 0; i++) {
                D3D11_TEXTURE2D_DESC desc;

                if (this->m_rtDepthTexture[i]) {
                    this->m_rtDepthTexture[i]->GetDesc(&desc);

                    if (desc.Width == width && desc.Height == height) {
                        slot = i;
                    }
                }
            }

            if (slot < 0) {
                slot = static_cast<int>(this->m_rtDepthNext++ % kRtDepthSlots);
                SafeRelease(this->m_rtDepthDsv[slot]);
                SafeRelease(this->m_rtDepthTexture[slot]);
                this->ICreateDepth(width, height, 1, this->m_rtDepthTexture[slot], this->m_rtDepthDsv[slot], nullptr);
            }

            dsv = this->m_rtDepthDsv[slot];
        }
    }

    if (this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
        dsv = static_cast<ID3D11DepthStencilView*>(this->m_textureTarget[GxBuffers_Depth].m_apiSpecific);
    }

    // Drop every PS SRV first: a texture becoming the target would otherwise be bound twice (the runtime nulls it
    // behind our back). IStateSyncTextures rebinds on the next draw.
    ID3D11ShaderResourceView* noSrvs[16] = {};
    this->m_d3dContext->PSSetShaderResources(0, 16, noSrvs);
    memset(this->m_curSrv, 0, sizeof(this->m_curSrv));

    // A depth texture (shadow map) whose size or sample count differs from the colour target: bind depth-only.
    // D3D11 rejects mismatched RTV/DSV pairs outright (D3D9 accepted a larger depth), which left the previous
    // targets bound and let the shadow clears/draws land on them. The viewport already follows the depth
    // texture through m_curWindowRect (CGxDevice::DeviceSetRenderTarget).
    if (rtv && dsv && this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
        ID3D11Resource* r = nullptr;
        ID3D11Resource* d = nullptr;
        rtv->GetResource(&r);
        dsv->GetResource(&d);
        D3D11_TEXTURE2D_DESC rd;
        D3D11_TEXTURE2D_DESC dd;
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&rd);
        static_cast<ID3D11Texture2D*>(d)->GetDesc(&dd);
        r->Release();
        d->Release();

        if (rd.Width != dd.Width || rd.Height != dd.Height || rd.SampleDesc.Count != dd.SampleDesc.Count) {
            static bool logged = false;

            if (!logged) {
                logged = true;
                D3d11Log("CGxDeviceD3d11: RTV %ux%u x%u / DSV %ux%u x%u mismatch: binding depth-only (logged once)", rd.Width, rd.Height, rd.SampleDesc.Count, dd.Width, dd.Height, dd.SampleDesc.Count);
            }

            rtv = nullptr;
        }
    }

    this->m_curRtv = rtv;
    this->m_curDsv = dsv;
    this->m_curRtvSamples = 1;

    if (rtv) {
        ID3D11Resource* r = nullptr;
        rtv->GetResource(&r);
        D3D11_TEXTURE2D_DESC rd;
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&rd);
        r->Release();
        this->m_curRtvSamples = rd.SampleDesc.Count;
    }

    this->IOMSetTargets();
}

// GBUFFER: the back buffer gets the normal target as RT1 (written only by oC1 pixel shaders, IStateSync's blend mask).
void CGxDeviceD3d11::IOMSetTargets() {
    ID3D11RenderTargetView* rtvs[2] = { this->m_curRtv, nullptr };
    UINT count = this->m_curRtv ? 1 : 0;

    if (count && this->m_curRtv == this->m_backBufferRtv && CGxDevice::s_normalBuffer && !this->m_normalSampled && (rtvs[1] = this->INormalRtv())) {
        count = 2;
    }

    this->m_d3dContext->OMSetRenderTargets(count, count ? rtvs : nullptr, this->m_curDsv);
}

// GBUFFER: the normal target's RTV, made on first use at the back buffer's size and sample count (cleared to 0).
ID3D11RenderTargetView* CGxDeviceD3d11::INormalRtv() {
    if (!this->m_backBuffer) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC bb;
    this->m_backBuffer->GetDesc(&bb);

    if (this->m_normalTex && (this->m_normalTex->m_width != bb.Width || this->m_normalTex->m_height != bb.Height)) {
        this->TexDestroy(this->m_normalTex);
        this->m_normalTex = nullptr;
        SafeRelease(this->m_normalMsRtv);
        SafeRelease(this->m_normalMs);
    }

    if (!this->m_normalTex) {
        CGxTexFlags flags(GxTex_Nearest);
        flags.m_renderTarget = 1;
        this->TexCreate(GxTex_2d, bb.Width, bb.Height, 0, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, nullptr, "GxNormalBuffer", this->m_normalTex);

        if (!this->m_normalTex) {
            return nullptr;
        }
    }

    bool fresh = false;

    if (this->m_normalTex->m_needsCreation || !this->m_normalTex->m_apiSpecificData) {
        this->ITexCreate(this->m_normalTex);
        SafeRelease(this->m_normalMsRtv);
        SafeRelease(this->m_normalMs);
        fresh = true;
    }

    TexApi* api = static_cast<TexApi*>(this->m_normalTex->m_apiSpecificData);

    if (!api || !api->m_texture || !api->m_rtv[0]) {
        return nullptr;
    }

    if (bb.SampleDesc.Count <= 1) {
        if (fresh) {
            const float zero[4] = {};
            this->m_d3dContext->ClearRenderTargetView(api->m_rtv[0], zero);
        }

        return api->m_rtv[0];
    }

    if (!this->m_normalMsRtv) {
        D3D11_TEXTURE2D_DESC desc;
        api->m_texture->GetDesc(&desc);
        desc.MipLevels = 1;
        desc.SampleDesc = bb.SampleDesc;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = 0;

        if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &this->m_normalMs))
            || FAILED(this->m_d3dDevice->CreateRenderTargetView(this->m_normalMs, nullptr, &this->m_normalMsRtv))) {
            SafeRelease(this->m_normalMs);
            this->m_normalMsRtv = nullptr;
            return nullptr;
        }

        const float zero[4] = {};
        this->m_d3dContext->ClearRenderTargetView(this->m_normalMsRtv, zero);
    }

    return this->m_normalMsRtv;
}

// GBUFFER: CGxDevice.h contract. RT1 is unbound until the next present (it is sampled now); MSAA resolves into the CGxTex.
CGxTex* CGxDeviceD3d11::DeviceNormalBuffer() {
    if (!this->m_context || !CGxDevice::s_normalBuffer || !this->m_normalTex || !this->m_normalTex->m_apiSpecificData) {
        return nullptr;
    }

    TexApi* api = static_cast<TexApi*>(this->m_normalTex->m_apiSpecificData);

    if (!this->m_normalSampled) {
        // ponytail: opaque draws after the first lighting pass of a frame write no normal; per-pass rebinding if one ever does.
        this->m_normalSampled = true;
        this->IOMSetTargets();

        if (this->m_normalMs) {
            D3D11_TEXTURE2D_DESC desc;
            api->m_texture->GetDesc(&desc);
            this->m_d3dContext->ResolveSubresource(api->m_texture, 0, this->m_normalMs, 0, desc.Format);
        }
    }

    return this->m_normalTex;
}

void CGxDeviceD3d11::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (!this->m_context) {
        return;
    }

    CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];

    // Same target, but ITexForceRecreation (device recreate, resize) dropped its view: fall through and rebind it.
    if (target.m_texture == texture && target.m_plane == plane && (!texture || target.m_apiSpecific)) {
        return;
    }

    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);
    target.m_apiSpecific = nullptr;

    if (texture) {
        if (texture->m_needsCreation || !texture->m_apiSpecificData) {
            this->ITexCreate(texture);
        }

        TexApi* api = static_cast<TexApi*>(texture->m_apiSpecificData);

        if (!texture->m_needsCreation && api) {
            if (buffer == GxBuffers_Color) {
                target.m_apiSpecific = plane < 16 ? api->m_rtv[plane] : nullptr;     // past its levels: no view (IBindRenderTargets)
            } else {
                target.m_apiSpecific = api->m_dsv;
            }
        }
    }

    this->IBindRenderTargets();

    GxXformSetViewport(this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);

    this->m_viewportDirty = 1;
    this->m_scissorRectDirty = 1;
}

void CGxDeviceD3d11::DeviceSaveDepthSurface() {
    if (this->m_context) {
        // Holds a reference: the view it names (a scratch depth, a depth texture) may be released before the restore.
        if (this->m_curDsv) {
            this->m_curDsv->AddRef();
        }
        SafeRelease(this->m_savedDsv);
        this->m_savedDsv = this->m_curDsv;
    }
}

void CGxDeviceD3d11::DeviceRestoreDepthSurface() {
    if (!this->m_context) {
        return;
    }

    this->m_curDsv = this->m_savedDsv;
    this->IOMSetTargets();
}

void CGxDeviceD3d11::DeviceResolveDepthBuffer(CGxTex* texture) {
    if (!this->m_context || !texture) {
        return;
    }

    if (texture->m_needsCreation || !texture->m_apiSpecificData) {
        this->ITexCreate(texture);
    }

    TexApi* api = static_cast<TexApi*>(texture->m_apiSpecificData);

    if (api && api->m_dsv) {
        this->m_curDsv = api->m_dsv;
        this->IOMSetTargets();
    }
}

// VFOG: snapshot of the back buffer's depth for the volumetric fog (CGxDevice.h contract).
// Single-sampled D24: CopyResource into a GxTex_D24X8 render target made through ITexCreate, so it has the
// R24_UNORM_X8 SRV; its filter is GxTex_Nearest and the sampler's ComparisonFunc is NEVER (plain point depth).
// MSAA or D16/D32: CopyResource cannot do it (sample count / format), so IBlit draws m_depthSrv into a GxTex_R32F
// render target with a Load shader (MSAA: sample 0, coa-vfog's one-sample-per-pixel copy; needs 10_1 for the
// multisampled SRV). Either is a separate resource from m_depthTexture, so it can be bound as an SRV while
// m_depthDsv stays the DSV. Resize/device recreate: ITexForceRecreation releases it (render-target flag), and a
// size or kind change makes a new one here.
CGxTex* CGxDeviceD3d11::DeviceCopySceneDepth(bool* transient) {
    // Only a format or feature gap (no depth SRV, no R32F target, the copy shader) is final; the rest retries.
    bool dummy;
    bool& retry = transient ? *transient : dummy;
    retry = true;
    if (!CGxDevice::s_depthUsers) {
        return nullptr;     // LEGION SetDepthUser: no consumer this frame
    }

    if (!this->m_context || !this->m_depthTexture) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc;
    this->m_depthTexture->GetDesc(&desc);

    bool copy = desc.SampleDesc.Count <= 1 && desc.Format == s_GxTexFmtToDxgiFmt[GxTex_D24X8];
    EGxTexFormat format = copy ? GxTex_D24X8 : GxTex_R32F;
    ID3D11PixelShader* ps = nullptr;

    if (!copy) {
        uint32_t ms = desc.SampleDesc.Count > 1 ? 1 : 0;

        if (!this->m_depthSrv || !this->m_caps.m_rttFormat[GxTex_R32F]) {
            retry = false;
            return nullptr;
        }

        if (!this->m_blitDepthPs[ms]) {
            const FfBlob& blob = ms ? g_blitDepthMsPs : g_blitDepthPs;

            if (FAILED(this->m_d3dDevice->CreatePixelShader(blob.data, blob.size, nullptr, &this->m_blitDepthPs[ms]))) {
                this->m_blitDepthPs[ms] = nullptr;
                D3d11LogOnce("CGxDeviceD3d11: VFOG depth copy shader failed (ms %u)", ms);
                retry = false;
                return nullptr;
            }
        }

        ps = this->m_blitDepthPs[ms];
    }

    if (this->m_sceneDepthCopy && (this->m_sceneDepthCopy->m_width != desc.Width || this->m_sceneDepthCopy->m_height != desc.Height || this->m_sceneDepthCopy->m_format != format)) {
        this->TexDestroy(this->m_sceneDepthCopy);
        this->m_sceneDepthCopy = nullptr;
    }

    if (!this->m_sceneDepthCopy) {
        CGxTexFlags flags(GxTex_Nearest);
        flags.m_renderTarget = 1;
        this->TexCreate(GxTex_2d, desc.Width, desc.Height, 0, format, format, flags, nullptr, nullptr, "VFogSceneDepth", this->m_sceneDepthCopy);
    }

    CGxTex* texture = this->m_sceneDepthCopy;

    if (texture->m_needsCreation || !texture->m_apiSpecificData) {
        this->m_sceneDepthFresh = false;
        this->ITexCreate(texture);
    }

    TexApi* api = static_cast<TexApi*>(texture->m_apiSpecificData);

    if (!api || !api->m_texture || !api->m_srv) {
        return nullptr;
    }

    // Nothing wrote or cleared depth since the last copy (water, then fog): it still holds it (audit P13).
    if (this->m_sceneDepthFresh) {
        retry = false;
        return texture;
    }

    if (copy) {
        this->m_d3dContext->CopyResource(api->m_texture, this->m_depthTexture);
    } else if (!this->IBlit(this->m_depthSrv, desc.Width, desc.Height, nullptr, texture, 0, 0, nullptr, false, ps)) {
        return nullptr;
    }

    this->m_sceneDepthFresh = true;
    retry = false;
    return texture;
}

// The single-sampled back buffer, or a resolve of the MSAA one (4.3.4 keeps the same copy at +0x2820).
ID3D11Texture2D* CGxDeviceD3d11::IResolvedBackBuffer() {
    D3D11_TEXTURE2D_DESC desc;
    this->m_backBuffer->GetDesc(&desc);

    if (desc.SampleDesc.Count <= 1) {
        return this->m_backBuffer;
    }

    // TexCopy/TexStretch/ReadPixels with no draw or clear in between share one resolve.
    if (this->m_resolveTexture && this->m_resolveValid) {
        return this->m_resolveTexture;
    }

    if (!this->m_resolveTexture) {
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = 0;

        if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &this->m_resolveTexture))) {
            this->m_resolveTexture = nullptr;
            return nullptr;
        }
    }

    this->m_d3dContext->ResolveSubresource(this->m_resolveTexture, 0, this->m_backBuffer, 0, desc.Format);
    this->m_resolveValid = true;

    return this->m_resolveTexture;
}

// ---------------------------------------------------------------------------------------------------------------
// Window messages, frame

int32_t CGxDeviceD3d11::DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size:
            if (param2 == 1 || param2 == 2) {
                this->m_windowVisible = 0;
                break;
            }

            this->m_windowVisible = 1;
            this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));

            // No device loss in D3D11: resize the swap chain, rebuild the back buffer views, recreate the
            // render-target textures (D3D9 loses those on Reset, and their owners rebuild them on the callback).
            if (this->m_swapChain && this->m_context) {
                this->ITexForceRecreation(0);
                this->IReleaseBackBuffer();

                if (SUCCEEDED(this->m_swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)) && this->ICreateBackBuffer()) {
                    this->ISetWindowFocus(1);
                    this->m_intF5C = 0;
                    this->NotifyOnDeviceRestored();
                } else {
                    this->m_context = 0;
                }
            }

            this->m_viewportDirty = 1;
            break;

        case GxWM_DisplayChange:
            if (this->m_windowVisible) {
                this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_Destroy:
            this->m_windowFocus = 0;
            break;

        case GxWM_KillFocus:
            this->ISetWindowFocus(0);
            break;

        case GxWM_SetFocus:
            this->ISetWindowFocus(1);
            break;

        default:
            break;
    }

    return 0;
}

void CGxDeviceD3d11::ScenePresent() {
    if (this->m_context) {
        int32_t scrShotClick = this->m_scrShotClick;

        this->m_resolveValid = false;
        this->m_sceneDepthFresh = false;
        // CUSTOM (Badgermilk0 port): performance diagnostics (GxFrameStats)
        uint64_t frameStart = GxFrameStatsNow();
        CGxDevice::ScenePresent();
        // Software cursor (no hardware cursor in D3D11). Draws through the normal path, so it shows from phase 2 on.
        this->ICursorDraw();

        // CUSTOM (Badgermilk0 port): performance diagnostics (D3d11GpuTimer.h)
        this->m_gpuTimer.EndFrame(this->m_d3dDevice, this->m_d3dContext);

        uint64_t gpuWaitStart = GxFrameStatsNow();

        // CUSTOM (Badgermilk0 port, GxFrameLatency.h): gxMaxFrameLatency, live; 0 is the stock full wait
        if (this->m_format.m_fixLag) {
            uint32_t latency = g_gxMaxFrameLatency;

            if (latency != this->m_frameLatencyApplied) {
                IDXGIDevice1* dxgiDevice = nullptr;

                if (SUCCEEDED(this->m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDevice)))) {
                    dxgiDevice->SetMaximumFrameLatency(latency ? latency : 1);
                    dxgiDevice->Release();
                }

                this->m_frameLatencyApplied = latency;
            }

            if (!latency) {
                if (!this->m_frameLatencyQuery) {
                    D3D11_QUERY_DESC desc = { D3D11_QUERY_EVENT, 0 };
                    this->m_d3dDevice->CreateQuery(&desc, &this->m_frameLatencyQuery);
                }

                if (this->m_frameLatencyQuery) {
                    this->m_d3dContext->End(this->m_frameLatencyQuery);

                    while (this->m_d3dContext->GetData(this->m_frameLatencyQuery, nullptr, 0, 0) == S_FALSE) {
                        SwitchToThread();
                    }
                }
            }
        }

        if (scrShotClick) {
            this->DeviceScreenShot();
        }

        uint64_t capStart = GxFrameStatsNow();
        this->WaitForFPSCap();

        if (s_dumpFrame) {
            s_dumpFrame = false;
            D3d11Log("dump: end of frame, %u draws", s_dumpDraw);
        }

        CVar* dump = D3d11DumpFrameCVar();

        // N > 1 counts down one per frame, then 1 dumps the next frame as before. ClientDestroyGame sets 2 so the second
        // frame after a logout (character select) is dumped without the console.
        if (dump && dump->m_intValue > 1) {
            dump->Set(dump->m_intValue - 1, true, false, false, false);
        } else if (dump && dump->m_intValue == 1) {
            dump->Set(0, true, false, false, false);
            s_dumpFrame = true;
            s_dumpDraw = 0;
            D3d11Log("dump: next frame (vs/ps FF = fixed function; t#=CGxTex/SRV:WxH:GxTexFormat; blend=enable:src/dst; depth f=D3D11_COMPARISON w=write; aref/255 GREATEREQUAL, 0 off; ts=TextureShader tg=TexGen tgid=texgen matrix identity tc=TextureCoord; vp x,y,z)");
        }

        uint32_t vsync = this->m_format.m_vsync;
        UINT interval = vsync ? (this->m_format.m_window ? 1 : (vsync > 4 ? 4 : vsync)) : 0;
        uint64_t presentStart = GxFrameStatsNow();
        HRESULT hr = this->m_swapChain->Present(interval, 0);
        GxFrameStatsRecord(frameStart, gpuWaitStart, capStart, presentStart, GxFrameStatsNow());

        if (FAILED(hr)) {
            D3d11Log("CGxDeviceD3d11::ScenePresent(): Present failed: 0x%08X", static_cast<uint32_t>(hr));

            // 4.3.4: 0x00870310/0x1405A5E80 rebuilds the device on DEVICE_REMOVED/RESET (TDR, driver update) instead
            // of going dark for good. Our IDestroyD3d keeps the window and factory, so only the device is rebuilt.
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                this->IRecreateD3dDevice();
            } else {
                this->m_context = 0;
            }
        }

        // GBUFFER: the next frame starts with a zeroed normal target bound as RT1 again (and not as a texture).
        if (this->m_context && this->m_normalTex) {
            const float zero[4] = {};
            ID3D11RenderTargetView* rtv = this->INormalRtv();
            TexApi* api = this->m_normalTex ? static_cast<TexApi*>(this->m_normalTex->m_apiSpecificData) : nullptr;

            if (rtv) {
                this->m_d3dContext->ClearRenderTargetView(rtv, zero);
            }

            for (uint32_t i = 0; api && i < 16; i++) {
                if (this->m_curSrv[i] && this->m_curSrv[i] == api->m_srv) {
                    ID3D11ShaderResourceView* none = nullptr;
                    this->m_d3dContext->PSSetShaderResources(i, 1, &none);
                    this->m_curSrv[i] = nullptr;
                }
            }

            this->m_normalSampled = false;
            this->IOMSetTargets();
        }
    }

    this->ShaderConstantsClear();
}

// Device removed: rebuild the device. IDestroyD3d releases every shader object and marks textures and pools for
// recreation; ICreateD3dDevice recreates the shaders from the DXBC they keep in m_code.
void CGxDeviceD3d11::IRecreateD3dDevice() {
    CGxFormat format = this->m_format;
    this->IDestroyD3d();

    if (!this->ICreateD3dDevice(format)) {
        return; // m_context stays 0
    }

    this->m_intF5C = 0;
    this->NotifyOnDeviceRestored();
}

// D3D9 Clear(0, nullptr, ...) is clipped to the viewport; ClearRenderTargetView/ClearDepthStencilView are not.
// A partial viewport (model frames and portraits clear depth inside the UI) is cleared with a quad instead, as
// 4.3.4 SceneClearToViewport (0x00870400/0x1405A5F90) does. Stencil is not cleared on that path.
void CGxDeviceD3d11::SceneClear(uint32_t mask, CImVector color) {
    if (!this->m_context) {
        return;
    }

    this->m_resolveValid = false;

    if (mask & GxBufferBit_Depth) {
        this->m_sceneDepthFresh = false;
    }

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }

    // Depth and stencil only as the mask asks, as D3D9's Clear flags (a depth clear keeps the stencil).
    UINT dsFlags = (mask & GxBufferBit_Depth) ? D3D11_CLEAR_DEPTH : 0;

    if ((mask & GxBufferBit_Stencil) && this->m_caps.m_stencilBits) {
        dsFlags |= D3D11_CLEAR_STENCIL;
    }

    const CBoundingBox& vp = this->m_viewport;
    bool partial = vp.m_x.m_l > 0.0f || vp.m_y.m_l > 0.0f || vp.m_x.m_h < 1.0f || vp.m_y.m_h < 1.0f;

    if (partial && (mask & 0x3)) {
        // DECOMP: stencil clear, the whole surface (D3D9 clips it to the viewport)
        if ((dsFlags & D3D11_CLEAR_STENCIL) && this->m_curDsv) {
            this->m_d3dContext->ClearDepthStencilView(this->m_curDsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
        }

        this->ISceneClearQuad(mask, color);
        return;
    }

    if ((mask & 0x1) && this->m_curRtv) {
        const float rgba[4] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
        this->m_d3dContext->ClearRenderTargetView(this->m_curRtv, rgba);
    }

    if (dsFlags && this->m_curDsv) {
        this->m_d3dContext->ClearDepthStencilView(this->m_curDsv, dsFlags, 1.0f, 0);
    }
}

// RsSet(void*) is the only RsSet with a stock address (0x00685F50); GxRsSet exists as several inlined copies, which
// the stock-host resolver rejects. The int overload stores the same pointer-sized value.
static void* IntRs(int32_t value) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(value));
}

// 4.3.4: 0x00870400/0x1405A5F90 (SceneClearToViewport): a fixed-function quad at z = 1 over the whole viewport,
// colour and depth writes from the mask, every test off. Identity xforms: XformSetProjection maps clip z 1 to 1.0.
void CGxDeviceD3d11::ISceneClearQuad(uint32_t mask, CImVector color) {
    this->RsPush();

    this->RsSet(GxRs_PolygonOffset, IntRs(0));
    this->RsSet(GxRs_BlendingMode, IntRs(0));
    this->RsSet(GxRs_AlphaRef, IntRs(0));
    this->RsSet(GxRs_Lighting, IntRs(0));
    this->RsSet(GxRs_Fog, IntRs(0));
    this->RsSet(GxRs_DepthTest, IntRs(0));
    this->RsSet(GxRs_DepthWrite, IntRs((mask & 0x2) ? 1 : 0));
    this->RsSet(GxRs_ColorWrite, IntRs((mask & 0x1) ? 15 : 0));
    this->RsSet(GxRs_Culling, IntRs(0));
    this->RsSet(GxRs_ScissorTest, IntRs(0));
    this->RsSet(GxRs_ClipPlaneMask, IntRs(0));
    this->RsSet(GxRs_Texture0, static_cast<void*>(nullptr));
    this->RsSet(GxRs_TexGen0, IntRs(0));
    this->RsSet(GxRs_VertexShader, static_cast<void*>(nullptr));
    this->RsSet(GxRs_PixelShader, static_cast<void*>(nullptr));

    C44Matrix identity;
    GxXformPush(GxXform_World, identity);
    GxXformPush(GxXform_View, identity);
    C44Matrix projection;
    GxXformProjection(projection);
    GxXformSetProjection(identity);

    CGxBuf* buf = GxBufStream(GxPoolTarget_Vertex, 16, 4);
    uint32_t* vertices = reinterpret_cast<uint32_t*>(GxBufLock(buf));

    if (vertices) {
        static const float s_corners[4][2] = { { -1.0f, 1.0f }, { -1.0f, -1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f } };

        for (uint32_t i = 0; i < 4; i++) {
            float* v = reinterpret_cast<float*>(&vertices[i * 4]);
            v[0] = s_corners[i][0];
            v[1] = s_corners[i][1];
            v[2] = 1.0f;
            memcpy(&vertices[i * 4 + 3], &color, 4);
        }

        GxBufUnlock(buf, 0);
        GxPrimVertexPtr(buf, GxVBF_PC);

        CGxBatch batch;
        batch.m_primType = GxPrim_TriangleStrip;
        batch.m_start = 0;
        batch.m_count = 4;
        batch.m_minIndex = 0;
        batch.m_maxIndex = 3;
        GxDraw(&batch, 0);
    }

    GxXformSetProjection(projection);
    GxXformPop(GxXform_View);
    GxXformPop(GxXform_World);
    this->RsPop();
}

// Same as CGxDeviceD3d::IXformSetViewport.
void CGxDeviceD3d11::IXformSetViewport() {
    const CRect& window = this->DeviceCurWindow();

    float x = floorf(this->m_viewport.m_x.m_l * window.maxX + 0.5f);
    float y = floorf((1.0f - this->m_viewport.m_y.m_h) * window.maxY + 0.5f);

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = x;
    viewport.TopLeftY = y;
    viewport.Width = floorf(this->m_viewport.m_x.m_h * window.maxX - x + 0.5f);
    viewport.Height = floorf((1.0f - this->m_viewport.m_y.m_l) * window.maxY - y + 0.5f);
    viewport.MinDepth = this->m_viewport.m_z.m_l;
    viewport.MaxDepth = this->m_viewport.m_z.m_h;

    if (this->m_textureTarget[0].m_apiSpecific || this->m_textureTarget[1].m_apiSpecific) {
        viewport.TopLeftY = floorf(window.maxY * this->m_viewport.m_y.m_l);
    }

    this->m_d3dContext->RSSetViewports(1, &viewport);
    this->m_viewportDirty = 0;
}

// Same as CGxDeviceD3d::XformSetProjection: D3D11 clip space has the same 0..1 depth range as D3D9.
void CGxDeviceD3d11::XformSetProjection(const C44Matrix& matrix) {
    CGxDevice::XformSetProjection(matrix);

    C44Matrix projNative = matrix;

    if (!(fabs(projNative.c3 - 1.0f) < 0.00000023841858f) && !(fabs(projNative.c3) < 0.00000023841858f)) {
        projNative *= 1.0f / projNative.c3;
    }

    if (projNative.d3 == 0.0f) {
        float minZ = -(projNative.d2 / (projNative.c2 + 1.0f));
        float maxZ = -(projNative.d2 / (projNative.c2 - 1.0f));
        projNative.c2 = maxZ / (maxZ - minZ);
        projNative.d2 = maxZ * minZ / (minZ - maxZ);
    } else {
        float invC2 = 1.0f / projNative.c2;
        float minZ = (-1.0f - projNative.d2) * invC2;
        float maxZ = (1.0f - projNative.d2) * invC2;
        projNative.c2 = 1.0f / (maxZ - minZ);
        projNative.d2 = minZ / (minZ - maxZ);
    }

    if (!(this->m_appMasterEnables & (1 << GxMasterEnable_NormalProjection)) && projNative.d3 != 1.0f) {
        C44Matrix scale;
        scale.a0 = 0.2f;
        scale.b1 = 0.2f;
        scale.c2 = 0.2f;
        projNative = projNative * scale;
    }

    this->m_xforms[GxXform_Projection].m_dirty = 1;
    this->m_projNative = projNative;
}

// ---------------------------------------------------------------------------------------------------------------
// Buffer pools

int32_t CGxDeviceD3d11::CreatePoolAPI(CGxPool* pool) {
    if (!this->m_d3dDevice || pool->m_size <= 0) {
        return 0;
    }

    D3D11_BUFFER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.ByteWidth = pool->m_size;
    desc.BindFlags = pool->m_target == GxPoolTarget_Vertex ? D3D11_BIND_VERTEX_BUFFER : D3D11_BIND_INDEX_BUFFER;

    if (pool->m_usage == GxPoolUsage_Static) {
        desc.Usage = D3D11_USAGE_DEFAULT;
    } else {
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    }

    ID3D11Buffer* buffer = nullptr;

    if (FAILED(this->m_d3dDevice->CreateBuffer(&desc, nullptr, &buffer))) {
        return 0;
    }

    PoolApi* api = new PoolApi;
    api->m_buffer = buffer;
    // ponytail: a full CPU copy per pool (double memory for static geometry); drop it for Static pools if the
    // 32-bit address space gets tight, at the cost of indexed-fan rewriting from static index pools.
    api->m_shadow = static_cast<char*>(malloc(pool->m_size));
    api->m_discard = 0;

    if (!api->m_shadow) {
        buffer->Release();
        delete api;
        return 0;
    }

    pool->m_apiSpecific = api;

    return 1;
}

void CGxDeviceD3d11::IReleaseD3dPool(CGxPool* pool) {
    if (pool->m_usage == GxPoolUsage_Stream) {
        pool->m_offset = 0;
    }

    pool->Invalidate();

    PoolApi* api = static_cast<PoolApi*>(pool->m_apiSpecific);

    if (api) {
        api->m_buffer->Release();
        free(api->m_shadow);
        delete api;
        pool->m_apiSpecific = nullptr;
    }
}

void CGxDeviceD3d11::IReleaseD3dPools(int32_t freePools) {
    for (CGxPool* pool = this->m_poolList.Head(); pool; pool = this->m_poolList.Next(pool)) {
        if (freePools || pool->m_usage == GxPoolUsage_Stream || pool->m_usage == GxPoolUsage_Dynamic) {
            this->IReleaseD3dPool(pool);
        }
    }
}

void CGxDeviceD3d11::PoolSizeSet(CGxPool* pool, uint32_t size) {
    this->IReleaseD3dPool(pool);
    pool->m_size = size;
    this->CreatePoolAPI(pool);
}

void CGxDeviceD3d11::PoolDestroy(CGxPool* pool) {
    if (pool) {
        this->IReleaseD3dPool(pool);
        CGxDevice::PoolDestroy(pool);
    }
}

// Same stream-pool ring logic as CGxDeviceD3d::IBufLock; the pointer is into the pool's CPU copy.
char* CGxDeviceD3d11::IBufLock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;

    if (!this->m_context) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    if (!pool->m_apiSpecific) {
        this->CreatePoolAPI(pool);
    }

    PoolApi* api = static_cast<PoolApi*>(pool->m_apiSpecific);

    if (!api) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    if (pool->m_usage == GxPoolUsage_Stream) {
        uint32_t index = (buf->m_itemSize + pool->m_offset - 1) - (buf->m_itemSize + pool->m_offset - 1) % buf->m_itemSize;

        if (index + buf->m_size > static_cast<uint32_t>(pool->m_size)) {
            api->m_discard = 1;
            pool->Discard();
            buf->m_index = 0;
            pool->m_offset = buf->m_size;
        } else {
            buf->m_index = index;
            pool->m_offset = index + buf->m_size;
        }
    }

    return api->m_shadow + buf->m_index;
}

void CGxDeviceD3d11::IBufUnlock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;
    EmergencyMem& emergencyMem = this->m_emergencyMem[pool->m_target];

    if (emergencyMem.m_lock) {
        emergencyMem.Unlock();
        buf->m_valid = 0;
        this->m_intF5C = 1;
        return;
    }

    PoolApi* api = static_cast<PoolApi*>(pool->m_apiSpecific);

    if (!api || !buf->m_size) {
        buf->m_valid = api != nullptr;
        return;
    }

    const char* src = api->m_shadow + buf->m_index;

    if (pool->m_usage == GxPoolUsage_Static) {
        D3D11_BOX box = { buf->m_index, 0, 0, buf->m_index + buf->m_size, 1, 1 };
        this->m_d3dContext->UpdateSubresource(api->m_buffer, 0, &box, src, 0, 0);
        buf->m_valid = 1;
        return;
    }

    // Stream pools discard when the ring wraps (everything else in them is dead then); otherwise no-overwrite, as
    // D3D9 locked them. Only the locked range is copied.
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_MAP mapType = api->m_discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
    HRESULT hr = this->m_d3dContext->Map(api->m_buffer, 0, mapType, 0, &mapped);

    if (SUCCEEDED(hr)) {
        memcpy(static_cast<char*>(mapped.pData) + buf->m_index, src, buf->m_size);
        this->m_d3dContext->Unmap(api->m_buffer, 0);
        api->m_discard = 0;
    }

    buf->m_valid = SUCCEEDED(hr);
}

char* CGxDeviceD3d11::BufLock(CGxBuf* buf) {
    CGxDevice::BufLock(buf);

    return this->IBufLock(buf);
}

int32_t CGxDeviceD3d11::BufUnlock(CGxBuf* buf, uint32_t size) {
    CGxDevice::BufUnlock(buf, size);
    this->IBufUnlock(buf);

    return 1;
}

void CGxDeviceD3d11::BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset) {
    CGxDevice::BufData(buf, data, size, offset);

    char* dst = this->IBufLock(buf);
    memcpy(dst + offset, data, size);
    this->IBufUnlock(buf);
}

// ---------------------------------------------------------------------------------------------------------------
// Draw

void CGxDeviceD3d11::ISetVertexBuffer(uint32_t stream, CGxBuf* buf, uint32_t stride) {
    PoolApi* api = buf ? static_cast<PoolApi*>(buf->m_pool->m_apiSpecific) : nullptr;
    ID3D11Buffer* buffer = api ? api->m_buffer : nullptr;
    uint32_t offset = buf ? buf->m_index : 0;

    if (this->m_curVertexBuf[stream] != buffer || this->m_curVertexOfs[stream] != offset || this->m_curVertexStride[stream] != stride) {
        UINT strides = stride;
        UINT offsets = offset;
        this->m_d3dContext->IASetVertexBuffers(stream, 1, &buffer, &strides, &offsets);
        this->m_curVertexBuf[stream] = buffer;
        this->m_curVertexOfs[stream] = offset;
        this->m_curVertexStride[stream] = stride;
    }
}

// Stream grouping and strides as CGxDeviceD3d::IStateSyncVertexPtrs. The matching input layout is phase 2.
void CGxDeviceD3d11::IStateSyncVertexPtrs() {
    if (this->m_primVertexFormat < GxVertexBufferFormats_Last) {
        this->ISetVertexBuffer(0, this->m_primVertexBuf, this->m_primVertexSize);
        return;
    }

    CGxBuf* streamBufs[GxVAs_Last] = {};
    uint32_t streamStrides[GxVAs_Last] = {};
    uint32_t streamCount = 0;

    for (uint32_t i = 0; i < GxVAs_Last; i++) {
        if (!(this->m_primVertexMask & (1 << i))) {
            continue;
        }

        uint32_t stream;

        for (stream = 0; stream < streamCount; stream++) {
            if (streamBufs[stream] == this->m_primVertexFormatBuf[i]) {
                break;
            }
        }

        if (stream == streamCount) {
            streamBufs[streamCount++] = this->m_primVertexFormatBuf[i];
        }

        streamStrides[stream] += s_gxAttribToD3dAttribSize[this->m_primVertexFormatAttrib[i].m_type];
    }

    for (uint32_t stream = 0; stream < streamCount && stream < 8; stream++) {
        this->ISetVertexBuffer(stream, streamBufs[stream], streamStrides[stream]);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Pipeline state (phase 2) and fixed function (FfShaders.h, /root/d3d11-port/ffshaders)

// D3D9 s_srcBlend / s_dstBlend per GxBlend mode. Alpha uses the same factors, colour ones swapped for alpha ones
// (D3D11 rejects colour factors in the alpha slots).
static const D3D11_BLEND s_srcBlend11[12] = { D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_BLEND_FACTOR };
static const D3D11_BLEND s_dstBlend11[12] = { D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_INV_BLEND_FACTOR };
static const D3D11_COMPARISON_FUNC s_cmpFunc11[4] = { D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_LESS };
// D3D9 NONE / CW / CCW, with clockwise front faces (FrontCounterClockwise = FALSE).
static const D3D11_CULL_MODE s_cullMode11[3] = { D3D11_CULL_NONE, D3D11_CULL_FRONT, D3D11_CULL_BACK };
// Index = CGxVertexAttrib::m_type (D3D9 s_gxAttribToD3dAttribType), formats from the shader-xlate README.
static const DXGI_FORMAT s_gxAttribToDxgiFormat[7] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R32_FLOAT };
static const char* s_gxAttribSemantic[GxVAs_Last] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "COLOR", "COLOR", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD", "TEXCOORD" };
static const UINT s_gxAttribSemanticIndex[GxVAs_Last] = { 0, 0, 0, 0, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7 };
// D3D9 s_filterModes; Nearest/Linear have no mip filter there, so they get MaxLOD 0 here.
static const D3D11_FILTER s_filter11[GxTexFilters_Last] = { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FILTER_ANISOTROPIC };

static D3D11_BLEND AlphaBlend11(D3D11_BLEND blend) {
    switch (blend) {
        case D3D11_BLEND_SRC_COLOR:
            return D3D11_BLEND_SRC_ALPHA;
        case D3D11_BLEND_INV_SRC_COLOR:
            return D3D11_BLEND_INV_SRC_ALPHA;
        case D3D11_BLEND_DEST_COLOR:
            return D3D11_BLEND_DEST_ALPHA;
        case D3D11_BLEND_INV_DEST_COLOR:
            return D3D11_BLEND_INV_DEST_ALPHA;
        default:
            return blend;
    }
}

static uint64_t Fnv64(const void* data, size_t size, uint64_t hash = 0xCBF29CE484222325ull) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    }

    return hash;
}

// A DXBC chunk's body, or nullptr.
static const uint8_t* DxbcChunk(const void* code, uint32_t size, const char* tag, uint32_t& chunkSize) {
    const uint8_t* bytes = static_cast<const uint8_t*>(code);

    if (bytes && size >= 32 && !memcmp(bytes, "DXBC", 4)) {
        uint32_t chunks;
        memcpy(&chunks, bytes + 28, 4);

        for (uint32_t i = 0; i < chunks && 32 + i * 4 + 4 <= size; i++) {
            uint32_t offset;
            memcpy(&offset, bytes + 32 + i * 4, 4);

            if (offset + 8 <= size && !memcmp(bytes + offset, tag, 4)) {
                memcpy(&chunkSize, bytes + offset + 4, 4);

                if (offset + 8 + chunkSize <= size) {
                    return bytes + offset + 8;
                }
            }
        }
    }

    return nullptr;
}

// Hash of the DXBC input signature (ISGN chunk), so input layouts are shared by every VS with the same inputs.
static uint64_t DxbcInputSignatureHash(const void* code, uint32_t size) {
    uint32_t chunkSize;
    const uint8_t* isgn = DxbcChunk(code, size, "ISGN", chunkSize);
    return isgn ? Fnv64(isgn, chunkSize) : Fnv64(code, size);
}

// GBUFFER: the pixel shader's output signature (OSGN) has SV_Target1 (normals.py's oC1, sm3hlsl.py).
static bool DxbcWritesTarget1(const void* code, uint32_t size) {
    uint32_t chunkSize;
    const uint8_t* osgn = DxbcChunk(code, size, "OSGN", chunkSize);

    if (!osgn || chunkSize < 8) {
        return false;
    }

    uint32_t count;
    memcpy(&count, osgn, 4);

    for (uint32_t i = 0; i < count && 8 + (i + 1) * 24 <= chunkSize; i++) {
        uint32_t element[6];
        memcpy(element, osgn + 8 + i * 24, sizeof(element));

        // name offset, semantic index, system value, component type, register, mask
        if (element[4] == 1 && element[0] + 9 <= chunkSize && !memcmp(osgn + element[0], "SV_Target", 9)) {
            return true;
        }
    }

    return false;
}

// Bytes of b0 the shader declares (dcl_constantbuffer cb0[n] in SHDR/SHEX; shader-xlate's cbuffers()), so
// IShaderConstantsFlush maps no more than it reads. Not found: all 4 KB.
static uint32_t DxbcCb0Bytes(const void* code, uint32_t size) {
    constexpr uint32_t kAll = 256 * 16;
    uint32_t chunkSize = 0;
    const uint8_t* shdr = DxbcChunk(code, size, "SHDR", chunkSize);

    if (!shdr) {
        shdr = DxbcChunk(code, size, "SHEX", chunkSize);
    }

    if (!shdr) {
        return kAll;
    }

    uint32_t count = chunkSize / 4;
    auto tok = [shdr](uint32_t i) { uint32_t t; memcpy(&t, shdr + i * 4, 4); return t; };

    for (uint32_t i = 2; i < count;) {
        uint32_t t = tok(i);
        uint32_t op = t & 0x7FF;
        uint32_t length = (t >> 24) & 0x7F;

        if (op == 0x35 && length == 0 && i + 1 < count) {   // customdata (immediate constant buffer)
            length = tok(i + 1);
        }

        if (op == 0x59 && i + 3 < count && tok(i + 2) == 0) {   // dcl_constantbuffer cb0[n]
            uint32_t bytes = tok(i + 3) * 16;
            return bytes && bytes < kAll ? bytes : kAll;
        }

        if (!length) {
            break;
        }

        i += length;
    }

    return kAll;
}

static void ColorToFloat4(uint32_t value, float* out, float alpha = -1.0f) {
    CImVector color;
    memcpy(&color, &value, sizeof(color));
    out[0] = color.r * (1.0f / 255.0f);
    out[1] = color.g * (1.0f / 255.0f);
    out[2] = color.b * (1.0f / 255.0f);
    out[3] = alpha < 0.0f ? color.a * (1.0f / 255.0f) : alpha;
}

// The client's own texLodBias setting (WorldParam), applied in the samplers: the D3D9 path ignores it in 12340.
static float TexLodBias() {
    static CVar* cvar;

    if (!cvar) {
        cvar = CVar::Lookup("texLodBias");
    }

    return cvar ? cvar->m_floatValue : 0.0f;
}


// Live bisect switch for world-look differences against D3D9, read every draw.
static int32_t D3d11Debug() {
    static CVar* cvar;

    if (!cvar) {
        cvar = CVar::Register("d3d11Debug", "D3D11: 1 constants every draw, 2 skip SM2 draws, 3 sun occlusion 0, 4 unset constants 0, 5 FFX D3D9 half-texel UVs, 6 no TEXCOORD2/3 FF VS", 0, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
        D3d11Log("CGxDeviceD3d11: d3d11Debug modes: 0 off, 1 re-upload both constant buffers every draw, 2 skip draws with an SM2 (ps_2_0/vs_2_0: FFX, weather) shader, 3 occlusion queries return 0 pixels (no sun glare dimming), 4 never-set constant registers read 0 instead of FLT_MAX, 5 FFX quads use the D3D9 +0.5 texel UV offset, 6 FF VS drops TEXCOORD2/3 (pre-25fe6c1)");
    }

    return cvar ? cvar->m_intValue : 0;
}

// SM2 families of the pack (shader-xlate ba20c00): every one before that fell back to fixed function.
static bool IsSm2Family(const char* name) {
    static const char* s_names[] = { "FFXBox4", "FFXComposite", "FFXDeath", "FFXFogCombine", "FFXGauss4", "FFXGlow", "FFXGlowWave", "FFXNetherBlur", "FFXNetherCombine", "FFXPropagateFog", "FFXQuad", "patter", "rain", "sand", "snowpoint" };

    for (const char* known : s_names) {
        if (!SStrCmpI(name, known, STORM_MAX_STR)) {
            return true;
        }
    }

    return false;
}

// D3D9 defaults (IStateSetD3DDefaults) and every render state re-sent, after device creation.
void CGxDeviceD3d11::IStateReset() {
    memset(&this->m_rsDesc, 0, sizeof(this->m_rsDesc));
    this->m_rsDesc.FillMode = D3D11_FILL_SOLID;
    this->m_rsDesc.CullMode = D3D11_CULL_NONE;
    this->m_rsDesc.DepthClipEnable = TRUE;

    memset(&this->m_blendDesc, 0, sizeof(this->m_blendDesc));
    D3D11_RENDER_TARGET_BLEND_DESC& rt = this->m_blendDesc.RenderTarget[0];
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_ZERO;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    // GBUFFER: RT1 (the normal target) never blends; its mask is set per draw in IStateSync.
    this->m_blendDesc.IndependentBlendEnable = TRUE;
    this->m_blendDesc.RenderTarget[1] = rt;
    this->m_blendDesc.RenderTarget[1].RenderTargetWriteMask = 0;

    memset(&this->m_dsDesc, 0, sizeof(this->m_dsDesc));
    this->m_dsDesc.DepthEnable = TRUE;
    this->m_dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    this->m_dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    this->m_dsDesc.StencilReadMask = 0xFF;
    this->m_dsDesc.StencilWriteMask = 0xFF;
    D3D11_DEPTH_STENCILOP_DESC stencilOp = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
    this->m_dsDesc.FrontFace = stencilOp;
    this->m_dsDesc.BackFace = stencilOp;

    for (uint32_t i = 0; i < 4; i++) {
        this->m_blendFactor[i] = 1.0f;
    }

    this->m_stateDirty = Dirty_Raster | Dirty_Blend | Dirty_Depth;
    this->m_samplerDirty = 0xFFFF;
    memset(this->m_curSrv, 0, sizeof(this->m_curSrv));
    memset(this->m_curCb, 0, sizeof(this->m_curCb));
    this->m_curVs = nullptr;
    this->m_curPs = nullptr;
    this->m_curVsCode = nullptr;
    this->m_curVsCodeSize = 0;
    this->m_curVsSig = 0;
    this->m_curLayoutKey = 0;
    // NaN never compares equal, so the first draw uploads b1 and the FF blocks.
    memset(this->m_devConstants, 0xFF, sizeof(this->m_devConstants));
    memset(this->m_ffConstants, 0xFF, sizeof(this->m_ffConstants));
    this->m_hwMasterEnables = ~this->m_appMasterEnables;

    this->IRsForceUpdate();
}

void CGxDeviceD3d11::IStateRelease() {
    for (auto& entry : this->m_stateCache) {
        SafeRelease(entry.second);
    }

    this->m_stateCache.clear();

    for (uint32_t i = 0; i < sizeof(this->m_ffVs) / sizeof(this->m_ffVs[0]); i++) {
        SafeRelease(this->m_ffVs[i]);
    }

    for (uint32_t i = 0; i < 3; i++) {
        SafeRelease(this->m_ffPs[i]);
    }

    SafeRelease(this->m_ffConstantBuffers[0]);
    SafeRelease(this->m_ffConstantBuffers[1]);
    SafeRelease(this->m_devConstantBuffer);
    SafeRelease(this->m_blitVs);
    SafeRelease(this->m_blitPs);
    SafeRelease(this->m_blitDepthPs[0]);
    SafeRelease(this->m_blitDepthPs[1]);
    SafeRelease(this->m_blitConstantBuffer);
}

// ponytail: 64-bit FNV keys without a desc compare; a collision would hand back the wrong state object. Store and
// memcmp the desc if that ever shows up.
ID3D11DeviceChild* CGxDeviceD3d11::IStateGet(uint32_t kind, const void* desc, uint32_t size) {
    uint64_t key = Fnv64(desc, size, Fnv64(&kind, sizeof(kind)));
    auto found = this->m_stateCache.find(key);

    if (found != this->m_stateCache.end()) {
        return found->second;
    }

    ID3D11DeviceChild* object = nullptr;
    HRESULT hr = E_FAIL;

    switch (kind) {
        case State_Raster:
            hr = this->m_d3dDevice->CreateRasterizerState(static_cast<const D3D11_RASTERIZER_DESC*>(desc), reinterpret_cast<ID3D11RasterizerState**>(&object));
            break;

        case State_Blend:
            hr = this->m_d3dDevice->CreateBlendState(static_cast<const D3D11_BLEND_DESC*>(desc), reinterpret_cast<ID3D11BlendState**>(&object));
            break;

        case State_Depth:
            hr = this->m_d3dDevice->CreateDepthStencilState(static_cast<const D3D11_DEPTH_STENCIL_DESC*>(desc), reinterpret_cast<ID3D11DepthStencilState**>(&object));
            break;

        case State_Sampler:
            hr = this->m_d3dDevice->CreateSamplerState(static_cast<const D3D11_SAMPLER_DESC*>(desc), reinterpret_cast<ID3D11SamplerState**>(&object));
            break;

        default:
            break;
    }

    if (FAILED(hr)) {
        D3d11Log("CGxDeviceD3d11: state object %u creation failed (0x%08X)", kind, static_cast<uint32_t>(hr));
        object = nullptr;
    }

    this->m_stateCache[key] = object;

    return object;
}

// Appends a slot-15 element (zero buffer, stride 0) for every ISGN input of the VS that no element supplies.
// ISGN: count, 8, then 24-byte entries (name offset, semantic index, system value, component type, register, masks).
static uint32_t AddMissingVsInputs(const void* code, uint32_t size, D3D11_INPUT_ELEMENT_DESC* elements, uint32_t count, uint32_t capacity) {
    static const DXGI_FORMAT s_zeroFormats[4] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_FLOAT };
    const uint8_t* bytes = static_cast<const uint8_t*>(code);

    if (size < 32 || memcmp(bytes, "DXBC", 4)) {
        return count;
    }

    uint32_t chunks;
    memcpy(&chunks, bytes + 28, 4);

    for (uint32_t c = 0; c < chunks && 32 + c * 4 + 4 <= size; c++) {
        uint32_t offset;
        memcpy(&offset, bytes + 32 + c * 4, 4);

        if (offset + 16 > size || memcmp(bytes + offset, "ISGN", 4)) {
            continue;
        }

        uint32_t chunkSize;
        memcpy(&chunkSize, bytes + offset + 4, 4);
        const uint8_t* sig = bytes + offset + 8;

        if (offset + 8 + chunkSize > size) {
            return count;
        }

        uint32_t inputs;
        memcpy(&inputs, sig, 4);

        for (uint32_t i = 0; i < inputs && 8 + i * 24 + 24 <= chunkSize && count < capacity; i++) {
            uint32_t entry[4];
            memcpy(entry, sig + 8 + i * 24, sizeof(entry));

            // System values (SV_VertexID and friends) come from the IA, not a buffer.
            if (entry[2] != 0 || entry[0] >= chunkSize) {
                continue;
            }

            const char* name = reinterpret_cast<const char*>(sig + entry[0]);

            if (!memchr(name, 0, chunkSize - entry[0])) {
                continue;
            }

            bool found = false;

            for (uint32_t e = 0; e < count && !found; e++) {
                found = elements[e].SemanticIndex == entry[1] && !_stricmp(elements[e].SemanticName, name);
            }

            if (!found) {
                D3D11_INPUT_ELEMENT_DESC& element = elements[count++];
                element.SemanticName = name;
                element.SemanticIndex = entry[1];
                element.Format = s_zeroFormats[entry[3] & 3];
                element.InputSlot = 15;
                element.AlignedByteOffset = 0;
                element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                element.InstanceDataStepRate = 0;
            }
        }

        return count;
    }

    return count;
}

// Same element list as CGxDeviceD3d::IStateSyncVertexPtrs builds for its vertex declaration (predefined formats use
// stream 0 only), named by the shader-xlate semantic table, hashed with the bound VS input signature.
void CGxDeviceD3d11::IStateSyncInputLayout() {
    D3D11_INPUT_ELEMENT_DESC elements[GxVAs_Last + 16];
    CGxBuf* streamBufs[GxVAs_Last] = {};
    uint32_t streamCount = 0;
    uint32_t count = 0;
    uint64_t key = Fnv64(&this->m_curVsSig, sizeof(this->m_curVsSig));
    bool predefined = this->m_primVertexFormat < GxVertexBufferFormats_Last;

    for (uint32_t i = 0; i < GxVAs_Last; i++) {
        if (!(this->m_primVertexMask & (1 << i))) {
            continue;
        }

        uint32_t stream = 0;

        if (!predefined) {
            for (stream = 0; stream < streamCount; stream++) {
                if (streamBufs[stream] == this->m_primVertexFormatBuf[i]) {
                    break;
                }
            }

            if (stream == streamCount) {
                streamBufs[streamCount++] = this->m_primVertexFormatBuf[i];
            }
        }

        const CGxVertexAttrib& attrib = this->m_primVertexFormatAttrib[i];
        D3D11_INPUT_ELEMENT_DESC& element = elements[count++];
        element.SemanticName = s_gxAttribSemantic[i];
        element.SemanticIndex = s_gxAttribSemanticIndex[i];
        element.Format = s_gxAttribToDxgiFormat[attrib.m_type < 7 ? attrib.m_type : 0];
        element.InputSlot = stream;
        element.AlignedByteOffset = attrib.m_offset;
        element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        element.InstanceDataStepRate = 0;

        uint32_t words[4] = { i, attrib.m_type, attrib.m_offset, stream };
        key = Fnv64(words, sizeof(words), key);
    }

    if (key == this->m_curLayoutKey) {
        return;
    }

    this->m_curLayoutKey = key;
    uint64_t cacheKey = Fnv64(&key, sizeof(key), State_InputLayout);
    auto found = this->m_stateCache.find(cacheKey);
    ID3D11InputLayout* layout = nullptr;

    if (found != this->m_stateCache.end()) {
        layout = static_cast<ID3D11InputLayout*>(found->second);
    } else {
        // VS inputs the decl lacks read the zero buffer at slot 15 (D3D9 fed them defaults).
        if (this->m_curVsCode) {
            count = AddMissingVsInputs(this->m_curVsCode, this->m_curVsCodeSize, elements, count, sizeof(elements) / sizeof(elements[0]));
        }

        // Anything still failing is cached as null (the draw is dropped).
        if (!this->m_curVsCode || FAILED(this->m_d3dDevice->CreateInputLayout(elements, count, this->m_curVsCode, this->m_curVsCodeSize, &layout))) {
            D3d11Log("CGxDeviceD3d11: CreateInputLayout failed (mask 0x%X)", this->m_primVertexMask);
            layout = nullptr;
        }

        this->m_stateCache[cacheKey] = layout;
    }

    this->m_d3dContext->IASetInputLayout(layout);
}

// D3D9 semantics: copy the dirty range into the mirror, then send the mirror. A DISCARD map leaves the rest of b0
// undefined, so only the part the bound shader declares is sent (m_cbBytes; FF reads its own buffers: 0), and a
// later shader that declares more than was sent re-sends. d3d11Debug 1 re-sends every draw; 4 turns FLT_MAX (written by ShaderConstantsClear, never by the app) into 0.
void CGxDeviceD3d11::IShaderConstantsFlush() {
    int32_t debug = D3d11Debug();

    for (uint32_t stage = 0; stage < 2; stage++) {
        ShadowConstants& shadow = CGxDevice::s_shadowConstants[stage == 0 ? 1 : 0];
        float* mirror = this->m_gpuConstants[stage];
        bool dirty = shadow.dirtyMin <= shadow.dirtyMax && shadow.dirtyMax < 256;

        if (dirty) {
            for (uint32_t i = shadow.dirtyMin * 4; i < (shadow.dirtyMax + 1) * 4; i++) {
                mirror[i] = debug == 4 && shadow.constants[i] == FLT_MAX ? 0.0f : shadow.constants[i];
            }
        }

        uint32_t bytes = this->m_cbBytes[stage];

        if (dirty) {
            this->m_cbSent[stage] = 0;
        }

        if (bytes > this->m_cbSent[stage] || (debug == 1 && bytes)) {
            UploadConstants(this->m_d3dContext, this->m_constantBuffers[stage], mirror, bytes);
            this->m_cbSent[stage] = bytes;
        }

        shadow.dirtyMin = 255;
        shadow.dirtyMax = 0;
    }
}

// cb1: int constants (none: stock shaders only use defi), alpha-test ref, user clip planes in clip space. D3D9 takes
// planes in clip space for shader draws and in world space for fixed function, so FF planes go through inv(view*proj).
void CGxDeviceD3d11::IStateSyncDeviceConstants(bool ffVs) {
    float dev[23 * 4];
    memset(dev, 0, sizeof(dev));

    int32_t alphaRef = this->m_appRenderStates[GxRs_AlphaRef].m_value.m_data.i[0];
    dev[16 * 4] = alphaRef > 0 ? alphaRef * (1.0f / 255.0f) : 0.0f;

    uint32_t mask = this->m_appRenderStates[GxRs_ClipPlaneMask].m_value.m_data.u[0] & 0x3F;

    if (mask) {
        C44Matrix toClip;

        if (ffVs) {
            CGxMatrixStack& view = this->m_xforms[GxXform_View];
            toClip = (view.m_mtx[view.m_level] * this->m_projNative).Inverse().Transpose();
        }

        for (uint32_t i = 0; i < 6; i++) {
            if (!(mask & (1 << i))) {
                continue;
            }

            const C4Plane& plane = this->m_clipPlane[i];
            C4Vector p;
            p.x = plane.n.x;
            p.y = plane.n.y;
            p.z = plane.n.z;
            p.w = plane.d;

            if (ffVs) {
                p = p * toClip;
            }

            memcpy(&dev[(17 + i) * 4], &p, sizeof(p));
        }
    }

    this->m_clipPlaneMask = 0;

    if (memcmp(dev, this->m_devConstants, sizeof(dev))) {
        memcpy(this->m_devConstants, dev, sizeof(dev));
        UploadConstants(this->m_d3dContext, this->m_devConstantBuffer, dev, sizeof(dev));
    }
}

// The FF register maps of ffshaders/ff_common.hlsli, filled as CGxDeviceD3d's IStateSyncLights / Material / Xforms
// and IXformSetTex would program D3D9.
void CGxDeviceD3d11::IStateSyncFixedFunction(bool vs, bool ps, bool lighting, bool ambientOnly) {
    const CGxAppRenderState* rs = this->m_appRenderStates.m_data;
    bool fog = rs[GxRs_Fog].m_value.m_data.i[0] && (this->m_appMasterEnables & 0x2);
    bool specular = rs[GxRs_MatSpecularExp].m_value.m_data.f[0] > 0.0f && rs[GxRs_MatSpecular].m_value.m_data.i[0];

    if (vs) {
        float c[53 * 4];
        memset(c, 0, sizeof(c));

        CGxMatrixStack& world = this->m_xforms[GxXform_World];
        CGxMatrixStack& view = this->m_xforms[GxXform_View];
        const C44Matrix& viewMtx = view.m_mtx[view.m_level];
        C44Matrix worldView = world.m_mtx[world.m_level] * viewMtx;
        memcpy(&c[0 * 4], &worldView, sizeof(C44Matrix));
        memcpy(&c[4 * 4], &this->m_projNative, sizeof(C44Matrix));

        for (uint32_t tmu = 0; tmu < 2; tmu++) {
            CGxMatrixStack& texGen = this->m_texGen[tmu];
            CGxMatrixStack& xform = this->m_xforms[tmu];
            int32_t gen = rs[GxRs_TexGen0 + tmu].m_value.m_data.i[0];
            C44Matrix matrix = texGen.m_mtx[texGen.m_level];
            float mode = 0.0f;

            switch (rs[GxRs_TextureShader0 + tmu].m_value.m_data.i[0]) {
                case 0:
                    mode = (texGen.m_flags[texGen.m_level] & CGxMatrixStack::F_Identity) ? 0.0f : 1.0f;
                    break;

                case 1: {
                    C44Matrix product = texGen.m_mtx[texGen.m_level] * xform.m_mtx[xform.m_level];
                    matrix = product;

                    if (!gen) {
                        matrix.c0 = product.d0;
                        matrix.c1 = product.d1;
                    }

                    mode = 1.0f;
                    break;
                }

                case 2:
                    matrix = texGen.m_mtx[texGen.m_level] * xform.m_mtx[xform.m_level];
                    mode = 2.0f;
                    break;

                default:
                    break;
            }

            float source;

            switch (gen) {
                case GxTexGen_ObjectLinear:
                case GxTexGen_World:
                case GxTexGen_View:
                    source = 8.0f;
                    break;

                case GxTexGen_ReflectionMap:
                case GxTexGen_SphereMap:
                    source = 10.0f;
                    break;

                case GxTexGen_Normal:
                    source = 9.0f;
                    break;

                default:
                    source = static_cast<float>(rs[GxRs_TextureCoord0 + tmu].m_value.m_data.u[0] & 7);
                    break;
            }

            memcpy(&c[(8 + tmu * 4) * 4], &matrix, sizeof(C44Matrix));
            c[(21 + tmu) * 4 + 0] = source;
            c[(21 + tmu) * 4 + 1] = mode;
        }

        // Material (ISetMaterial: ambient = diffuse).
        ColorToFloat4(rs[GxRs_MatEmissive].m_value.m_data.u[0], &c[16 * 4]);
        ColorToFloat4(rs[GxRs_MatDiffuse].m_value.m_data.u[0], &c[17 * 4]);
        ColorToFloat4(rs[GxRs_MatSpecular].m_value.m_data.u[0], &c[18 * 4], rs[GxRs_MatSpecularExp].m_value.m_data.f[0]);

        if (ambientOnly) {
            c[19 * 4 + 0] = c[19 * 4 + 1] = c[19 * 4 + 2] = c[19 * 4 + 3] = 1.0f;
        }

        if (fog) {
            float start = rs[GxRs_FogStart].m_value.m_data.f[0];
            float end = rs[GxRs_FogEnd].m_value.m_data.f[0];
            float range = end - start != 0.0f ? end - start : 1.0f;
            c[20 * 4 + 0] = -1.0f / range;
            c[20 * 4 + 1] = end / range;
        } else {
            c[20 * 4 + 1] = 1.0f;
        }

        if (this->m_primVertexMask & (1 << GxVA_Color0)) {
            int32_t colorMaterial = rs[GxRs_ColorMaterial].m_value.m_data.i[0];
            c[23 * 4 + 0] = colorMaterial == 0;
            c[23 * 4 + 1] = colorMaterial == 1;
            c[23 * 4 + 2] = colorMaterial == 2;
        }

        c[23 * 4 + 3] = specular;

        for (uint32_t i = 0; lighting && !ambientOnly && i < 4; i++) {
            const CGxApiLight& light = this->m_lights[i];

            if (!light.m_enable) {
                continue;
            }

            float* l = &c[(24 + 5 * i) * 4];
            bool point = light.m_dir.w == 1.0f;
            C4Vector dir;
            dir.x = light.m_dir.x;
            dir.y = light.m_dir.y;
            dir.z = light.m_dir.z;
            dir.w = point ? 1.0f : 0.0f;
            dir = dir * viewMtx;

            l[0] = dir.x;
            l[1] = dir.y;
            l[2] = dir.z;
            l[3] = point ? 2.0f : 1.0f;
            l[4] = light.m_dirColor.x;
            l[5] = light.m_dirColor.y;
            l[6] = light.m_dirColor.z;
            l[7] = 1.0f;
            l[8] = light.m_ambColor.x;
            l[9] = light.m_ambColor.y;
            l[10] = light.m_ambColor.z;
            l[12] = light.m_specColor.x;
            l[13] = light.m_specColor.y;
            l[14] = light.m_specColor.z;
            l[16] = light.m_constantAttenuation;
            l[17] = light.m_linearAttenuation;
            l[18] = light.m_quadraticAttenuation;
            l[19] = 10000.0f;
        }

        c[50 * 4] = rs[GxRs_NormalizeNormals].m_value.m_data.u[0] != 0;

        const float* attenuation = rs[GxRs_PointScaleAttenuation].m_value.m_data.f;
        c[51 * 4 + 0] = rs[GxRs_PointScale].m_value.m_data.f[0];
        c[51 * 4 + 1] = *reinterpret_cast<const C3Vector*>(attenuation) != CGxDevice::s_pointScaleAttenuation;
        c[52 * 4 + 0] = attenuation[0];
        c[52 * 4 + 1] = attenuation[1];
        c[52 * 4 + 2] = attenuation[2];
        c[52 * 4 + 3] = (this->m_viewport.m_y.m_h - this->m_viewport.m_y.m_l) * this->DeviceCurWindow().maxY;

        if (memcmp(c, this->m_ffConstants[0], sizeof(c))) {
            memcpy(this->m_ffConstants[0], c, sizeof(c));
            UploadConstants(this->m_d3dContext, this->m_ffConstantBuffers[0], c, sizeof(c));
        }
    }

    if (ps) {
        float c[3 * 4];
        memset(c, 0, sizeof(c));

        ColorToFloat4(rs[GxRs_FogColor].m_value.m_data.u[0], &c[0], fog ? 1.0f : 0.0f);

        for (uint32_t tmu = 0; tmu < 2; tmu++) {
            bool enabled = rs[GxRs_Texture0 + tmu].m_value.m_data.p != nullptr;
            c[4 + tmu * 2 + 0] = enabled ? static_cast<float>(rs[GxRs_ColorOp0 + tmu].m_value.m_data.u[0]) : 6.0f;
            c[4 + tmu * 2 + 1] = enabled ? static_cast<float>(rs[GxRs_AlphaOp0 + tmu].m_value.m_data.u[0]) : 6.0f;
        }

        c[8] = specular;

        if (memcmp(c, this->m_ffConstants[1], sizeof(c))) {
            memcpy(this->m_ffConstants[1], c, sizeof(c));
            UploadConstants(this->m_d3dContext, this->m_ffConstantBuffers[1], c, sizeof(c));
        }
    }
}

// App shaders when bound and valid, otherwise the FF permutation for the current vertex format, lighting and
// textures. Returns whether the VS is fixed function.
bool CGxDeviceD3d11::IStateSyncShaders() {
    const CGxAppRenderState* rs = this->m_appRenderStates.m_data;
    CGxShader* vs = static_cast<CGxShader*>(rs[GxRs_VertexShader].m_value.m_data.p);
    CGxShader* ps = static_cast<CGxShader*>(rs[GxRs_PixelShader].m_value.m_data.p);
    bool ffVs = !vs || !vs->m_valid || !vs->m_apiSpecific;
    bool ffPs = !ps || !ps->m_valid || !ps->m_apiSpecific;

    // IStateSyncLights: lighting as D3D9 would set Ds_Lighting, plus ambient-only mode.
    bool lighting = rs[GxRs_Lighting].m_value.m_data.i[0] && (this->m_appMasterEnables & 0x1);
    bool ambientOnly = !rs[GxRs_Lighting].m_value.m_data.i[0] && !(this->m_primVertexMask & (1 << GxVA_Color0)) && rs[GxRs_MatDiffuse].m_value.m_data.u[0] != 0xFFFFFFFF;

    void* vsObject;
    const void* vsCode;
    uint32_t vsCodeSize;

    if (ffVs) {
        // D3D9's FF VS passes every texcoord set through; the FFX Box4/Gauss4/GlowWave quads read TEXCOORD2/3 in
        // their ps_2_0, which the 2-texcoord permutations left at 0 (two of four blur taps on texel 0,0, so the
        // glow took the colour of one corner pixel: sky towards the sun).
        // d3d11Debug 6: never pick them (bisect switch for the glue screens).
        int32_t t34 = D3d11Debug() == 6 ? -1 : FfSelectVsT34(this->m_primVertexMask, lighting || ambientOnly);
        const FfBlob& blob = t34 >= 0 ? g_ffVsT34[t34] : g_ffVs[FfSelectVs(this->m_primVertexMask, lighting || ambientOnly)];
        uint32_t index = t34 >= 0 ? 48 + t34 : FfSelectVs(this->m_primVertexMask, lighting || ambientOnly);

        if (!this->m_ffVs[index]) {
            this->m_d3dDevice->CreateVertexShader(blob.data, blob.size, nullptr, &this->m_ffVs[index]);
        }

        vsObject = this->m_ffVs[index];
        vsCode = blob.data;
        vsCodeSize = static_cast<uint32_t>(blob.size);
    } else {
        vsObject = vs->m_apiSpecific;
        vsCode = vs->m_code.m_data;
        vsCodeSize = vs->m_code.Count();
    }

    if (vsObject != this->m_curVs) {
        this->m_d3dContext->VSSetShader(static_cast<ID3D11VertexShader*>(vsObject), nullptr, 0);
        this->m_curVs = vsObject;
    }

    if (vsCode != this->m_curVsCode || vsCodeSize != this->m_curVsCodeSize) {
        this->m_curVsCode = vsCode;
        this->m_curVsCodeSize = vsCodeSize;
        this->m_curVsSig = DxbcInputSignatureHash(vsCode, vsCodeSize);
        this->m_cbBytes[0] = ffVs ? 0 : DxbcCb0Bytes(vsCode, vsCodeSize);
    }

    void* psObject;

    if (ffPs) {
        uint32_t index = FfSelectPs(rs[GxRs_Texture0].m_value.m_data.p != nullptr, rs[GxRs_Texture1].m_value.m_data.p != nullptr);

        if (!this->m_ffPs[index]) {
            this->m_d3dDevice->CreatePixelShader(g_ffPs[index].data, g_ffPs[index].size, nullptr, &this->m_ffPs[index]);
        }

        psObject = this->m_ffPs[index];
    } else {
        psObject = ps->m_apiSpecific;
    }

    if (psObject != this->m_curPs) {
        this->m_d3dContext->PSSetShader(static_cast<ID3D11PixelShader*>(psObject), nullptr, 0);
        this->m_curPs = psObject;
        this->m_cbBytes[1] = ffPs ? 0 : DxbcCb0Bytes(ps->m_code.m_data, ps->m_code.Count());
    }

    ID3D11Buffer* cb[2] = { ffVs ? this->m_ffConstantBuffers[0] : this->m_constantBuffers[0], ffPs ? this->m_ffConstantBuffers[1] : this->m_constantBuffers[1] };

    if (cb[0] != this->m_curCb[0]) {
        this->m_d3dContext->VSSetConstantBuffers(0, 1, &cb[0]);
        this->m_curCb[0] = cb[0];
    }

    if (cb[1] != this->m_curCb[1]) {
        this->m_d3dContext->PSSetConstantBuffers(0, 1, &cb[1]);
        this->m_curCb[1] = cb[1];
    }

    if (ffVs || ffPs) {
        this->IStateSyncFixedFunction(ffVs, ffPs, lighting || ambientOnly, ambientOnly);
    }

    return ffVs;
}

// PS SRVs every draw (cheap, and survives texture recreation behind an unchanged CGxTex*), samplers when the texture
// or its flags changed. Filtering follows the client's own settings: the texture's filter and anisotropy
// (textureFilteringMode) and texLodBias, which is what wxl-texfilter forced onto D3D9.
void CGxDeviceD3d11::IStateSyncTextures() {
    static float s_lodBias;
    float lodBias = TexLodBias();

    if (lodBias != s_lodBias) {
        s_lodBias = lodBias;
        this->m_samplerDirty = 0xFFFF;
    }

    for (uint32_t i = 0; i < 16; i++) {
        CGxTex* texture = static_cast<CGxTex*>(this->m_appRenderStates[GxRs_Texture0 + i].m_value.m_data.p);

        // Not stock: IRsSync skips a bind whose CGxTex* is unchanged, so a CGxTex destroyed and made again at the same
        // address, or given new flags, would never be created or uploaded. Every draw makes sure of it here. Render
        // targets (ShadowCache maps left in t4..t7 after a world session) are the app's to manage: never touch them.
        if (texture && !texture->m_flags.m_renderTarget) {
            this->ITexMarkAsUpdated(texture);
        }

        // Diagnostic (glue-shadow): the runtime silently nulls an SRV whose resource is the bound RTV/DSV, while
        // m_curSrv records it as bound until the next IBindRenderTargets.
        if (texture && (texture == this->m_textureTarget[GxBuffers_Color].m_texture || texture == this->m_textureTarget[GxBuffers_Depth].m_texture)) {
            D3d11LogOnce("CGxDeviceD3d11: t%u samples the current render target %p (%ux%u f%d)", i, static_cast<void*>(texture), texture->m_width, texture->m_height, texture->m_format);
        }

        TexApi* api = texture && !texture->m_needsCreation ? static_cast<TexApi*>(texture->m_apiSpecificData) : nullptr;
        ID3D11ShaderResourceView* srv = api ? api->m_srv : nullptr;

        if (srv != this->m_curSrv[i]) {
            this->m_d3dContext->PSSetShaderResources(i, 1, &srv);
            this->m_curSrv[i] = srv;
            this->m_samplerDirty |= 1 << i;
        }

        if (!texture || !(this->m_samplerDirty & (1 << i))) {
            continue;
        }

        const CGxTexFlags& flags = texture->m_flags;
        uint32_t anisotropy = flags.m_maxAnisotropy < 1 ? 1 : (flags.m_maxAnisotropy > 16 ? 16 : flags.m_maxAnisotropy);

        D3D11_SAMPLER_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.Filter = s_filter11[flags.m_filter < GxTexFilters_Last ? flags.m_filter : GxTex_Linear];

        if (flags.m_filter == GxTex_Anisotropic && anisotropy == 1) {
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        }

        desc.AddressU = flags.m_wrapU ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = flags.m_wrapV ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MipLODBias = lodBias;
        desc.MaxAnisotropy = anisotropy;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MaxLOD = flags.m_filter <= GxTex_Linear ? 0.0f : D3D11_FLOAT32_MAX;

        // Hardware PCF (S1b): a D24X8 render target here is a ShadowCache map (hwPCF), which the _pcf shaders read
        // with SampleCmpLevelZero (shader-xlate pcf_slots). D3D9's depth-texture compare is ref <= depth, bilinear
        // for a linear map. The vfog scene-depth copy is the one D24X8 read as plain depth. 4.3.4 IGetSamplerState.
        if (texture->m_format == GxTex_D24X8 && flags.m_renderTarget && texture != this->m_sceneDepthCopy) {
            desc.Filter = flags.m_filter == GxTex_Nearest ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
            desc.MaxLOD = 0.0f;
        }

        ID3D11SamplerState* sampler = static_cast<ID3D11SamplerState*>(this->IStateGet(State_Sampler, &desc, sizeof(desc)));
        this->m_d3dContext->PSSetSamplers(i, 1, &sampler);
    }

    this->m_samplerDirty = 0;
}

// CGxDeviceD3d::IStateSyncScissorRect.
void CGxDeviceD3d11::IStateSyncScissorRect() {
    if (!this->m_scissorRectDirty) {
        return;
    }

    const CRect& window = this->DeviceCurWindow();
    D3D11_RECT rect;

    rect.left = static_cast<LONG>(this->m_scissorRect.minX * window.maxX + 0.5f);
    rect.right = static_cast<LONG>(this->m_scissorRect.maxX * window.maxX + 1.0f);

    if (this->m_textureTarget[0].m_apiSpecific || this->m_textureTarget[1].m_apiSpecific) {
        rect.top = static_cast<LONG>(this->m_scissorRect.minY * window.maxY + 0.5f);
        rect.bottom = static_cast<LONG>(1.0f + this->m_scissorRect.maxY * window.maxY);
    } else {
        rect.top = static_cast<LONG>((1.0f - this->m_scissorRect.maxY) * window.maxY + 0.5f);
        rect.bottom = static_cast<LONG>(1.0f + (1.0f - this->m_scissorRect.minY) * window.maxY);
    }

    rect.left = rect.left < 0 ? 0 : rect.left;
    rect.top = rect.top < 0 ? 0 : rect.top;
    rect.right = rect.right > window.maxX ? static_cast<LONG>(window.maxX) : rect.right;
    rect.bottom = rect.bottom > window.maxY ? static_cast<LONG>(window.maxY) : rect.bottom;

    this->m_d3dContext->RSSetScissorRects(1, &rect);
    this->m_scissorRectDirty = 0;
}

// CGxDeviceD3d::ISetTexGen: the texgen matrix is taken from the view (and world) current when the state changes.
void CGxDeviceD3d11::ISetTexGen(uint32_t tmu, int32_t texGen) {
    C44Matrix matrix;

    if (texGen == GxTexGen_ObjectLinear || texGen == GxTexGen_World) {
        CGxMatrixStack& view = this->m_xforms[GxXform_View];
        matrix = view.m_mtx[view.m_level];
        matrix = matrix.AffineInverse();

        if (texGen == GxTexGen_ObjectLinear) {
            CGxMatrixStack& world = this->m_xforms[GxXform_World];
            matrix = matrix * world.m_mtx[world.m_level].Inverse();
        }
    } else if (texGen == GxTexGen_SphereMap) {
        matrix.a0 = 0.5f;
        matrix.b1 = 0.5f;
        matrix.d0 = 0.5f;
        matrix.d1 = 0.5f;
    } else {
        this->m_texGen[tmu].Identity();
        return;
    }

    CGxMatrixStack& stack = this->m_texGen[tmu];
    stack.m_dirty = 1;
    stack.m_flags[stack.m_level] &= ~CGxMatrixStack::F_Identity;
    stack.m_mtx[stack.m_level] = matrix;
}

// 4.3.4 IRsSendToHw (0x0086FF70) model: only descs and dirty bits change here, IStateSync resolves them. Fixed-function,
// fog, alpha-ref and clip-plane states are read straight from m_appRenderStates at draw time.
void CGxDeviceD3d11::IRsSendToHw(EGxRenderState which) {
    const CGxStateBom& value = this->m_appRenderStates[which].m_value;
    uint32_t master = this->m_appMasterEnables;

    switch (which) {
        case GxRs_PolygonOffset:
            // ponytail: D3D9 DEPTHBIAS is in depth units, D3D11 counts steps of a 24-bit depth buffer; D16/D32 targets
            // get a bias off by their own step size.
            this->m_rsDesc.DepthBias = static_cast<INT>(-value.m_data.f[0] * 16777216.0f);
            this->m_stateDirty |= Dirty_Raster;
            break;

        case GxRs_BlendingMode: {
            uint32_t mode = value.m_data.u[0] < 12 ? value.m_data.u[0] : 0;
            D3D11_RENDER_TARGET_BLEND_DESC& rt = this->m_blendDesc.RenderTarget[0];
            rt.BlendEnable = mode >= 2;
            rt.SrcBlend = mode >= 2 ? s_srcBlend11[mode] : D3D11_BLEND_ONE;
            rt.DestBlend = mode >= 2 ? s_dstBlend11[mode] : D3D11_BLEND_ZERO;
            rt.SrcBlendAlpha = AlphaBlend11(rt.SrcBlend);
            rt.DestBlendAlpha = AlphaBlend11(rt.DestBlend);
            this->m_stateDirty |= Dirty_Blend;
            break;
        }

        case GxRs_DepthTest:
        case GxRs_DepthFunc:
            if ((master & 0x4) && this->m_appRenderStates[GxRs_DepthTest].m_value.m_data.i[0]) {
                this->m_dsDesc.DepthFunc = s_cmpFunc11[this->m_appRenderStates[GxRs_DepthFunc].m_value.m_data.u[0] & 3];
            } else {
                this->m_dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
            }

            this->m_stateDirty |= Dirty_Depth;
            break;

        case GxRs_DepthWrite:
            this->m_dsDesc.DepthWriteMask = (master & 0x8) && value.m_data.i[0] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            this->m_stateDirty |= Dirty_Depth;
            break;

        case GxRs_ColorWrite:
            this->m_blendDesc.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>((master & 0x10) ? value.m_data.u[0] & 0xF : 0);
            this->m_stateDirty |= Dirty_Blend;
            break;

        case GxRs_Culling: {
            int32_t cull = (master & 0x20) ? value.m_data.i[0] : 0;
            this->m_rsDesc.CullMode = s_cullMode11[cull < 0 ? 0 : (cull > 2 ? 2 : cull)];
            this->m_stateDirty |= Dirty_Raster;
            break;
        }

        case GxRs_Multisample:
            this->m_rsDesc.MultisampleEnable = value.m_data.i[0] != 0;
            this->m_stateDirty |= Dirty_Raster;
            break;

        case GxRs_ScissorTest:
            this->m_rsDesc.ScissorEnable = value.m_data.i[0] != 0;
            this->m_stateDirty |= Dirty_Raster;
            break;

        case GxRs_Texture0:
        case GxRs_Texture1:
        case GxRs_Texture2:
        case GxRs_Texture3:
        case GxRs_Texture4:
        case GxRs_Texture5:
        case GxRs_Texture6:
        case GxRs_Texture7:
        case GxRs_Texture8:
        case GxRs_Texture9:
        case GxRs_Texture10:
        case GxRs_Texture11:
        case GxRs_Texture12:
        case GxRs_Texture13:
        case GxRs_Texture14:
        case GxRs_Texture15:
            if (value.m_data.p) {
                this->ITexMarkAsUpdated(static_cast<CGxTex*>(value.m_data.p));
            }

            this->m_samplerDirty |= 1 << (which - GxRs_Texture0);
            break;

        case GxRs_TexGen0:
        case GxRs_TexGen1:
        case GxRs_TexGen2:
        case GxRs_TexGen3:
        case GxRs_TexGen4:
        case GxRs_TexGen5:
        case GxRs_TexGen6:
        case GxRs_TexGen7:
            this->ISetTexGen(which - GxRs_TexGen0, value.m_data.i[0]);
            break;

        case GxRs_VertexShader:
        case GxRs_PixelShader: {
            CGxShader* shader = static_cast<CGxShader*>(value.m_data.p);

            if (shader && !shader->m_loaded) {
                this->IShaderCreate(shader);
            }

            break;
        }

        case Unk84:
            for (uint32_t i = 0; i < 4; i++) {
                this->m_blendFactor[i] = value.m_data.f[0];
            }

            this->m_stateDirty |= Dirty_Blend;
            break;

        case GxRs_StencilTest:
        case GxRs_StencilFunc:
        case GxRs_StencilRef:
        case GxRs_StencilReadMask:
        case GxRs_StencilWriteMask:
        case GxRs_StencilFail:
        case GxRs_StencilZFail:
        case GxRs_StencilPass: {
            // DECOMP: stencil (not in the stock client); EGxStencilCmp/Op are the D3D11 values. Off: the init desc.
            const CGxAppRenderState* rs = this->m_appRenderStates.Ptr();
            D3D11_DEPTH_STENCILOP_DESC face = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
            bool on = rs[GxRs_StencilTest].m_value.m_data.i[0] && this->m_caps.m_stencilBits;

            if (on) {
                face.StencilFailOp = static_cast<D3D11_STENCIL_OP>(rs[GxRs_StencilFail].m_value.m_data.u[0]);
                face.StencilDepthFailOp = static_cast<D3D11_STENCIL_OP>(rs[GxRs_StencilZFail].m_value.m_data.u[0]);
                face.StencilPassOp = static_cast<D3D11_STENCIL_OP>(rs[GxRs_StencilPass].m_value.m_data.u[0]);
                face.StencilFunc = static_cast<D3D11_COMPARISON_FUNC>(rs[GxRs_StencilFunc].m_value.m_data.u[0]);
            }

            this->m_dsDesc.StencilEnable = on;
            this->m_dsDesc.StencilReadMask = on ? static_cast<UINT8>(rs[GxRs_StencilReadMask].m_value.m_data.u[0]) : 0xFF;
            this->m_dsDesc.StencilWriteMask = on ? static_cast<UINT8>(rs[GxRs_StencilWriteMask].m_value.m_data.u[0]) : 0xFF;
            this->m_dsDesc.FrontFace = face;
            this->m_dsDesc.BackFace = face;
            s_stencilRef = on ? (rs[GxRs_StencilRef].m_value.m_data.u[0] & 0xFF) : 0;
            this->m_stateDirty |= Dirty_Depth;
            break;
        }

        default:
            break;
    }
}

void CGxDeviceD3d11::IStateSync() {
    this->IRsSync(0);

    if (this->m_appMasterEnables != this->m_hwMasterEnables) {
        this->m_rsDesc.FillMode = (this->m_appMasterEnables & (1 << GxMasterEnable_PolygonFill)) ? D3D11_FILL_SOLID : D3D11_FILL_WIREFRAME;
        this->m_hwMasterEnables = this->m_appMasterEnables;
        this->m_stateDirty |= Dirty_Raster;
    }

    bool ffVs = this->IStateSyncShaders();
    this->IShaderConstantsFlush();
    this->IStateSyncDeviceConstants(ffVs);

    if (this->m_stateDirty & Dirty_Raster) {
        this->m_d3dContext->RSSetState(static_cast<ID3D11RasterizerState*>(this->IStateGet(State_Raster, &this->m_rsDesc, sizeof(this->m_rsDesc))));
    }

    // gxAlphaToCoverage: alpha-tested opaque / alpha-key draws into an MSAA target. Plain A2C: the SM4 pack's
    // alpha test (discard below the reference) stays, alpha above it is not sharpened (the Vulkan path sharpens).
    const BOOL a2c = this->m_curRtvSamples > 1 && this->m_a2cVar && this->m_a2cVar->m_intValue != 0 && this->m_appRenderStates[GxRs_AlphaRef].m_value.m_data.i[0] > 0
        && static_cast<uint32_t>(this->m_appRenderStates[GxRs_BlendingMode].m_value.m_data.i[0]) <= GxBlend_AlphaKey;

    if (this->m_blendDesc.AlphaToCoverageEnable != a2c) {
        this->m_blendDesc.AlphaToCoverageEnable = a2c;
        this->m_stateDirty |= Dirty_Blend;
    }

    // GBUFFER: RT1 written by an oC1 pixel shader with an opaque / alpha-key blend only (particles, glass keep the normal under them).
    const CGxShader* psh = static_cast<const CGxShader*>(this->m_appRenderStates[GxRs_PixelShader].m_value.m_data.p);
    const UINT8 normalMask = psh && psh->m_writesNormal && this->m_blendDesc.RenderTarget[0].RenderTargetWriteMask
        && static_cast<uint32_t>(this->m_appRenderStates[GxRs_BlendingMode].m_value.m_data.i[0]) <= GxBlend_AlphaKey ? 0xF : 0;

    if (this->m_blendDesc.RenderTarget[1].RenderTargetWriteMask != normalMask) {
        this->m_blendDesc.RenderTarget[1].RenderTargetWriteMask = normalMask;
        this->m_stateDirty |= Dirty_Blend;
    }

    if (this->m_stateDirty & Dirty_Blend) {
        this->m_d3dContext->OMSetBlendState(static_cast<ID3D11BlendState*>(this->IStateGet(State_Blend, &this->m_blendDesc, sizeof(this->m_blendDesc))), this->m_blendFactor, 0xFFFFFFFF);
    }

    if (this->m_stateDirty & Dirty_Depth) {
        this->m_d3dContext->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(this->IStateGet(State_Depth, &this->m_dsDesc, sizeof(this->m_dsDesc))), s_stencilRef);
    }

    this->m_stateDirty = 0;

    this->IStateSyncTextures();
    this->IStateSyncScissorRect();
    this->IStateSyncInputLayout();
    this->IStateSyncVertexPtrs();

    if (this->m_primIndexBuf) {
        PoolApi* api = static_cast<PoolApi*>(this->m_primIndexBuf->m_pool->m_apiSpecific);

        if (api && this->m_curIndexBuf != api->m_buffer) {
            this->m_d3dContext->IASetIndexBuffer(api->m_buffer, DXGI_FORMAT_R16_UINT, 0);
            this->m_curIndexBuf = api->m_buffer;
        }
    }

    this->m_primIndexDirty = 0;

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }
}


void CGxDeviceD3d11::Draw(CGxBatch* batch, int32_t count) {
    if (!this->m_context || this->m_intF5C) {
        return;
    }

    // CUSTOM (Badgermilk0 port): performance diagnostics (FrameProfile.h), API draw calls per frame
    FrameProfileCount(FRAMEPROFILE_DRAW_CALLS, static_cast<uint32_t>(count));

    this->m_resolveValid = false;
    this->IStateSync();

    if (this->m_dsDesc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO) {
        this->m_sceneDepthFresh = false;
    }

    if (s_dumpFrame) {
        const CGxAppRenderState* rs = this->m_appRenderStates.m_data;
        const CGxShader* vs = static_cast<const CGxShader*>(rs[GxRs_VertexShader].m_value.m_data.p);
        const CGxShader* ps = static_cast<const CGxShader*>(rs[GxRs_PixelShader].m_value.m_data.p);
        bool ffVs = !vs || !vs->m_valid || !vs->m_apiSpecific;
        bool ffPs = !ps || !ps->m_valid || !ps->m_apiSpecific;
        bool lighting = rs[GxRs_Lighting].m_value.m_data.i[0] && (this->m_appMasterEnables & 0x1);
        uint32_t lights = 0;

        for (uint32_t i = 0; i < 4; i++) {
            lights += lighting && this->m_lights[i].m_enable;
        }

        char srvs[256];
        int32_t used = 0;
        srvs[0] = '\0';

        for (uint32_t i = 0; i < 8 && used < static_cast<int32_t>(sizeof(srvs)) - 48; i++) {
            const CGxTex* tex = static_cast<const CGxTex*>(rs[GxRs_Texture0 + i].m_value.m_data.p);

            if (tex || this->m_curSrv[i]) {
                used += snprintf(srvs + used, sizeof(srvs) - used, " t%u=%p/%p:%ux%u:f%d", i, static_cast<const void*>(tex), static_cast<void*>(this->m_curSrv[i]), tex ? tex->m_width : 0, tex ? tex->m_height : 0, tex ? static_cast<int32_t>(tex->m_format) : -1);
            }
        }

        const D3D11_RENDER_TARGET_BLEND_DESC& blend = this->m_blendDesc.RenderTarget[0];
        const CGxMatrixStack& tg0 = this->m_texGen[0];
        const CGxMatrixStack& tg1 = this->m_texGen[1];

        D3d11Log("dump %u: vs=%s ps=%s%s%s vtx=%u prim=%d blend=%d:%d/%d wm=%X depth=f%d w%d aref=%d(ge) bm=%d clip=%X fog=%d c=%08X %g-%g lights=%u%s ts=%d,%d tg=%d,%d tgid=%d,%d tc=%u,%u vp=%.3f-%.3f,%.3f-%.3f,%.2f-%.2f",
            s_dumpDraw++, DumpShaderName(vs, ffVs), DumpShaderName(ps, ffPs), ffVs || ffPs ? " vmask=" : "", ffVs || ffPs ? std::to_string(this->m_primVertexMask).c_str() : "",
            batch->m_count, batch->m_primType,
            blend.BlendEnable, blend.SrcBlend, blend.DestBlend, blend.RenderTargetWriteMask,
            this->m_dsDesc.DepthFunc, this->m_dsDesc.DepthWriteMask,
            rs[GxRs_AlphaRef].m_value.m_data.i[0], rs[GxRs_BlendingMode].m_value.m_data.i[0],
            rs[GxRs_ClipPlaneMask].m_value.m_data.u[0],
            rs[GxRs_Fog].m_value.m_data.i[0] && (this->m_appMasterEnables & 0x2), rs[GxRs_FogColor].m_value.m_data.u[0], rs[GxRs_FogStart].m_value.m_data.f[0], rs[GxRs_FogEnd].m_value.m_data.f[0],
            lights, srvs,
            rs[GxRs_TextureShader0].m_value.m_data.i[0], rs[GxRs_TextureShader1].m_value.m_data.i[0],
            rs[GxRs_TexGen0].m_value.m_data.i[0], rs[GxRs_TexGen1].m_value.m_data.i[0],
            (tg0.m_flags[tg0.m_level] & CGxMatrixStack::F_Identity) != 0, (tg1.m_flags[tg1.m_level] & CGxMatrixStack::F_Identity) != 0,
            rs[GxRs_TextureCoord0].m_value.m_data.u[0], rs[GxRs_TextureCoord1].m_value.m_data.u[0],
            this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);
    }

    if (!this->m_sm2Shaders.empty()) {
        const CGxAppRenderState* rs = this->m_appRenderStates.m_data;

        for (const void* bound : { rs[GxRs_VertexShader].m_value.m_data.p, rs[GxRs_PixelShader].m_value.m_data.p }) {
            auto found = bound ? this->m_sm2Shaders.find(static_cast<const CGxShader*>(bound)) : this->m_sm2Shaders.end();

            if (found == this->m_sm2Shaders.end()) {
                continue;
            }

            if (!found->second.empty() && found->second[0] != '*') {
                D3d11Log("CGxDeviceD3d11: first draw with SM2 shader %s", found->second.c_str());
                found->second.insert(0, "*");
            }

            if (D3d11Debug() == 2) {
                return;
            }
        }
    }

    D3D11_PRIMITIVE_TOPOLOGY topology = s_primitiveConversion[batch->m_primType];

    if (this->m_curTopology != topology) {
        this->m_d3dContext->IASetPrimitiveTopology(topology);
        this->m_curTopology = topology;
    }

    if (batch->m_primType != GxPrim_TriangleFan) {
        if (count) {
            this->m_d3dContext->DrawIndexed(batch->m_count, batch->m_start + (this->m_primIndexBuf->m_index >> 1), 0);
        } else {
            this->m_d3dContext->Draw(batch->m_count, 0);
        }

        return;
    }

    // Triangle fan -> list through a scratch index buffer.
    if (batch->m_count < 3) {
        return;
    }

    uint32_t indexCount = (batch->m_count - 2) * 3;
    uint32_t bytes = indexCount * sizeof(uint16_t);

    if (bytes > this->m_fanIndexBufferSize) {
        SafeRelease(this->m_fanIndexBuffer);

        D3D11_BUFFER_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.ByteWidth = bytes < 0x10000 ? 0x10000 : bytes;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(this->m_d3dDevice->CreateBuffer(&desc, nullptr, &this->m_fanIndexBuffer))) {
            this->m_fanIndexBuffer = nullptr;
            this->m_fanIndexBufferSize = 0;
            return;
        }

        this->m_fanIndexBufferSize = desc.ByteWidth;
    }

    const uint16_t* source = nullptr;

    if (count) {
        PoolApi* api = static_cast<PoolApi*>(this->m_primIndexBuf->m_pool->m_apiSpecific);

        if (!api) {
            return;
        }

        source = reinterpret_cast<const uint16_t*>(api->m_shadow + this->m_primIndexBuf->m_index) + batch->m_start;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;

    if (FAILED(this->m_d3dContext->Map(this->m_fanIndexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }

    uint16_t* dest = static_cast<uint16_t*>(mapped.pData);

    for (uint32_t i = 1; i + 1 < batch->m_count; i++) {
        *dest++ = source ? source[0] : 0;
        *dest++ = source ? source[i] : static_cast<uint16_t>(i);
        *dest++ = source ? source[i + 1] : static_cast<uint16_t>(i + 1);
    }

    this->m_d3dContext->Unmap(this->m_fanIndexBuffer, 0);

    if (this->m_curIndexBuf != this->m_fanIndexBuffer) {
        this->m_d3dContext->IASetIndexBuffer(this->m_fanIndexBuffer, DXGI_FORMAT_R16_UINT, 0);
        this->m_curIndexBuf = this->m_fanIndexBuffer;
    }

    this->m_d3dContext->DrawIndexed(indexCount, 0, 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Queries

static ID3D11Query* CreateOcclusionQuery(ID3D11Device* device) {
    D3D11_QUERY_DESC desc = { D3D11_QUERY_OCCLUSION, 0 };
    ID3D11Query* query = nullptr;

    if (FAILED(device->CreateQuery(&desc, &query))) {
        return nullptr;
    }

    return query;
}

void CGxDeviceD3d11::QueryCreate(CGxQuery*& query, EGxQueryType type) {
    CGxDevice::QueryCreate(query, type);

    if (!query->m_apiSpecific && this->m_d3dDevice) {
        query->m_apiSpecific = CreateOcclusionQuery(this->m_d3dDevice);
    }
}

void CGxDeviceD3d11::QueryDestroy(CGxQuery*& query) {
    if (query->m_apiSpecific) {
        static_cast<ID3D11Query*>(query->m_apiSpecific)->Release();
    }

    query->m_apiSpecific = nullptr;
    CGxDevice::QueryDestroy(query);
}

void CGxDeviceD3d11::IReleaseD3dQueries() {
    for (CGxQuery* query = this->m_queryList.Head(); query; query = this->m_queryList.Next(query)) {
        if (query->m_apiSpecific) {
            static_cast<ID3D11Query*>(query->m_apiSpecific)->Release();
            query->m_apiSpecific = nullptr;
        }
    }
}

bool CGxDeviceD3d11::QueryBegin(CGxQuery* query) {
    if (!this->m_context) {
        return false;
    }

    if (!query->m_apiSpecific) {
        query->m_apiSpecific = CreateOcclusionQuery(this->m_d3dDevice);

        if (!query->m_apiSpecific) {
            return false;
        }
    }

    if (query->m_type == GxQueryType_Occlusion) {
        this->m_d3dContext->Begin(static_cast<ID3D11Query*>(query->m_apiSpecific));
    }

    return true;
}

bool CGxDeviceD3d11::QueryEnd(CGxQuery* query) {
    if (!this->m_context || !query->m_apiSpecific) {
        return false;
    }

    if (query->m_type == GxQueryType_Occlusion) {
        this->m_d3dContext->End(static_cast<ID3D11Query*>(query->m_apiSpecific));
    }

    return true;
}

bool CGxDeviceD3d11::QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) {
    if (!this->m_context || !query->m_apiSpecific) {
        return false;
    }

    ID3D11Query* d3dQuery = static_cast<ID3D11Query*>(query->m_apiSpecific);

    switch (param) {
        case GxQueryParam_Unk0:
            data = d3dQuery->GetDataSize();
            return true;

        case GxQueryParam_Unk1:
            data = this->m_d3dContext->GetData(d3dQuery, nullptr, 0, 0) == S_OK;
            return false;

        default:
            return false;
    }
}

bool CGxDeviceD3d11::QueryGetData(CGxQuery* query, uint32_t& data) {
    if (!this->m_context || !query->m_apiSpecific) {
        return false;
    }

    ID3D11Query* d3dQuery = static_cast<ID3D11Query*>(query->m_apiSpecific);
    UINT64 samples = 0;
    HRESULT hr;

    // Bounded: an unbounded S_FALSE spin hangs if the GPU work never retires (lost device, teardown).
    DWORD start = GetTickCount();

    do {
        hr = this->m_d3dContext->GetData(d3dQuery, &samples, sizeof(samples), 0);
    } while (hr == S_FALSE && GetTickCount() - start < 100);

    if (hr != S_OK) {
        return false;
    }

    data = D3d11Debug() == 3 ? 0 : static_cast<uint32_t>(samples);

    return hr == S_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Shaders

// The translated pack (shader-xlate xlate.py) sits beside the SM3 files as Shaders\Vertex\vs_4_0 and
// Shaders\Pixel\ps_4_0. Caps keep reporting vs_3_0 / ps_3_0, so every client feature check behaves as on D3D9; only
// the load path moves to the SM4 folder. IShaderLoad has no fallback below vs_4_0 / ps_4_0, so SM3 bytecode is never
// loaded on this device.
static int32_t Sm4Profile(EGxShTarget target, int32_t profile) {
    if (profile <= 0) {
        return profile;
    }

    return target == GxSh_Vertex ? 4 : (target == GxSh_Pixel ? 5 : profile);
}

// Body is DXBC (same 'GXSH' container). shader->m_code keeps the bytecode, which input layouts are built from.
void CGxDeviceD3d11::IShaderCreate(CGxShader* shader) {
    shader->m_valid = 0;

    if (!this->m_context) {
        return;
    }

    shader->m_loaded = 1;
    SafeRelease(reinterpret_cast<IUnknown*&>(shader->m_apiSpecific));

    uint32_t size = shader->m_code.Count();
    const void* code = shader->m_code.m_data;

    if (size < 4 || memcmp(code, "DXBC", 4)) {
        static int32_t s_logged;

        if (!s_logged) {
            s_logged = 1;
            D3d11Log("CGxDeviceD3d11: shader without DXBC body; is the translated pack (vs_4_0 / ps_4_0) installed?");
        }

        return;
    }

    HRESULT hr = E_FAIL;

    if (shader->m_target == GxSh_Vertex) {
        hr = this->m_d3dDevice->CreateVertexShader(code, size, nullptr, reinterpret_cast<ID3D11VertexShader**>(&shader->m_apiSpecific));
    } else if (shader->m_target == GxSh_Pixel) {
        hr = this->m_d3dDevice->CreatePixelShader(code, size, nullptr, reinterpret_cast<ID3D11PixelShader**>(&shader->m_apiSpecific));
        shader->m_writesNormal = DxbcWritesTarget1(code, size);   // GBUFFER
    }

    if (FAILED(hr)) {
        shader->m_apiSpecific = nullptr;
        return;
    }

    shader->m_valid = 1;
}

void CGxDeviceD3d11::ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations) {
    int32_t& profile = this->m_caps.m_shaderTargets[target];
    int32_t saved = profile;
    profile = Sm4Profile(target, profile);
    CGxDevice::ShaderCreate(shaders, target, path, name, permutations);
    profile = saved;

    // No SM2 fallback exists for vs_4_0/ps_4_0, so a family missing from the pack (or an empty permutation) falls
    // back to fixed function. Name every one, not just the first.
    for (uint32_t i = 0; i < permutations; i++) {
        if (shaders[i]->m_code.Count() < 4 || memcmp(shaders[i]->m_code.m_data, "DXBC", 4)) {
            D3d11Log("CGxDeviceD3d11: no DXBC for %s\\%s.bls permutation %u of %u", path, name, i, permutations);
        }
    }

    if (permutations == 1 && !shaders[0]->m_loaded) {
        this->IShaderCreate(shaders[0]);
    }

    if (IsSm2Family(name)) {
        for (uint32_t i = 0; i < permutations; i++) {
            this->m_sm2Shaders[shaders[i]] = name;
        }
    }
}

// The input-layout cache keys on the bound VS code pointer; a destroyed or reloaded shader's buffer can come back at
// the same address with another input signature, so forget it.
void CGxDeviceD3d11::IShaderForget(CGxShader* shader) {
    if (shader->m_target == GxSh_Vertex && shader->m_code.m_data == this->m_curVsCode) {
        this->m_curVsCode = nullptr;
        this->m_curVsCodeSize = 0;
        this->m_curLayoutKey = 0;
    }
}

void CGxDeviceD3d11::ShaderDestroy(CGxShader*& shader) {
    this->IShaderForget(shader);

    if (shader->m_refCount == 1) {
        this->m_sm2Shaders.erase(shader);

        if (shader->m_apiSpecific) {
            static_cast<IUnknown*>(shader->m_apiSpecific)->Release();
            shader->m_apiSpecific = nullptr;
        }

        shader->m_loaded = 0;
    }

    CGxDevice::ShaderDestroy(shader);
}

void CGxDeviceD3d11::IShaderReload(CGxShader* shader, const char* path, const char* name) {
    if (shader->m_target != GxSh_Vertex && shader->m_target != GxSh_Pixel) {
        return;
    }

    this->IShaderForget(shader);

    if (shader->m_refCount == 1) {
        if (shader->m_apiSpecific) {
            static_cast<IUnknown*>(shader->m_apiSpecific)->Release();
            shader->m_apiSpecific = nullptr;
        }

        shader->m_loaded = 0;
    }

    EGxShTarget target = static_cast<EGxShTarget>(shader->m_target);
    int32_t& profile = this->m_caps.m_shaderTargets[target];
    int32_t saved = profile;
    profile = Sm4Profile(target, profile);
    this->IShaderLoad(&shader, target, path, name, 1);
    profile = saved;
    this->IShaderCreate(shader);
}

// ---------------------------------------------------------------------------------------------------------------
// Misc

void CGxDeviceD3d11::DeviceEvictResources() {
}

void* CGxDeviceD3d11::DeviceWindow() {
    return this->m_hwnd;
}

void CGxDeviceD3d11::DeviceOverride(EGxOverride override, uint32_t value) {
    CGxDevice::DeviceOverride(override, value);

    if (override == GxOverride_PixelShader) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = value;
    } else if (override == GxOverride_Unk7) {
        this->m_caps.m_texNonPow2 = value != 0;
    }
}

void CGxDeviceD3d11::CapsWindowSize(CRect& rect) {
    rect = this->DeviceCurWindow();
}

void CGxDeviceD3d11::CapsWindowSizeInScreenCoords(CRect& rect) {
    if (this->IDevIsWindowed()) {
        const CRect& window = this->DeviceCurWindow();
        POINT points[2];
        points[0].x = 0;
        points[0].y = 0;
        points[1].x = static_cast<LONG>(window.maxX);
        points[1].y = static_cast<LONG>(window.maxY);
        MapWindowPoints(this->m_hwnd, nullptr, points, 2);
        rect.minY = static_cast<float>(points[0].y);
        rect.minX = static_cast<float>(points[0].x);
        rect.maxY = static_cast<float>(points[1].y);
        rect.maxX = static_cast<float>(points[1].x);
    } else {
        rect = this->DeviceCurWindow();
    }
}

void CGxDeviceD3d11::LogCrashInfo(char* buffer, uint32_t size) {
    char line[260];
    SStrPrintf(line, sizeof(line), "D3D11 feature level: 0x%X\r\n", static_cast<uint32_t>(this->m_featureLevel));
    SStrPack(buffer, line, size);

    IDXGIAdapter1* adapter = nullptr;

    if (this->m_dxgiFactory && SUCCEEDED(this->m_dxgiFactory->EnumAdapters1(0, &adapter))) {
        DXGI_ADAPTER_DESC1 desc;

        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            SStrPrintf(line, sizeof(line), "Adapter: %s (vendor 0x%04X device 0x%04X)\r\n", name, desc.VendorId, desc.DeviceId);
            SStrPack(buffer, line, size);
        }

        adapter->Release();
    }
}

// No stereo on D3D11 (NVAPI stereo is a D3D9 path here).
void CGxDeviceD3d11::StereoSetConvergence(float convergence) {
}

float CGxDeviceD3d11::StereoGetConvergence() {
    return 0.0f;
}

void CGxDeviceD3d11::StereoSetSeparation(float separation) {
}

float CGxDeviceD3d11::StereoGetSeparation() {
    return 0.0f;
}

bool CGxDeviceD3d11::StereoEnabled() {
    return false;
}

void CGxDeviceD3d11::DeviceUnk81(uint32_t a, uint32_t b) {
}

void CGxDeviceD3d11::DeviceUnk82() {
}

void CGxDeviceD3d11::DeviceUnk83(uint32_t a, uint32_t b) {
}
