#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Error.h"
#include "Engine/Source/Storm/H/storm.h"

// Phase status (see README.md): device, swap chain, back buffer, readable depth, clear/present/resize, buffer pools,
// textures and draw submission are real. Render states, the cbuffer flush, input layouts, fixed function and shader
// objects are stubs marked TODO(phase N).

typedef HRESULT (WINAPI* CreateDXGIFactory1Proc)(REFIID riid, void** factory);

// cbuffer b0 per stage: 256 float4 registers, then the int/bool/alpha-test slots (layout: shader-xlate README).
// TODO(phase 2): size the tail to the translator's final layout.
static const uint32_t kConstantBufferBytes = (256 + 32) * 16;

static const uint32_t s_gxAttribToD3dAttribSize[7] = { 4, 4, 4, 8, 12, 4, 4 };

// Colour formats: 565 is widened (D3D11.0 has no 16-bit swap chain formats). Index = CGxFormat::Format.
const DXGI_FORMAT CGxDeviceD3d11::s_GxFormatToDxgiColor[CGxFormat::Formats_Last] = {
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM
};

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
    DXGI_FORMAT_R24G8_TYPELESS
};

EGxTexFormat CGxDeviceD3d11::s_GxTexFormatToUse[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Abgr8888, GxTex_Argb8888, GxTex_Argb4444, GxTex_Argb1555, GxTex_Rgb565, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8 };

// 4.3.4 widens every 16-bit format to 8888 (table 0x00BD67E8). Blit has Argb8888/Argb4444 -> Abgr8888 converters.
// TODO(phase 5): Argb1555/Rgb565 data has no converter to 8888; on pre-11.1 runtimes those textures fail to create.
const EGxTexFormat CGxDeviceD3d11::s_tolerableTexFmtMapping[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Argb1555, GxTex_Rgb565, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8 };

// Triangle fans have no D3D11 topology: Draw() rewrites them as lists.
const D3D11_PRIMITIVE_TOPOLOGY CGxDeviceD3d11::s_primitiveConversion[GxPrims_Last] = {
    D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
};

template <class T>
static void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

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
    this->m_rtDepthTexture = nullptr;
    this->m_rtDepthDsv = nullptr;
    this->m_resolveTexture = nullptr;
    this->m_constantBuffers[0] = nullptr;
    this->m_constantBuffers[1] = nullptr;
    this->m_fanIndexBuffer = nullptr;
    this->m_fanIndexBufferSize = 0;
    this->m_curRtv = nullptr;
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
        CGxDevice::Log("CGxDeviceD3d11::ILoadD3dLib(): unable to load d3d11.dll/dxgi.dll");
        CGxDeviceD3d11::IUnloadD3dLib(d3dLib, dxgiLib, factory);
        return 0;
    }

    createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3dLib, "D3D11CreateDevice"));
    CreateDXGIFactory1Proc createFactory = reinterpret_cast<CreateDXGIFactory1Proc>(GetProcAddress(dxgiLib, "CreateDXGIFactory1"));

    if (!createDevice || !createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        CGxDevice::Log("CGxDeviceD3d11::ILoadD3dLib(): unable to GetProcAddress()/CreateDXGIFactory1()");
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

void CGxDeviceD3d11::ISetWindowFocus(int32_t focus) {
    this->m_windowFocus = focus;

    if (focus && !this->m_format.m_window) {
        RECT rect;
        GetWindowRect(this->m_hwnd, &rect);
        ClipCursor(&rect);
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
        CGxDevice::Log("CGxDeviceD3d11::DeviceCreate(): WindowClassCreate() failed: %s", error);
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
    CGxDevice::Log("CGxDeviceD3d11::DeviceSetFormat():");
    this->Log(format);

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

    CGxDevice::Log("CGxDeviceD3d11::DeviceSetFormat(): unable to set format!");
    this->IDestroyD3d();

    if (this->m_hwnd) {
        DestroyWindow(this->m_hwnd);
    }

    this->m_hwnd = nullptr;

    return 0;
}

void CGxDeviceD3d11::DeviceDestroy() {
    CGxDevice::DeviceDestroy();

    if (this->m_hwnd && this->m_ownhwnd) {
        DestroyWindow(this->m_hwnd);
        this->m_hwnd = nullptr;
    }

    if (this->m_hwndClass) {
        UnregisterClassA(reinterpret_cast<const char*>(static_cast<uintptr_t>(this->m_hwndClass)), GetModuleHandleA(nullptr));
        this->m_hwndClass = 0;
    }

    this->IDestroyD3d();
    CGxDeviceD3d11::IUnloadD3dLib(this->m_d3dLib, this->m_dxgiLib, this->m_dxgiFactory);
}

// 4.3.4 FUN_00853EE0: adapter 0 from the factory, FL {11_0, 10_1, 10_0}, single-threaded; one dynamic cbuffer per
// stage bound once at b0.
int32_t CGxDeviceD3d11::ICreateD3dDevice(const CGxFormat& format) {
    IDXGIAdapter1* adapter = nullptr;

    if (FAILED(this->m_dxgiFactory->EnumAdapters1(0, &adapter))) {
        CGxDevice::Log("CGxDeviceD3d11: no adapter");
        return 0;
    }

    static const D3D_FEATURE_LEVEL s_levels[3] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = this->m_d3dCreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED, s_levels, 3, D3D11_SDK_VERSION, &this->m_d3dDevice, &this->m_featureLevel, &this->m_d3dContext);
    adapter->Release();

    if (FAILED(hr)) {
        CGxDevice::Log("CGxDeviceD3d11: D3D11CreateDevice failed: 0x%08X", static_cast<uint32_t>(hr));
        this->m_d3dDevice = nullptr;
        this->m_d3dContext = nullptr;
        return 0;
    }

    CGxDevice::Log("CGxDeviceD3d11: feature level 0x%X", static_cast<uint32_t>(this->m_featureLevel));

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
        CGxDevice::Log("CGxDeviceD3d11: CreateSwapChain failed");
        this->m_swapChain = nullptr;
        return 0;
    }

    this->m_dxgiFactory->MakeWindowAssociation(this->m_hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (format.m_fixLag) {
        IDXGIDevice1* dxgiDevice = nullptr;

        if (SUCCEEDED(this->m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDevice)))) {
            dxgiDevice->SetMaximumFrameLatency(1);
            dxgiDevice->Release();
        }
    }

    if (!this->ICreateBackBuffer()) {
        return 0;
    }

    for (uint32_t i = 0; i < 2; i++) {
        D3D11_BUFFER_DESC cbDesc;
        memset(&cbDesc, 0, sizeof(cbDesc));
        cbDesc.ByteWidth = kConstantBufferBytes;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(this->m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &this->m_constantBuffers[i]))) {
            return 0;
        }
    }

    this->m_d3dContext->VSSetConstantBuffers(0, 1, &this->m_constantBuffers[0]);
    this->m_d3dContext->PSSetConstantBuffers(0, 1, &this->m_constantBuffers[1]);

    this->m_context = 1;

    this->ISetCaps(format);
    this->Log(this->m_caps);

    this->DeviceSetGamma(this->m_gammaRamp);
    this->ICursorCreate(format);

    g_theGxDevicePtr->ShaderCreate(CGxDevice::s_uiVertexShader, GxSh_Vertex, "Shaders\\Vertex", "UI", 2);
    g_theGxDevicePtr->ShaderCreate(&CGxDevice::s_uiPixelShader, GxSh_Pixel, "Shaders\\Pixel", "UI", 1);

    return 1;
}

void CGxDeviceD3d11::IDestroyD3d() {
    for (uint32_t i = 0; i < 2; i++) {
        if (CGxDevice::s_uiVertexShader[i]) {
            g_theGxDevicePtr->ShaderDestroy(CGxDevice::s_uiVertexShader[i]);
        }
    }

    if (CGxDevice::s_uiPixelShader) {
        g_theGxDevicePtr->ShaderDestroy(CGxDevice::s_uiPixelShader);
    }

    this->ICursorDestroy();
    this->ITexForceRecreation(1);
    this->IReleaseD3dPools(1);
    this->IReleaseD3dQueries();

    if (this->m_d3dContext) {
        this->m_d3dContext->ClearState();
    }

    this->IReleaseBackBuffer();
    SafeRelease(this->m_constantBuffers[0]);
    SafeRelease(this->m_constantBuffers[1]);
    SafeRelease(this->m_fanIndexBuffer);
    this->m_fanIndexBufferSize = 0;

    if (this->m_swapChain) {
        this->m_swapChain->SetFullscreenState(FALSE, nullptr);
    }

    SafeRelease(this->m_swapChain);
    SafeRelease(this->m_d3dContext);
    SafeRelease(this->m_d3dDevice);

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
    this->m_caps.unk_0010 = 1;

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

    for (uint32_t i = 0; i < GxTexFormats_Last; i++) {
        UINT support = 0;
        this->m_caps.m_texFmt[i] = i && SUCCEEDED(this->m_d3dDevice->CheckFormatSupport(s_GxTexFmtToDxgiFmt[i], &support)) && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D);
    }

    this->m_caps.m_generateMipMaps = 1;

    static const EGxTexFormat s_rttFormats[4] = { GxTex_Argb8888, GxTex_Rgb565, GxTex_Gr1616F, GxTex_R32F };

    for (uint32_t i = 0; i < 4; i++) {
        UINT support = 0;
        this->m_caps.m_rttFormat[s_rttFormats[i]] = SUCCEEDED(this->m_d3dDevice->CheckFormatSupport(s_GxTexFmtToDxgiFmt[s_rttFormats[i]], &support)) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);
    }

    this->m_caps.m_rttFormat[GxTex_D24X8] = 1;

    this->m_caps.m_colorWrite = 1;
    this->m_caps.m_hwCursor = 0;
    // TODO(phase 3): user clip planes become SV_ClipDistance in the FF/translated vertex shaders.
    this->m_caps.m_maxClipPlanes = 0;
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
    this->m_caps.unk_0130 = 1;

    for (uint32_t i = 0; i < 5; i++) {
        this->m_caps.unk_0114[i] = 0;
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
    this->m_curDsv = nullptr;
    this->m_savedDsv = nullptr;
    SafeRelease(this->m_backBufferRtv);
    SafeRelease(this->m_backBuffer);
    SafeRelease(this->m_depthSrv);
    SafeRelease(this->m_depthDsv);
    SafeRelease(this->m_depthTexture);
    SafeRelease(this->m_rtDepthDsv);
    SafeRelease(this->m_rtDepthTexture);
    SafeRelease(this->m_resolveTexture);
}

// Colour = texture target or back buffer. Depth = depth target, else the back buffer's depth, or for a colour
// texture target a scratch depth of exactly its size (CGxDeviceD3d::ICreateDepthStencilSurface's job).
void CGxDeviceD3d11::IBindRenderTargets() {
    ID3D11RenderTargetView* rtv = this->m_backBufferRtv;
    ID3D11DepthStencilView* dsv = this->m_depthDsv;

    if (this->m_textureTarget[GxBuffers_Color].m_apiSpecific) {
        rtv = static_cast<ID3D11RenderTargetView*>(this->m_textureTarget[GxBuffers_Color].m_apiSpecific);

        if (!this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
            CGxTex* color = this->m_textureTarget[GxBuffers_Color].m_texture;
            D3D11_TEXTURE2D_DESC desc;

            if (this->m_rtDepthTexture) {
                this->m_rtDepthTexture->GetDesc(&desc);

                if (desc.Width != color->m_width || desc.Height != color->m_height) {
                    SafeRelease(this->m_rtDepthDsv);
                    SafeRelease(this->m_rtDepthTexture);
                }
            }

            // ponytail: one scratch depth, recreated on every size change; cache per size if RT ping-pong shows up.
            if (!this->m_rtDepthTexture) {
                this->ICreateDepth(color->m_width, color->m_height, 1, this->m_rtDepthTexture, this->m_rtDepthDsv, nullptr);
            }

            dsv = this->m_rtDepthDsv;
        }
    }

    if (this->m_textureTarget[GxBuffers_Depth].m_apiSpecific) {
        dsv = static_cast<ID3D11DepthStencilView*>(this->m_textureTarget[GxBuffers_Depth].m_apiSpecific);
    }

    this->m_curRtv = rtv;
    this->m_curDsv = dsv;
    this->m_d3dContext->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
}

void CGxDeviceD3d11::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (!this->m_context) {
        return;
    }

    CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];

    if (target.m_texture == texture && target.m_plane == plane) {
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
                target.m_apiSpecific = api->m_rtv[plane < 6 ? plane : 0];
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
        this->m_savedDsv = this->m_curDsv;
    }
}

void CGxDeviceD3d11::DeviceRestoreDepthSurface() {
    if (!this->m_context) {
        return;
    }

    this->m_curDsv = this->m_savedDsv;
    this->m_d3dContext->OMSetRenderTargets(this->m_curRtv ? 1 : 0, this->m_curRtv ? &this->m_curRtv : nullptr, this->m_curDsv);
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
        this->m_d3dContext->OMSetRenderTargets(this->m_curRtv ? 1 : 0, this->m_curRtv ? &this->m_curRtv : nullptr, this->m_curDsv);
    }
}

// The single-sampled back buffer, or a resolve of the MSAA one (4.3.4 keeps the same copy at +0x2820).
ID3D11Texture2D* CGxDeviceD3d11::IResolvedBackBuffer() {
    D3D11_TEXTURE2D_DESC desc;
    this->m_backBuffer->GetDesc(&desc);

    if (desc.SampleDesc.Count <= 1) {
        return this->m_backBuffer;
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
        case GxWM_KillFocus:
            this->m_windowFocus = 0;
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

        CGxDevice::ScenePresent();
        // Software cursor (no hardware cursor in D3D11). Draws through the normal path, so it shows from phase 2 on.
        this->ICursorDraw();

        if (scrShotClick) {
            this->DeviceScreenShot();
        }

        this->WaitForFPSCap();

        uint32_t vsync = this->m_format.m_vsync;
        UINT interval = vsync ? (this->m_format.m_window ? 1 : (vsync > 4 ? 4 : vsync)) : 0;
        HRESULT hr = this->m_swapChain->Present(interval, 0);

        if (FAILED(hr)) {
            CGxDevice::Log("CGxDeviceD3d11::ScenePresent(): Present failed: 0x%08X", static_cast<uint32_t>(hr));
            this->m_context = 0;
        }
    }

    this->ShaderConstantsClear();
}

// TODO(phase 5): D3D9 Clear only clears the viewport rect; this clears the whole view. ID3D11DeviceContext1::ClearView
// takes rects if a partial clear turns out to matter (model frames clear depth inside the UI).
void CGxDeviceD3d11::SceneClear(uint32_t mask, CImVector color) {
    if (!this->m_context) {
        return;
    }

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }

    if ((mask & 0x1) && this->m_curRtv) {
        const float rgba[4] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
        this->m_d3dContext->ClearRenderTargetView(this->m_curRtv, rgba);
    }

    if ((mask & 0x2) && this->m_curDsv) {
        this->m_d3dContext->ClearDepthStencilView(this->m_curDsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
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
        pool->unk_001C = 0;
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
        uint32_t index = (buf->m_itemSize + pool->unk_001C - 1) - (buf->m_itemSize + pool->unk_001C - 1) % buf->m_itemSize;

        if (index + buf->m_size > static_cast<uint32_t>(pool->m_size)) {
            api->m_discard = 1;
            pool->Discard();
            buf->m_index = 0;
            pool->unk_001C = buf->m_size;
        } else {
            buf->m_index = index;
            pool->unk_001C = index + buf->m_size;
        }
    }

    return api->m_shadow + buf->m_index;
}

void CGxDeviceD3d11::IBufUnlock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;
    EmergencyMem& emergencyMem = this->m_emergencyMem[pool->m_target];

    if (emergencyMem.m_lock) {
        emergencyMem.Unlock();
        buf->unk_001D = 0;
        this->m_intF5C = 1;
        return;
    }

    PoolApi* api = static_cast<PoolApi*>(pool->m_apiSpecific);

    if (!api || !buf->m_size) {
        buf->unk_001D = api != nullptr;
        return;
    }

    const char* src = api->m_shadow + buf->m_index;

    if (pool->m_usage == GxPoolUsage_Static) {
        D3D11_BOX box = { buf->m_index, 0, 0, buf->m_index + buf->m_size, 1, 1 };
        this->m_d3dContext->UpdateSubresource(api->m_buffer, 0, &box, src, 0, 0);
        buf->unk_001D = 1;
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

    buf->unk_001D = SUCCEEDED(hr);
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

// TODO(phase 2): build the D3D11_INPUT_ELEMENT_DESC list from m_primVertexFormat / m_primVertexFormatAttrib, hash it
// with the bound VS input signature (PLAN contract) and IASetInputLayout. Nothing is drawn visibly until then.
void CGxDeviceD3d11::IStateSyncInputLayout() {
}

// TODO(phase 2): copy the dirty ranges of CGxDevice::s_shadowConstants into m_constantBuffers (Map DISCARD), as 4.3.4
// FUN_00871570 does, plus the int/bool/alpha-test tail.
void CGxDeviceD3d11::IShaderConstantsFlush() {
}

// TODO(phase 2): set dirty bits only and resolve them into the 5 hashed state objects (rasterizer / blend /
// depth-stencil / sampler / input layout) at draw time, like 4.3.4 IRsSendToHw 0x0086FF70. Textures (PS SRVs),
// shaders and the scissor rect are bound from here too.
void CGxDeviceD3d11::IRsSendToHw(EGxRenderState which) {
}

void CGxDeviceD3d11::IStateSync() {
    this->IShaderConstantsFlush();
    this->IRsSync(0);

    // TODO(phase 3): when no vertex shader is bound, feed lights / material / transforms to the FF shaders
    // (the CGxDeviceD3d::IStateSyncLights / Material / Xforms equivalents).

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

    this->IStateSync();

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
// Textures

void CGxDeviceD3d11::ITexRelease(CGxTex* texId) {
    TexApi* api = static_cast<TexApi*>(texId->m_apiSpecificData);

    if (!api) {
        return;
    }

    for (uint32_t i = 0; i < 6; i++) {
        SafeRelease(api->m_rtv[i]);
    }

    SafeRelease(api->m_srv);
    SafeRelease(api->m_dsv);
    SafeRelease(api->m_texture);
    delete api;
    texId->m_apiSpecificData = nullptr;
}

// 4.3.4 ITexCreate (0x0086B0C0) shape: colour textures typed, D24X8 typeless with a DSV and an SRV.
void CGxDeviceD3d11::ITexCreate(CGxTex* texId) {
    this->ITexRelease(texId);

    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    texId->m_format = s_GxTexFormatToUse[texId->m_format];

    bool cube = texId->m_target == GxTex_CubeMap;
    bool depth = texId->m_format == GxTex_D24X8;
    bool renderTarget = texId->m_flags.m_renderTarget;
    bool generateMips = texId->m_flags.m_generateMipMaps && !depth;

    D3D11_TEXTURE2D_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Width = width ? width : 1;
    desc.Height = height ? height : 1;
    desc.MipLevels = depth ? 1 : (generateMips ? 0 : mipCount - baseMip);
    desc.ArraySize = cube ? 6 : 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

    if (depth) {
        desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    } else if (renderTarget || generateMips) {
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    }

    if (generateMips) {
        desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }

    desc.Format = s_GxTexFmtToDxgiFmt[texId->m_format];

    ID3D11Texture2D* texture = nullptr;

    if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &texture))) {
        if (renderTarget || depth) {
            return;
        }

        s_GxTexFormatToUse[texId->m_format] = s_tolerableTexFmtMapping[texId->m_format];
        texId->m_format = s_GxTexFormatToUse[texId->m_format];
        desc.Format = s_GxTexFmtToDxgiFmt[texId->m_format];

        if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &texture))) {
            return;
        }
    }

    texture->GetDesc(&desc);

    TexApi* api = new TexApi;
    memset(api, 0, sizeof(*api));
    api->m_texture = texture;
    api->m_levels = desc.MipLevels;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    memset(&srvDesc, 0, sizeof(srvDesc));
    srvDesc.Format = depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : desc.Format;

    if (cube) {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = desc.MipLevels;
    } else {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
    }

    this->m_d3dDevice->CreateShaderResourceView(texture, &srvDesc, &api->m_srv);

    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
        memset(&dsvDesc, 0, sizeof(dsvDesc));
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        this->m_d3dDevice->CreateDepthStencilView(texture, &dsvDesc, &api->m_dsv);
    } else if (renderTarget) {
        for (uint32_t face = 0; face < desc.ArraySize; face++) {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
            memset(&rtvDesc, 0, sizeof(rtvDesc));
            rtvDesc.Format = desc.Format;

            if (cube) {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.FirstArraySlice = face;
                rtvDesc.Texture2DArray.ArraySize = 1;
            } else {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            }

            this->m_d3dDevice->CreateRenderTargetView(texture, &rtvDesc, &api->m_rtv[face]);
        }
    }

    texId->m_apiSpecificData = api;
    texId->m_needsCreation = 0;
}

// CGxDeviceD3d::ITexUpload with LockRect replaced by Blit into a scratch block + UpdateSubresource.
void CGxDeviceD3d11::ITexUpload(CGxTex* texId) {
    uint32_t texelStrideInBytes;
    const void* texels = nullptr;
    texId->m_userFunc(GxTex_Lock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    TexApi* api = static_cast<TexApi*>(texId->m_apiSpecificData);
    int32_t compressed = texId->m_dataFormat >= GxTex_Dxt1 && texId->m_dataFormat <= GxTex_Dxt5;
    int32_t destCompressed = texId->m_format >= GxTex_Dxt1 && texId->m_format <= GxTex_Dxt5;
    uint32_t faceCount = texId->m_target == GxTex_CubeMap ? 6 : 1;

    for (uint32_t face = 0; face < faceCount; face++) {
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        uint32_t level = 0;

        for (uint32_t mip = baseMip; mip != mipCount && level < api->m_levels; mip++, level++) {
            texels = nullptr;
            texId->m_userFunc(GxTex_Latch, mipWidth, mipHeight, face, mip, texId->m_userArg, texelStrideInBytes, texels);

            if (texels && !texId->m_flags.m_renderTarget) {
                RECT rect;
                rect.left = texId->m_updateRect.minX >> mip;
                rect.top = texId->m_updateRect.minY >> mip;
                rect.bottom = rect.top + 1 > texId->m_updateRect.maxY >> mip ? rect.top + 1 : texId->m_updateRect.maxY >> mip;
                rect.right = rect.left + 1 > texId->m_updateRect.maxX >> mip ? rect.left + 1 : texId->m_updateRect.maxX >> mip;

                if (destCompressed) {
                    rect.left &= ~3;
                    rect.top &= ~3;
                    rect.right = (rect.right + 3) & ~3;
                    rect.bottom = (rect.bottom + 3) & ~3;

                    if (rect.bottom > static_cast<int32_t>(mipHeight)) {
                        rect.bottom = mipHeight;
                    }

                    if (rect.right > static_cast<int32_t>(mipWidth)) {
                        rect.right = mipWidth;
                    }
                }

                const uint8_t* src = static_cast<const uint8_t*>(texels);

                if (!texId->m_flags.m_bit15) {
                    EGxTexFormat dataFormat = texId->m_dataFormat;

                    if (compressed) {
                        src += s_texFormatBytesPerBlock[dataFormat] * (rect.left >> 2) + texelStrideInBytes * (rect.top >> 2);
                    } else {
                        src += ((rect.left * s_texFormatBitDepth[dataFormat]) >> 3) + texelStrideInBytes * rect.top;
                    }
                }

                C2iVector size;
                size.x = rect.right - rect.left;
                size.y = rect.bottom - rect.top;

                uint32_t pitch;
                uint32_t rows;

                if (destCompressed) {
                    pitch = ((size.x + 3) >> 2) * s_texFormatBytesPerBlock[texId->m_format];
                    rows = (size.y + 3) >> 2;
                } else {
                    pitch = (size.x * s_texFormatBitDepth[texId->m_format] + 7) >> 3;
                    rows = size.y;
                }

                void* scratch = malloc(pitch * rows);

                if (!scratch) {
                    goto done;
                }

                Blit(size, BlitAlpha_0, src, texelStrideInBytes, GxGetBlitFormat(texId->m_dataFormat), scratch, pitch, GxGetBlitFormat(texId->m_format));

                D3D11_BOX box = { static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0, static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1 };
                this->m_d3dContext->UpdateSubresource(api->m_texture, D3D11CalcSubresource(level, face, api->m_levels), &box, scratch, pitch, 0);
                free(scratch);
            }

            mipHeight >>= 1;
            mipWidth >>= 1;

            if (!mipWidth) {
                mipWidth = 1;
            }

            if (!mipHeight) {
                mipHeight = 1;
            }
        }
    }

    if (texId->m_flags.m_generateMipMaps && api->m_srv && api->m_levels > 1) {
        this->m_d3dContext->GenerateMips(api->m_srv);
    }

done:
    texels = nullptr;
    texId->m_userFunc(GxTex_Unlock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
}

void CGxDeviceD3d11::ITexMarkAsUpdated(CGxTex* texId) {
    if (!texId->m_needsUpdate || !this->m_context) {
        return;
    }

    if (texId->m_needsCreation || !texId->m_apiSpecificData) {
        this->ITexCreate(texId);
    }

    if (!texId->m_needsCreation && texId->m_apiSpecificData) {
        if (texId->m_userFunc) {
            this->ITexUpload(texId);
        }

        CGxDevice::ITexMarkAsUpdated(texId);
    }
}

void CGxDeviceD3d11::ITexForceRecreation(int32_t all) {
    for (CGxTex* texId = this->m_textures.Head(); texId; texId = this->m_textures.Next(texId)) {
        if (!texId->m_apiSpecificData || !(all || texId->m_flags.m_renderTarget)) {
            continue;
        }

        static CiRect s_updateRect;

        // Unbind it if it is a current target, so no stale view stays bound.
        for (uint32_t i = 0; i < 2; i++) {
            if (this->m_textureTarget[i].m_texture == texId) {
                this->m_textureTarget[i].m_apiSpecific = nullptr;
            }
        }

        this->ITexRelease(texId);
        texId->m_needsCreation = 1;
        this->TexMarkForUpdate(texId, s_updateRect, 0);

        uint32_t texelStrideInBytes;
        const void* texels;
        texId->m_userFunc(GxTex_3, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
    }

    if (this->m_hwnd && this->m_d3dDevice) {
        this->NotifyOnTextureRecreation();
    }
}

void CGxDeviceD3d11::TexDestroy(CGxTex* texId) {
    for (uint32_t i = 0; i < 2; i++) {
        if (this->m_textureTarget[i].m_texture == texId && this->m_textureTarget[i].m_apiSpecific) {
            this->m_textureTarget[i].m_apiSpecific = nullptr;

            if (this->m_d3dContext) {
                this->IBindRenderTargets();
            }
        }
    }

    this->ITexRelease(texId);
    CGxDevice::TexDestroy(texId);
}

void CGxDeviceD3d11::DeviceSetBaseMipLevel(uint32_t level) {
    this->ITexForceRecreation(1);
    CGxDevice::DeviceSetBaseMipLevel(level);
}

void CGxDeviceD3d11::DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane) {
    if (!this->m_context || !source || !dest) {
        return;
    }

    if (source->m_needsCreation || !source->m_apiSpecificData) {
        this->ITexCreate(source);
    }

    if (dest->m_needsCreation || !dest->m_apiSpecificData) {
        this->ITexCreate(dest);
    }

    TexApi* src = static_cast<TexApi*>(source->m_apiSpecificData);
    TexApi* dst = static_cast<TexApi*>(dest->m_apiSpecificData);

    if (src && dst) {
        this->m_d3dContext->CopySubresourceRegion(dst->m_texture, D3D11CalcSubresource(0, destPlane, dst->m_levels), 0, 0, 0, src->m_texture, D3D11CalcSubresource(0, sourcePlane, src->m_levels), nullptr);
    }
}

// Back buffer region -> texture (D3D9 StretchRect with equal rects).
int32_t CGxDeviceD3d11::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    if (!this->m_context) {
        return 0;
    }

    if (dest->m_needsCreation || !dest->m_apiSpecificData) {
        this->ITexCreate(dest);
    }

    TexApi* api = static_cast<TexApi*>(dest->m_apiSpecificData);
    ID3D11Texture2D* source = this->IResolvedBackBuffer();

    if (dest->m_needsCreation || !api || !source) {
        return 0;
    }

    D3D11_BOX box = { static_cast<UINT>(sourcePos.x), static_cast<UINT>(sourcePos.y), 0, static_cast<UINT>(sourcePos.x + size.x), static_cast<UINT>(sourcePos.y + size.y), 1 };
    this->m_d3dContext->CopySubresourceRegion(api->m_texture, D3D11CalcSubresource(level, plane, api->m_levels), destPos.x, destPos.y, 0, source, 0, &box);

    return 1;
}

// Equal-size stretches are copies; scaled ones need a blit shader.
int32_t CGxDeviceD3d11::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    if (!this->m_context) {
        return 0;
    }

    if (destRect && sourceRect && destRect->maxX - destRect->minX == sourceRect->maxX - sourceRect->minX && destRect->maxY - destRect->minY == sourceRect->maxY - sourceRect->minY) {
        C2iVector destPos;
        destPos.x = destRect->minX;
        destPos.y = destRect->minY;
        C2iVector sourcePos;
        sourcePos.x = sourceRect->minX;
        sourcePos.y = sourceRect->minY;
        C2iVector size;
        size.x = sourceRect->maxX - sourceRect->minX;
        size.y = sourceRect->maxY - sourceRect->minY;

        return this->TexCopy(dest, destPos, sourcePos, size, level, face);
    }

    // TODO(phase 4): scaled stretch through a full-screen StretchRect shader (4.3.4 ships StretchRect.bls).
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Read back

void CGxDeviceD3d11::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    this->ClampRectToWindow(rect);

    uint32_t width = rect.maxX - rect.minX;
    uint32_t height = rect.maxY - rect.minY;
    pixels.SetCount(width * height);

    if (!this->m_context || !width || !height) {
        return;
    }

    ID3D11Texture2D* source = this->IResolvedBackBuffer();

    if (!source) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;

    if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &staging))) {
        return;
    }

    D3D11_BOX box = { static_cast<UINT>(rect.minX), static_cast<UINT>(rect.minY), 0, static_cast<UINT>(rect.maxX), static_cast<UINT>(rect.maxY), 1 };
    this->m_d3dContext->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped;

    if (SUCCEEDED(this->m_d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData);
            CImVector* dest = pixels.Ptr();

            for (uint32_t y = 0; y < height; y++, row += mapped.RowPitch, dest += width) {
                memcpy(dest, row, sizeof(CImVector) * width);
            }
        } else {
            memset(pixels.Ptr(), 0xFF, sizeof(CImVector) * width * height);
        }

        this->m_d3dContext->Unmap(staging, 0);
    }

    staging->Release();
}

// Works where 12340 D3D9 returns zeros (0x006A1950): copies the readable depth to a staging texture. Depth-stencil
// resources can only be copied whole, so the full surface is copied and the rect cut out on the CPU.
// TODO(phase 5): MSAA depth (needs the resolve from the depth contract); returns zeros for now.
void CGxDeviceD3d11::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    this->ClampRectToWindow(rect);

    uint32_t width = rect.maxX - rect.minX;
    uint32_t height = rect.maxY - rect.minY;
    depths.SetCount(width * height);
    memset(depths.Ptr(), 0, sizeof(float) * width * height);

    if (!this->m_context || !this->m_depthTexture || !width || !height) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc;
    this->m_depthTexture->GetDesc(&desc);

    if (desc.SampleDesc.Count > 1) {
        return;
    }

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;

    if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &staging))) {
        return;
    }

    this->m_d3dContext->CopyResource(staging, this->m_depthTexture);

    D3D11_MAPPED_SUBRESOURCE mapped;

    if (SUCCEEDED(this->m_d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        float* dest = depths.Ptr();

        for (uint32_t y = 0; y < height && rect.minY + y < desc.Height; y++) {
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * (rect.minY + y);

            for (uint32_t x = 0; x < width && rect.minX + x < desc.Width; x++) {
                uint32_t px = rect.minX + x;
                float value;

                switch (desc.Format) {
                    case DXGI_FORMAT_R32_TYPELESS:
                        value = reinterpret_cast<const float*>(row)[px];
                        break;

                    case DXGI_FORMAT_R16_TYPELESS:
                        value = reinterpret_cast<const uint16_t*>(row)[px] / 65535.0f;
                        break;

                    default:
                        value = (reinterpret_cast<const uint32_t*>(row)[px] & 0xFFFFFF) / 16777215.0f;
                        break;
                }

                dest[y * width + x] = value;
            }
        }

        this->m_d3dContext->Unmap(staging, 0);
    }

    staging->Release();
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

    do {
        hr = this->m_d3dContext->GetData(d3dQuery, &samples, sizeof(samples), 0);
    } while (hr == S_FALSE);

    data = static_cast<uint32_t>(samples);

    return hr == S_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Shaders

// Shader files are read as on D3D9 (SM3 .bls via IShaderLoad); no API objects are made yet.
// TODO(phase 4): create ID3D11VertexShader / ID3D11PixelShader from the shader-xlate output (keep the VS bytecode
// for input-layout creation), set m_valid.
void CGxDeviceD3d11::IShaderCreate(CGxShader* shader) {
}

void CGxDeviceD3d11::ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations) {
    CGxDevice::ShaderCreate(shaders, target, path, name, permutations);

    if (permutations == 1 && !shaders[0]->m_loaded) {
        this->IShaderCreate(shaders[0]);
    }
}

void CGxDeviceD3d11::ShaderDestroy(CGxShader*& shader) {
    if (shader->m_refCount == 1) {
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

    if (shader->m_refCount == 1) {
        if (shader->m_apiSpecific) {
            static_cast<IUnknown*>(shader->m_apiSpecific)->Release();
            shader->m_apiSpecific = nullptr;
        }

        shader->m_loaded = 0;
    }

    this->IShaderLoad(&shader, static_cast<EGxShTarget>(shader->m_target), path, name, 1);
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
