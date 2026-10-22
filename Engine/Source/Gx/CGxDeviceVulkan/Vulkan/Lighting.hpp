// LIGHTING: the Vulkan side of the modern-lighting post passes (Engine/Source/Gx/GxLighting.h), the same
// math as the D3D11 device's (CGxDeviceD3d11/D3d11Lighting.cpp, Lighting/Lighting.hlsl) as compute
// (Shaders/Lighting*.comp), recorded outside the scene's render pass:
//
//   OpaqueDone  the scene depth copied (frame::CopyDepthFromScene), SSAO at half resolution, two blurs, then
//               one pass that multiplies the occlusion into the world viewport of the scene and blends the
//               height fog / scattering over it, written into an RGBA8 image as B, G, R, A and copied into the
//               B8G8R8A8 scene (frame::CopyIntoScene)
//   EndWorld    (GxLighting_Hdr) Vulkan has no float scene -- the scene is 8-bit and every draw pipeline is
//               built against it -- so "hdr" is exposure + bloom + the tonemap over the 8-bit scene
//
// Single-sampled scenes only: with MSAA every pass instance resolves into the scene, over anything written
// outside it, so nothing runs (logged once). Everything is made on first use; with every feature off nothing
// is made and nothing recorded.
#pragma once

class CGxDeviceVulkan;

namespace gxvk::vk::lighting
{
    /// Sets g_gxLighting. After the draw path is up.
    void Init(CGxDeviceVulkan* device);

    /// Everything, after the device is idle and before texture::Shutdown(); clears g_gxLighting.
    void Shutdown();
}
