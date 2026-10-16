// The device's optional capabilities: what the adapter offered, what was enabled, what a renderer
// built on the device may use.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

// The device is a Vulkan 1.0 one with six core features (Context/Logical.cpp). The instance asks for
// the loader's version up to 1.2, the device is created at min(that, the adapter's), and the optional
// features below are enabled whenever the adapter reports them -- each probed through
// vkGetPhysicalDeviceFeatures2 / Properties2 (core from 1.1), never assumed. None of them changes what
// the device itself draws: its SPIR-V is 1.0 and uses none. They are recorded for whatever is built
// on top of the device.
namespace gxvk::vk
{
    /// What one flag of DeviceCaps::enabled means.
    enum CapBit : uint32_t
    {
        kCapFloat16          = 1u << 0,   ///< shaderFloat16: float16_t arithmetic
        kCapInt8             = 1u << 1,   ///< shaderInt8
        kCapStorage16        = 1u << 2,   ///< storageBuffer16BitAccess: 16-bit members in storage buffers
        kCapUniform16        = 1u << 3,   ///< uniformAndStorageBuffer16BitAccess
        kCapPush16           = 1u << 4,   ///< storagePushConstant16
        kCapInputOutput16    = 1u << 5,   ///< storageInputOutput16 (16-bit varyings; NVIDIA has none)
        kCapInt16            = 1u << 6,   ///< shaderInt16
        kCapStorageImageExt  = 1u << 7,   ///< shaderStorageImageExtendedFormats: R16F, RG16F, R11G11B10F ... storage images
        kCapMultiview        = 1u << 8,   ///< multiview: one draw into several layers, gl_ViewIndex
        kCapLayerFromVertex  = 1u << 9,   ///< VK_EXT_shader_viewport_index_layer: gl_Layer from a vertex shader
        kCapMemoryBudget     = 1u << 10,  ///< VK_EXT_memory_budget: the driver's per-heap budget and usage
        kCapMultiDrawIndirect = 1u << 11, ///< multiDrawIndirect: drawCount > 1 in one vkCmdDraw*Indirect
        kCapIndirectFirstInstance = 1u << 12,  ///< drawIndirectFirstInstance
        kCapSpirv13          = 1u << 13,  ///< a 1.1 device: SPIR-V 1.3, the subgroup operations (subgroupOps)
        kCapSpirv15          = 1u << 14,  ///< a 1.2 device: SPIR-V 1.5
        kCapMultiViewport    = 1u << 15,  ///< multiViewport: gl_ViewportIndex (with kCapLayerFromVertex, from a vertex shader)
    };

    /// What the probe found and the device was created with. Zeroed (a 1.0 device, nothing optional)
    /// until the device exists, and for good when the probe is off.
    struct DeviceCaps
    {
        uint32_t loaderVersion   = VK_API_VERSION_1_0;   ///< vkEnumerateInstanceVersion (1.0 when the loader has none)
        uint32_t instanceVersion = VK_API_VERSION_1_0;   ///< what the instance was created with
        uint32_t adapterVersion  = VK_API_VERSION_1_0;   ///< the chosen adapter's VkPhysicalDeviceProperties::apiVersion
        uint32_t deviceVersion   = VK_API_VERSION_1_0;   ///< min(instanceVersion, adapterVersion): what the device may be used as
        uint32_t offered = 0;    ///< CapBit: what the adapter reports
        uint32_t enabled = 0;    ///< CapBit: what the device was created with (a subset of offered)
        uint32_t subgroupSize   = 0;   ///< VkPhysicalDeviceSubgroupProperties (a 1.1 device), 0 unknown
        uint32_t subgroupStages = 0;   ///< VkShaderStageFlags
        uint32_t subgroupOps    = 0;   ///< VkSubgroupFeatureFlags
        uint32_t maxMultiviewViews = 0;
        uint32_t computeFamily  = UINT32_MAX;   ///< a queue family with compute and no graphics (async compute)
        uint32_t computeQueues  = 0;
        uint32_t transferFamily = UINT32_MAX;   ///< a queue family with transfer alone (a copy engine)
        uint32_t transferQueues = 0;
        uint32_t graphicsQueues = 0;            ///< queues of the graphics family (the device takes one)
        char     driverName[64] = {};           ///< VkPhysicalDeviceDriverProperties (1.2), "" unknown
        char     driverInfo[64] = {};
        bool     probed = false;                ///< the probe ran (the probe on and the device came up)
    };
}
