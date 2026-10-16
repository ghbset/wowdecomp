// The Vulkan backend: construction, the device lifecycle (create, set format, destroy), the gamma
// ramp and the crash-log lines. The window is CGxDeviceVulkanWindow.cpp, the caps
// CGxDeviceVulkanCaps.cpp, the frame CGxDeviceVulkanScene.cpp, the pools CGxDeviceVulkanPool.cpp,
// the textures CGxDeviceVulkanTexture.cpp, the shaders CGxDeviceVulkanShader.cpp, the draw
// CGxDeviceVulkanDraw.cpp, the render targets CGxDeviceVulkanTarget.cpp, the queries
// CGxDeviceVulkanQuery.cpp, the cursor CGxDeviceVulkanCursor.cpp, the rest CGxDeviceVulkanStubs.cpp.
#include <cstdint>
#include <cstring>
#include <new>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Manifest.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Present.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Queries.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Shaders.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Error.h"
#include "Engine/Source/Storm/H/storm.h"

namespace context   = gxvk::vk::context;
namespace frame     = gxvk::vk::frame;
namespace manifest  = gxvk::vk::manifest;
namespace memory    = gxvk::vk::memory;
namespace pipelines = gxvk::vk::pipelines;
namespace present   = gxvk::vk::present;
namespace queries   = gxvk::vk::queries;
namespace shaders   = gxvk::vk::shaders;
namespace texture   = gxvk::vk::texture;

// Mirrors CreateCGxDeviceD3d (0x0068E0B0) and CreateCGxDeviceOpenGl (0x0068BF20): the engine's
// allocator, placement new.
CGxDevice* CreateCGxDeviceVulkan() {
    void* mem = SMemAlloc(sizeof(CGxDeviceVulkan), __FILE__, __LINE__, 0);

    if (!mem) {
        return nullptr;
    }

    return new (mem) CGxDeviceVulkan();
}

// Mirrors CGxDeviceD3d::CGxDeviceD3d (0x0068DE70): the API tag, the colour order the engine swizzles
// vertex colours to (D3D's ARGB, which is what the B8G8R8A8 swapchain and the D3DCOLOR vertex fetch
// expect), the stream pools.
CGxDeviceVulkan::CGxDeviceVulkan() {
    this->m_api = GxApi_Vulkan;
    this->m_hwnd = nullptr;
    this->m_hwndClass = 0;
    this->m_ownhwnd = 0;
    this->m_vulkanPending = 0;
    this->m_drawPath = 0;
    this->m_filling = nullptr;
    this->m_manifestTried = 0;
    this->m_hcursor = nullptr;
    this->m_hwCursorDirty = 0;
    // D3D's constant registers start at zero.
    memset(this->m_vertexRegs, 0, sizeof(this->m_vertexRegs));
    memset(this->m_pixelRegs, 0, sizeof(this->m_pixelRegs));
    this->m_caps.m_colorFormat = GxCF_argb;
    this->m_caps.unk_0018 = 1;
    this->DeviceCreatePools();
    this->DeviceCreateStreamBufs();
}

CGxDeviceVulkan::~CGxDeviceVulkan() {
}

// Mirrors CGxDeviceD3d::DeviceCreate (0x00690750): the system gamma ramp, the window class, then the
// base DeviceCreate, which stores the engine's window proc and calls DeviceSetFormat.
int32_t CGxDeviceVulkan::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;

    HDC dc = GetDC(nullptr);

    if (GetDeviceGammaRamp(dc, &this->m_systemGammaRamp) && &this->m_gammaRamp != &this->m_systemGammaRamp) {
        this->m_gammaRamp = this->m_systemGammaRamp;
    }

    ReleaseDC(nullptr, dc);

    WNDCLASSEXA windowClass;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = CGxDeviceVulkan::WindowProcVulkan;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "GxWindowClassVulkan";
    windowClass.hIcon = static_cast<HICON>(LoadImageA(windowClass.hInstance, "BlizzardIcon.ico", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    windowClass.hCursor = LoadCursorA(windowClass.hInstance, "BlizzardCursor.cur");

    if (!windowClass.hCursor) {
        windowClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    }

    this->m_hwndClass = RegisterClassExA(&windowClass);

    if (!this->m_hwndClass) {
        char* error = OsGetLastErrorStr();
        CGxDevice::Log("CGxDeviceVulkan::DeviceCreate(): RegisterClassExA() failed: %s", error);
        OsFreeLastErrorStr(error);
        this->DeviceDestroy();
        return 0;
    }

    if (CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

// Mirrors CGxDeviceD3d::DeviceCreate (0x00690830): a window the caller owns.
int32_t CGxDeviceVulkan::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_ownhwnd = 0;
    CGxDevice::DeviceCreate(window, format);
    this->m_hwnd = static_cast<HWND>(window);

    if (this->ICreateVulkan(format) && CGxDevice::DeviceCreate(window, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

// Mirrors CGxDeviceD3d::DeviceDestroy (0x006905F0).
void CGxDeviceVulkan::DeviceDestroy() {
    CGxDevice::DeviceDestroy();
    this->IDestroyVulkan();

    if (this->m_hwnd && this->m_ownhwnd) {
        DestroyWindow(this->m_hwnd);
        this->m_hwnd = nullptr;
    }

    if (this->m_hwndClass) {
        UnregisterClassA(reinterpret_cast<const char*>(static_cast<uintptr_t>(this->m_hwndClass)), GetModuleHandleA(nullptr));
        this->m_hwndClass = 0;
    }
}

// Mirrors CGxDeviceD3d::DeviceSetFormat (0x006904D0): a new window for the new format. The Vulkan
// instance and device survive the window, and with them the pools' buffers and the pipelines; the
// surface and the swapchain are rebuilt on the new window.
int32_t CGxDeviceVulkan::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceVulkan::DeviceSetFormat():");
    this->Log(format);

    if (this->m_hwnd) {
        ShowWindow(this->m_hwnd, SW_HIDE);
    }

    this->IReleaseVulkanWindow();
    this->IDestroyWindow();
    this->m_format = format;

    CGxFormat createFormat = format;

    if (this->ICreateWindow(createFormat) && this->ICreateVulkan(createFormat) && CGxDevice::DeviceSetFormat(createFormat)) {
        this->ISetWindowFocus(1);
        return 1;
    }

    CGxDevice::Log("CGxDeviceVulkan::DeviceSetFormat(): unable to set format!");
    this->IReleaseVulkanWindow();
    this->IDestroyWindow();

    return 0;
}

// The Vulkan half of CGxDeviceD3d::ICreateD3dDevice (0x0068F3D0): the context on the window, the
// draw path, then the caps, the defaults, the gamma ramp and the cursor, as D3D does once its device
// exists.
int32_t CGxDeviceVulkan::ICreateVulkan(const CGxFormat& format) {
    // Validation is a developer switch: GX_VULKAN_VALIDATION=1 in the environment asks for the
    // Khronos layer; a layer that is not installed is a log line, not a failure.
    char validation[8];
    DWORD length = GetEnvironmentVariableA("GX_VULKAN_VALIDATION", validation, sizeof(validation));
    context::SetValidation(length > 0 && length < sizeof(validation) && validation[0] != '0');

    // Before the swapchain is built: the format's vsync picks its present mode.
    context::SetVsync(format.m_vsync != 0);

    this->m_vulkanPending = 0;

    if (context::Live()) {
        // A device that is up and a window that is new: the format changed, or the engine restarted
        // its graphics. Rebind sees the HWND differ and rebuilds surface and swapchain.
        context::Rebind(this->m_hwnd);
        this->ICreateDrawPath();
    } else {
        switch (context::Bring(this->m_hwnd)) {
            case context::Attempt::Declined:
                CGxDevice::Log("CGxDeviceVulkan::ICreateVulkan(): the Vulkan device could not be brought up (the lines above say why)");
                return 0;

            case context::Attempt::Retry:
                // The window is not measurable yet (minimised): the device comes up on the first
                // present that finds it. The caps below are the table's defaults until then.
                CGxDevice::Log("CGxDeviceVulkan::ICreateVulkan(): the window is not measurable yet; the Vulkan device comes up at the first present");
                this->m_vulkanPending = 1;
                break;

            default:
                this->ICreateDrawPath();

                // Without the draw path or the translated shaders nothing of the world is drawn:
                // decline, so ConsoleDeviceInitialize can fall back to D3d9.
                if (!this->m_drawPath || !manifest::Loaded()) {
                    CGxDevice::Log("CGxDeviceVulkan::ICreateVulkan(): no draw path or no shader manifest; declining");
                    return 0;
                }
                break;
        }
    }

    this->m_context = 1;
    this->ISetCaps(format);
    this->Log(this->m_caps);

    this->IStateSetDefaults();
    this->DeviceSetGamma(this->m_gammaRamp);
    this->ICursorCreate(format);

    return 1;
}

// Device memory, the textures, the translated shaders' manifest and modules, the pipelines, the
// present pass and the queries, once the VkDevice exists. Without them the pools answer locks with
// the engine's emergency memory, the textures stay pending and nothing reaches the window. The
// manifest is looked for once per device: without it only the backend's own programs draw.
void CGxDeviceVulkan::ICreateDrawPath() {
    if (this->m_drawPath || !context::Live()) {
        return;
    }

    char folder[512];
    this->IShaderFolder(folder, sizeof(folder));

    if (!this->m_manifestTried) {
        this->m_manifestTried = 1;
        manifest::Load(folder);
    }

    this->m_drawPath = memory::Init() && texture::Init() && shaders::Init(folder) && pipelines::Init() && present::Init() && queries::Init();

    if (!this->m_drawPath) {
        CGxDevice::Log("CGxDeviceVulkan::ICreateDrawPath(): device memory, the textures, the shaders, the pipelines, the present pass or the queries could not be brought up (the lines above say why); nothing is drawn");
    }
}

// A bring-up that answered Retry at DeviceSetFormat: tried again from ScenePresent while the window
// is visible, and the caps refined from the device once it exists.
void CGxDeviceVulkan::IBringUpIfPending() {
    if (!this->m_vulkanPending || !this->IWindowVisible()) {
        return;
    }

    switch (context::Bring(this->m_hwnd)) {
        case context::Attempt::Done:
            this->m_vulkanPending = 0;
            this->ICreateDrawPath();
            this->ISetCaps(this->m_format);
            this->Log(this->m_caps);
            break;

        case context::Attempt::Declined:
            this->m_vulkanPending = 0;
            this->m_context = 0;
            CGxDevice::Log("CGxDeviceVulkan::IBringUpIfPending(): the Vulkan device could not be brought up; the context is lost");
            break;

        default:
            break;
    }
}

// Before the window goes (a format change): the frame's swapchain objects, then the surface. The
// instance, the VkDevice, the pools' buffers and the pipelines stay for the next window.
void CGxDeviceVulkan::IReleaseVulkanWindow() {
    frame::ReleaseSwapchain();
    context::ReleaseWindow();
    this->m_context = 0;
}

// Everything, in reverse order of creation: the frame's swapchain objects (which waits for the
// device, and drops the present pass's framebuffers), the pools' buffers and the textures' images,
// the frame's own objects (the retired ones with them), the pipelines, the present pass, the
// queries, the shader modules, the textures' table, device memory, the context, the manifest.
void CGxDeviceVulkan::IDestroyVulkan() {
    this->ICursorDestroy();
    frame::ReleaseSwapchain();
    this->IReleasePools();
    this->IReleaseTextures();
    frame::Destroy();
    pipelines::Shutdown();
    present::Shutdown();
    queries::Shutdown();
    shaders::Shutdown();
    texture::Shutdown();
    memory::Shutdown();
    this->m_drawPath = 0;
    this->m_filling = nullptr;
    context::LogValidationSummary();
    context::Teardown();
    manifest::Unload();
    this->m_manifestTried = 0;
    this->m_context = 0;
    this->m_vulkanPending = 0;
}

// The engine half of CGxDeviceD3d::IStateSetD3DDefaults (0x006A3A60): every render state marked for
// re-send, the vertex streams and the index buffer dirty, the lights reset, the shader constants
// cleared.
void CGxDeviceVulkan::IStateSetDefaults() {
    this->IRsForceUpdate();
    this->IRsSync(0);
    this->m_primVertexDirty = 0xFFFFFFFF;
    this->m_primIndexDirty = 0;
    this->InitLights();
    this->ShaderConstantsClear();
}

void* CGxDeviceVulkan::DeviceWindow() {
    return this->m_hwnd;
}

// Mirrors CGxDeviceD3d::DeviceSetGamma (0x0068E4C0 / 0x0069FE80): the base keeps the ramp; the
// present pass applies it to the frame it shows, in fullscreen only, as D3D's SetGammaRamp applies
// to a fullscreen swap chain alone.
void CGxDeviceVulkan::DeviceSetGamma(float gamma) {
    CGxDevice::DeviceSetGamma(gamma);
    present::SetGamma(this->m_gammaRamp.m_red, this->m_gammaRamp.m_green, this->m_gammaRamp.m_blue, !this->IDevIsWindowed());
}

void CGxDeviceVulkan::DeviceSetGamma(const CGxGammaRamp& ramp) {
    CGxDevice::DeviceSetGamma(ramp);
    present::SetGamma(this->m_gammaRamp.m_red, this->m_gammaRamp.m_green, this->m_gammaRamp.m_blue, !this->IDevIsWindowed());
}

// Mirrors CGxDeviceD3d::DeviceOverride (0x0069FF40) for the one override the shader level depends on.
void CGxDeviceVulkan::DeviceOverride(EGxOverride override, uint32_t value) {
    CGxDevice::DeviceOverride(override, value);

    if (override == GxOverride_PixelShader) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = value;
    }
}

void CGxDeviceVulkan::LogCrashInfo(char* buffer, uint32_t size) {
    char name[256];
    char driver[128];
    context::AdapterText(name, sizeof(name), driver, sizeof(driver));

    char line[512];

    if (name[0]) {
        SStrPrintf(line, sizeof(line), "Adapter: %s\r\n", name);
        SStrPack(buffer, line, size);
        SStrPrintf(line, sizeof(line), "Driver: %s\r\n", driver);
        SStrPack(buffer, line, size);
    } else {
        SStrPack(buffer, "Adapter: no Vulkan device\r\n", size);
    }

    frame::Stats f;
    frame::GetStats(f);
    pipelines::Stats p;
    pipelines::GetStats(p);
    texture::Stats t;
    texture::GetStats(t);
    SStrPrintf(line, sizeof(line), "Frames: %llu submitted, %llu presented, %llu passes (%llu resumed, %llu on render targets); draws: %llu drawn, %llu skipped, %u pipeline(s), %u sampler(s)\r\n",
               static_cast<unsigned long long>(f.frames), static_cast<unsigned long long>(f.presented),
               static_cast<unsigned long long>(f.passes), static_cast<unsigned long long>(f.passResumes),
               static_cast<unsigned long long>(f.targetPasses),
               static_cast<unsigned long long>(p.drawn), static_cast<unsigned long long>(p.skipped), p.pipelines, p.samplers);
    SStrPack(buffer, line, size);

    queries::Stats q;
    queries::GetStats(q);
    SStrPrintf(line, sizeof(line), "Targets: %u depth buffer(s), %u framebuffer(s), %llu scene copies, %llu scene reads; queries: %llu begun, %llu ended, %llu read (%llu waited, %llu flushed), %llu cut by a pass end\r\n",
               f.depthBuffers, f.framebuffers, static_cast<unsigned long long>(f.sceneCopies), static_cast<unsigned long long>(f.sceneReads),
               static_cast<unsigned long long>(q.begun), static_cast<unsigned long long>(q.ended), static_cast<unsigned long long>(q.results),
               static_cast<unsigned long long>(q.waits), static_cast<unsigned long long>(q.flushes), static_cast<unsigned long long>(f.queriesCut));
    SStrPack(buffer, line, size);
    SStrPrintf(line, sizeof(line), "Textures: %u image(s) live (peak %u), %llu region(s) uploaded in %llu batch(es), %llu MiB staged\r\n",
               t.live, t.livePeak, static_cast<unsigned long long>(t.regions), static_cast<unsigned long long>(t.batches),
               static_cast<unsigned long long>(f.stagedBytes >> 20));
    SStrPack(buffer, line, size);

    manifest::Stats m;
    manifest::GetStats(m);
    shaders::Stats sh;
    shaders::GetStats(sh);
    SStrPrintf(line, sizeof(line), "Shaders: manifest %s (%u record(s)), %u translated module(s) loaded, %u failed; %llu uniform slice(s), %llu key(s) refused\r\n",
               manifest::Loaded() ? "loaded" : "absent", m.records, sh.loaded, sh.failed,
               static_cast<unsigned long long>(p.regsSlices), static_cast<unsigned long long>(p.refusedKeys));
    SStrPack(buffer, line, size);
}
