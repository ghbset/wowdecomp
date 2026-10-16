// GPUPARTICLES: the M2 particle quad expansion. The CPU writes one compact record per quad
// (GxParticleQuad) into the vertex stream; a compute dispatch recorded into the frame's pre-pass
// command buffer expands each into the four engine-layout vertices, into a per-frame-slot device
// buffer the draw binds in place of the records. The draw itself (programs, fog, blend, alpha test,
// texture stages) is the stock one.
//
// Why compute rather than the vertex shader: particle draws run the client's translated DX9 vertex
// programs (fog, lighting and shadow permutations of the default effect), which read a per-vertex
// position. Expanding in front of them keeps every one of those programs untouched; the pre-pass
// buffer is submitted ahead of the frame's draws, so the render pass never has to break.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::particles
{
    /// The compute pipeline and its layouts. After pipelines::Init(). False (logged) when the
    /// graphics queue cannot run compute or a Vulkan object could not be made: Ready() stays false.
    bool Init();

    /// Everything, after the device is idle. Before memory::Shutdown().
    void Shutdown();

    bool Ready();

    /// Where the expanded vertices of one draw land, and how they are laid out.
    struct Layout
    {
        uint32_t stride;      ///< bytes per vertex
        uint32_t normalOff;   ///< bytes; UINT32_MAX: no normal
        uint32_t colorOff;
        uint32_t uvOff;
    };

    /**
     * Records the expansion of `quads` records at `inOffset` of `in` (a stream pool chunk) into the
     * open frame's pre-pass, and hands back the buffer and offset the draw binds instead.
     *
     * @return bool ok : false when the frame or the memory could not be had (logged once)
     */
    bool Expand(VkBuffer in, VkDeviceSize inOffset, uint32_t quads, const Layout& layout, const float normal[3], VkBuffer& out, VkDeviceSize& outOffset);
}
