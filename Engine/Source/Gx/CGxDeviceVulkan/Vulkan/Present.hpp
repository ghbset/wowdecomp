// The present pass: the scene image onto the acquired swapchain image, one full-screen triangle
// through the gamma ramp (D3D9's SetGammaRamp, applied by the engine in fullscreen).
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

// Owns its render pass (the swapchain's format, UNDEFINED -> PRESENT_SRC), its pipeline, a
// framebuffer per swapchain image, one descriptor set and one gamma uniform buffer per frame slot.
// The frame calls Rebuild() whenever the swapchain changes and Record() once per presented frame.
namespace gxvk::vk::present
{
    /**
     * The shader modules, the layouts, the sampler, the per-slot gamma buffers and sets. After
     * memory::Init(). Idempotent. A Rebuild() that came before Init() is applied here.
     */
    bool Init();

    /// Everything, after the device is idle.
    void Shutdown();

    /**
     * The objects that name the swapchain and the scene: the render pass and pipeline when the
     * format changed, the framebuffers over the swapchain's views, the sets' scene binding. The
     * caller has made the device idle when an earlier swapchain stood.
     *
     * @param VkFormat format : the swapchain's
     * @param VkExtent2D extent : the swapchain's
     * @param const VkImageView* views, uint32 count : the swapchain images' views
     * @param VkImageView scene : the scene image's view
     * @return bool ok : false when the pass cannot be built (logged); Record() then draws nothing
     */
    bool Rebuild(VkFormat format, VkExtent2D extent, const VkImageView* views, uint32_t count, VkImageView scene);

    /// The framebuffers; the pass and pipeline stay for the next Rebuild() of the same format.
    void Drop();

    /**
     * Records the pass on the swapchain image `image`: the scene sampled 1:1 through the gamma
     * ramp when it applies. Outside any render pass on `cmd`.
     *
     * @param uint32 slot : frame::Slot(), whose gamma buffer is written if the ramp changed
     * @return bool recorded
     */
    bool Record(VkCommandBuffer cmd, uint32_t image, uint32_t slot);

    /**
     * The gamma ramp, as the engine's CGxGammaRamp holds it (three 256-entry tables of 16-bit
     * values), and whether it applies to the presented frame.
     */
    void SetGamma(const uint16_t* red, const uint16_t* green, const uint16_t* blue, bool apply);

    /// Whether Init() succeeded and a Rebuild() stands: Record() will draw.
    bool Ready();
}
