// The one create path behind every texture: 2D, mipped, cube, target.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture/Internal.hpp"

namespace gxvk::vk::texture::detail
{
    /**
     * A 2D image with `levels` mip levels and, for a cube, six array layers, CUBE_COMPATIBLE and a
     * cube view; DeviceLocal memory bound; the view made with it (a draw binds it through a
     * descriptor on the per-frame path, where a create call does not belong).
     *
     * @param VkImageUsageFlags extraUsage : CreateTarget's COLOR_ATTACHMENT bit
     *
     * A depth format (CreateDepthCopy, VFOG) has no FormatInfo -- it is never uploaded -- and gets a
     * DEPTH-aspect view: sampled, .r is the stored depth.
     * @return bool ok : on failure nothing is left created and `out` is untouched
     */
    bool CreateImage(uint32_t width, uint32_t height, uint32_t levels, bool cube, VkFormat format, Image& out, VkImageUsageFlags extraUsage)
    {
        if (!s.ready)
        {
            Write(Warn, "vulkan: texture: an image was asked for before Init()");
            return false;
        }
        if (out.handle != VK_NULL_HANDLE)
        {
            Write(Warn, "vulkan: texture: an image created into an Image that still holds one; it must be destroyed first");
            ++s.stats.refused;
            return false;
        }
        if (width == 0 || height == 0 || width > s.maxDimension || height > s.maxDimension)
        {
            Write(Warn, "vulkan: texture: refusing a %ux%u image; this device allows 1..%u on each side", width, height, s.maxDimension);
            ++s.stats.refused;
            return false;
        }
        if (levels == 0 || levels > MaxLevels(width, height))
        {
            Write(Warn, "vulkan: texture: refusing %u mip level(s) for a %ux%u image; 1..%u are possible", levels, width, height, MaxLevels(width, height));
            ++s.stats.refused;
            return false;
        }

        const bool       depth = IsDepth(format);
        const FormatInfo fmt   = Describe(format);
        if (fmt.unitBytes == 0 && !depth)
        {
            Write(Warn, "vulkan: texture: refusing format %s: its texel size is not known here, so an upload's length could not be checked", Name(format));
            ++s.stats.refused;
            return false;
        }

        if (fmt.compressed)
        {
            // Creating a BC image without textureCompressionBC is invalid usage, not a graceful
            // failure; the caller decodes on the CPU instead.
            const context::Handles* h = context::Get();
            if (!h || !h->textureCompressionBC)
            {
                Write(Warn, "vulkan: texture: refusing format %s: the device feature textureCompressionBC is not enabled", Name(format));
                ++s.stats.refused;
                return false;
            }
        }

        if (s.ownedCount >= kMaxImages)
        {
            Write(Warn, "vulkan: texture: refusing a new image: %u are already tracked (kMaxImages)", s.ownedCount);
            ++s.stats.refused;
            return false;
        }

        // SAMPLED_IMAGE is what a texture needs. The per-format TRANSFER_SRC/DST bits exist only
        // from 1.1 (VK_KHR_maintenance1); on a 1.0 driver they are never set, and any format the
        // device supports with optimal tiling supports transfers.
        VkFormatProperties fp = {};
        s.iapi->GetPhysicalDeviceFormatProperties(s.physical, format, &fp);

        VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (s.transferBits) needed |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (extraUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) needed |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (extraUsage & VK_IMAGE_USAGE_STORAGE_BIT) needed |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;   // LIGHTING

        if ((fp.optimalTilingFeatures & needed) != needed)
        {
            Write(Warn, "vulkan: texture: format %s is not supported with optimal tiling for sampling%s%s on this device (features 0x%08x)",
                  Name(format), s.transferBits ? " and transfer" : "", (extraUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? " and rendering" : "",
                  static_cast<unsigned>(fp.optimalTilingFeatures));
            ++s.stats.refused;
            return false;
        }

        const uint32_t layers = cube ? 6u : 1u;

        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.flags         = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = format;
        info.extent        = VkExtent3D{ width, height, 1 };
        info.mipLevels     = levels;
        info.arrayLayers   = layers;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        // OPTIMAL: linear tiling is allowed to support almost nothing (BLIT_SRC included) and is
        // slow to read.
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_DST for the uploads, TRANSFER_SRC for the mip chain's blits and copies out of
        // it, SAMPLED for the draws.
        info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image = VK_NULL_HANDLE;
        VkResult r = s.dapi->CreateImage(s.device, &info, nullptr, &image);
        if (r != VK_SUCCESS)
        {
            Fail("vkCreateImage", r);
            ++s.stats.refused;
            return false;
        }

        VkMemoryRequirements req = {};
        s.dapi->GetImageMemoryRequirements(s.device, image, &req);

        // The block honours req.alignment and bufferImageGranularity; both are the allocator's contract.
        memory::Block block;
        if (!memory::Allocate(req, memory::Where::DeviceLocal, block))
        {
            Write(Warn, "vulkan: texture: no device-local memory for a %ux%u %s %s with %u level(s) (%llu bytes, alignment %llu)",
                  width, height, Name(format), cube ? "cube map" : "image", levels,
                  static_cast<unsigned long long>(req.size), static_cast<unsigned long long>(req.alignment));
            s.dapi->DestroyImage(s.device, image, nullptr);
            ++s.stats.refused;
            return false;
        }

        r = s.dapi->BindImageMemory(s.device, image, block.memory, block.offset);
        if (r != VK_SUCCESS)
        {
            Fail("vkBindImageMemory", r);
            s.dapi->DestroyImage(s.device, image, nullptr);
            memory::Free(block);
            ++s.stats.refused;
            return false;
        }

        // Every level, identity swizzle: what was uploaded is what the shader reads. A cube view
        // over the six layers in the API's face order, which is also D3D9's.
        VkImageViewCreateInfo view = {};
        view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image            = image;
        view.viewType         = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        view.format           = format;
        view.components.r     = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.components.g     = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.components.b     = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.components.a     = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.subresourceRange = WholeColour(levels, layers);
        if (depth) view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

        VkImageView imageView = VK_NULL_HANDLE;
        r = s.dapi->CreateImageView(s.device, &view, nullptr, &imageView);
        if (r != VK_SUCCESS)
        {
            Fail("vkCreateImageView", r);
            s.dapi->DestroyImage(s.device, image, nullptr);
            memory::Free(block);
            ++s.stats.refused;
            return false;
        }

        s.owned[s.ownedCount].handle = image;
        s.owned[s.ownedCount].view   = imageView;
        s.owned[s.ownedCount].block  = block;
        HashInsert(s.ownedCount);
        ++s.ownedCount;

        out.handle = image;
        out.view   = imageView;
        out.id     = ++s.nextId;
        out.block  = block;
        out.format = format;
        out.extent = VkExtent2D{ width, height };
        out.levels = levels;
        out.layers = layers;
        out.cube   = cube;
        out.layout = VK_IMAGE_LAYOUT_UNDEFINED;

        ++s.stats.created;
        s.stats.bytes += req.size;
        s.stats.live = s.ownedCount;
        if (s.ownedCount > s.stats.livePeak) s.stats.livePeak = s.ownedCount;
        return true;
    }
}
