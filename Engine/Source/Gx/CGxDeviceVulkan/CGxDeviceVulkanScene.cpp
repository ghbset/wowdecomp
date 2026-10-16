// The Vulkan backend's scene: the present and the clear.
#include <cstdint>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/Gx.h"

namespace context = gxvk::vk::context;
namespace frame   = gxvk::vk::frame;

// Mirrors CGxDeviceD3d::ScenePresent (0x006A3450), with the frame's present where D3D's Present was
// and the engine half of ISceneBegin (the shader constants cleared) after it. The frame is ended
// whether or not the window is visible: a frame that acquired a swapchain image has to present it;
// a hidden window gets no image and an empty submit.
void CGxDeviceVulkan::ScenePresent() {
    if (this->m_context) {
        int32_t scrShotClick = this->m_scrShotClick;

        CGxDevice::ScenePresent();
        this->ICursorDraw();

        if (scrShotClick) {
            this->DeviceScreenShot();
        }

        this->WaitForFPSCap();

        this->IBringUpIfPending();

        if (this->m_context && context::Live()) {
            if (frame::Present(this->IWindowVisible() != 0)) {
                // A resize, or a surface that went out of date: the frame has dropped what named the
                // old swapchain; the context rebuilds it on the window.
                context::Rebind(this->m_hwnd);
            }

            if (frame::Failed()) {
                // The frame stopped for good; the engine sees a lost context, as it would a failed
                // D3D Present.
                this->m_context = 0;
            }
        }
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
    } else {
        frame::Clear(mask, colour, nullptr);
    }
}
