// The Vulkan backend's scene: the present and the clear.
#include <cstdint>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/GxFrameLatency.h"

namespace context = gxvk::vk::context;
namespace frame   = gxvk::vk::frame;

// RENDERTHREAD: renderThread 1 hands each finished frame to a render thread that acquires, submits and
// presents it while the main thread records the next one (Vulkan only; read every frame, live).
static CVar* RenderThreadCVar() {
    static CVar* cvar;

    if (!cvar) {
        cvar = CVar::Register("renderThread", "Vulkan: 1 submits and presents frames on a render thread while the next one is recorded", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    }

    return cvar;
}

// Mirrors CGxDeviceD3d::ScenePresent (0x006A3450), with the frame's present where D3D's Present was
// and the engine half of ISceneBegin (the shader constants cleared) after it. The frame is ended
// whether or not the window is visible: a frame that acquired a swapchain image has to present it;
// a hidden window gets no image and an empty submit.
void CGxDeviceVulkan::ScenePresent() {
    if (this->m_context) {
        int32_t scrShotClick = this->m_scrShotClick;

        // CUSTOM (Badgermilk0 port): performance diagnostics (GxFrameStats). Vulkan has no wait before the present:
        // the GPU time a frame waits on is the fence of the slot it reuses (cpu), or with gxMaxFrameLatency 0 the
        // wait below (present).
        uint64_t frameStart = GxFrameStatsNow();
        CGxDevice::ScenePresent();
        this->ICursorDraw();

        if (scrShotClick) {
            this->DeviceScreenShot();
        }

        uint64_t capStart = GxFrameStatsNow();
        this->WaitForFPSCap();
        uint64_t presentStart = GxFrameStatsNow();

        this->IBringUpIfPending();

        if (this->m_context && context::Live()) {
            CVar* renderThread = RenderThreadCVar();
            frame::SetThreaded(renderThread && renderThread->m_intValue != 0);

            if (frame::Present(this->IWindowVisible() != 0)) {
                // A resize, or a surface that went out of date: the frame has dropped what named the
                // old swapchain; the context rebuilds it on the window.
                context::Rebind(this->m_hwnd);
            }

            // CUSTOM (Badgermilk0 port, GxFrameLatency.h): kFramesInFlight 2 already lets the CPU build one frame while
            // the GPU draws the last, which is gxMaxFrameLatency 1 (deeper queues are not offered). gxFixLag with 0 is
            // the stock full wait: the frame just presented has to finish.
            if (this->m_format.m_fixLag && !g_gxMaxFrameLatency) {
                frame::WaitSerial(frame::Serial() - 1);
            }

            if (frame::Failed()) {
                // The frame stopped for good; the engine sees a lost context, as it would a failed
                // D3D Present.
                this->m_context = 0;
            }
        }

        GxFrameStatsRecord(frameStart, capStart, capStart, presentStart, GxFrameStatsNow());
    }

    this->ShaderConstantsClear();
}

// Mirrors CGxDeviceD3d::SceneClear (0x006A74B0): D3D's Clear clears the viewport rectangle (and the
// scissor's, with the scissor test on) of the current target. Inside the frame's pass that is a
// vkCmdClearAttachments over the same rectangle; before the pass is open the colour becomes the
// pass's clear. The CImVector is a D3DCOLOR: b, g, r, a bytes.
void CGxDeviceVulkan::SceneClear(uint32_t mask, CImVector color) {
    if (!this->m_context) {
        return;
    }

    float colour[4];
    colour[0] = color.r / 255.0f;
    colour[1] = color.g / 255.0f;
    colour[2] = color.b / 255.0f;
    colour[3] = color.a / 255.0f;

    VkViewport viewport;
    VkRect2D rect;

    if (this->IPlaceD3d(viewport, rect)) {
        frame::Clear(mask, colour, &rect);
    } else if (frame::Extent().width == 0 || frame::Extent().height == 0) {
        // No target size known yet: the whole target, as before.
        frame::Clear(mask, colour, nullptr);
    }
    // Else an empty viewport or scissor: D3D clears nothing.
}
