// FSR: the world upscaler of the Vulkan device (Engine/Source/Gx/GxUpscale.h), a port of wxl-fsr's FSR 4 mode
// (/root/wxl-fsr wt/s6 src/Fsr4.cpp) and wxl-light's jitter / motion-vector job (wt/light-fsr4 src/Depth.cpp) onto
// the device's own VkDevice: no DXVK interop, no second device.
//
// FSR 4 is the community Vulkan provider (github.com/dvj5411/fsr4vk, GPLv3, model data from AMD's binary), a run-time
// DLL loaded from <exe folder>\FSR4\x64\ (Wow64) or \x86\ (Wow32) and never linked (NATIVE-PLAN section 0):
//   fsr4Shaders fast   wxlfsr4-fast.dll (zgauthier2000's shader overrides), else wxlfsr4.dll   (wxl-fsr fsr4_shaders)
//   fsr4Shaders stock  wxlfsr4.dll
//   fsr4Provider X     X instead, any provider DLL with the same ffx exports (a 4.1.2 build drops in here)
//   fsr4Version V      the FSR version the provider offers to run, "4.1.1" (wxl-fsr mode fsr4) or "4.0.2" (fsr4-402)
// The provider needs device features the device otherwise never enables (descriptor buffers, mutable descriptors,
// compute shader derivatives), so it is asked for them when the device is made (PrepareDevice): fsrMode is read
// once, at the device's creation; fsrScale and turning fsrMode back off work live. A missing DLL, a GPU or driver
// that cannot run it, or a refused device: logged, the device made as without it, no upscale.
//
// A frame (fsrMode fsr4, fsrScale < 1): BeginWorld shrinks the world viewport to the render size at its top-left and
// jitters every perspective projection (CGxDeviceVulkan::m_projJitter, Halton 2/3 as ffxFsr3GetJitterOffset);
// the world and its post passes (vfog, SSAO, tonemap: they all read the world viewport) draw at the render size;
// EndWorld (UpscaleIn.comp) packs the scene's colour, FSR's inverted depth and camera motion vectors from the scene
// depth copy, dispatches FSR 4 on the frame's command buffer and writes its output into the full viewport
// (UpscaleOut.comp + frame::CopyIntoScene). A frame FSR cannot take is stretched bilinearly instead.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

class CGxDeviceVulkan;

namespace gxvk::vk::upscale
{
    /// context CreateDevice: fsrMode asks for FSR 4 (registers the CVars; read once per device).
    bool Wanted();

    /**
     * context CreateDevice: the create info with what the provider needs added, or null (not wanted, no provider,
     * refused: logged). Valid until DevicePrepared().
     */
    const VkDeviceCreateInfo* PrepareDevice(VkInstance instance, VkPhysicalDevice physical, const VkDeviceCreateInfo& info, uint32_t apiVersion);

    /// The device was (`created`) or was not made from PrepareDevice's create info; frees the provider's copy of it.
    void DevicePrepared(bool created);

    /// Sets g_gxUpscale when the device was made for FSR 4. After the draw path is up.
    void Init(CGxDeviceVulkan* device);

    /// Everything, after the device is idle and before texture::Shutdown(); clears g_gxUpscale.
    void Shutdown();
}
