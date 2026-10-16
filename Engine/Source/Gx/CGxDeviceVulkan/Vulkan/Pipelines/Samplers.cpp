// The sampler cache, keyed on the engine's texture flags.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines/Internal.hpp"

namespace gxvk::vk::pipelines::detail
{
    /**
     * The cached sampler for a CGxTexFlags key, built on first use.
     *
     * Filter (bits 0-2), the engine's EGxTexFilter, as CGxDeviceD3d::s_filterModes:
     *
     *   0 Nearest             NEAREST / NEAREST, no mip
     *   1 Linear              LINEAR / LINEAR,   no mip
     *   2 NearestMipNearest   NEAREST / NEAREST, mipmap NEAREST
     *   3 LinearMipNearest    LINEAR / LINEAR,   mipmap NEAREST
     *   4 LinearMipLinear     LINEAR / LINEAR,   mipmap LINEAR
     *   5 Anisotropic         as 4, anisotropic with the texture's max anisotropy (bits 9-13) when
     *                         the device enabled samplerAnisotropy
     *
     * "No mip" is mipmapMode NEAREST with maxLod 0: the level of detail is clamped to level 0.
     * Wrap: bit 3 (U) and bit 4 (V) set mean REPEAT, clear mean CLAMP_TO_EDGE (the engine has no
     * border or mirrored modes); W is always CLAMP_TO_EDGE.
     *
     * @param uint32 key : CGxTexFlags & 0x3E1F
     * @return VkSampler sampler : VK_NULL_HANDLE when the cache is full or the create failed (logged once)
     */
    VkSampler Sampler(uint32_t key)
    {
        for (uint32_t i = 0; i < s.samplerCount; ++i)
            if (s.samplers[i].key == key) return s.samplers[i].sampler;

        if (s.samplerCount >= kMaxSamplers)
        {
            if (!s.warnedSamplers)
            {
                s.warnedSamplers = true;
                Write(Warn, "vulkan: pipelines: the sampler cache holds %u; draws needing flags 0x%04X and later are skipped (logged once)", kMaxSamplers, key);
            }
            return VK_NULL_HANDLE;
        }

        const uint32_t filter = key & 0x7u;
        const bool     wrapU  = (key & (1u << 3)) != 0;
        const bool     wrapV  = (key & (1u << 4)) != 0;
        uint32_t aniso = filter == 5 && s.anisoMax > 1.0f ? (key >> 9) & 0x1Fu : 0u;
        if (static_cast<float>(aniso) > s.anisoMax) aniso = static_cast<uint32_t>(s.anisoMax);

        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

        const bool linear = filter != 0 && filter != 2;
        info.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.minFilter = info.magFilter;

        const bool noMip = filter < 2;
        info.mipmapMode = filter >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.minLod     = 0.0f;
        info.maxLod     = noMip ? 0.0f : VK_LOD_CLAMP_NONE;
        info.mipLodBias = 0.0f;

        info.addressModeU = wrapU ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = wrapV ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        info.anisotropyEnable        = aniso > 1 ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy           = aniso > 1 ? static_cast<float>(aniso) : 1.0f;
        info.compareEnable           = VK_FALSE;
        info.compareOp               = VK_COMPARE_OP_ALWAYS;
        info.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;

        VkSampler sampler = VK_NULL_HANDLE;
        const VkResult r = s.dapi->CreateSampler(s.device, &info, nullptr, &sampler);
        if (r != VK_SUCCESS)
        {
            if (!s.warnedSamplers)
            {
                s.warnedSamplers = true;
                Write(Error, "vulkan: pipelines: vkCreateSampler failed: %s (logged once)", ResultName(r));
            }
            return VK_NULL_HANDLE;
        }

        SamplerEntry& e = s.samplers[s.samplerCount++];
        e.key     = key;
        e.sampler = sampler;
        s.stats.samplers = s.samplerCount;

        Write(Debug, "vulkan: pipelines: sampler #%u for flags 0x%04X: filter %u -> %s, mip %s, anisotropy %u, wrap %s/%s",
              s.samplerCount, key, filter, linear ? "LINEAR" : "NEAREST",
              noMip ? "none" : (filter >= 4 ? "LINEAR" : "NEAREST"), aniso > 1 ? aniso : 1u,
              wrapU ? "REPEAT" : "CLAMP", wrapV ? "REPEAT" : "CLAMP");
        return sampler;
    }

    void DestroySamplers()
    {
        for (uint32_t i = 0; i < s.samplerCount; ++i)
            if (s.samplers[i].sampler != VK_NULL_HANDLE) s.dapi->DestroySampler(s.device, s.samplers[i].sampler, nullptr);
        s.samplerCount = 0;
    }
}
