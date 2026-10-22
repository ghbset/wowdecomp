// Images: the module's life, the recorded fills (region batches, clears, generated mip chains) and
// the destroy.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture/Internal.hpp"

using namespace gxvk::vk::texture::detail;

namespace
{
    /// The stages that read a sampled image: a draw's vertex and fragment shaders.
    constexpr VkPipelineStageFlags kReaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    /// The layout every fill leaves an image in: what a combined-image-sampler descriptor names,
    /// so the draw path binds it with no barrier per frame.
    constexpr VkImageLayout kSampled = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

namespace gxvk::vk::texture
{
    bool Init()
    {
        const context::Handles* h = context::Get();
        if (!h || !h->device || !h->dapi || !h->iapi || !h->physical)
        {
            Write(Warn, "vulkan: texture: Init() without a live context");
            return false;
        }

        if (s.ready)
        {
            if (s.device == h->device) return true;
            // A different VkDevice means the old one is gone: nothing on it can be destroyed.
            Write(Warn, "vulkan: texture: the context reports a different VkDevice; forgetting %u image(s) rather than destroying them", s.ownedCount);
            FreeTables();
            s = State();
        }

        if (!HaveEntryPoints(*h->dapi, *h->iapi)) return false;

        s.owned = new (std::nothrow) Owned[kMaxImages];
        s.hash  = new (std::nothrow) uint32_t[kHashSlots];   // without it FindOwned() scans
        if (s.hash) std::memset(s.hash, 0, sizeof(uint32_t) * kHashSlots);
        if (!s.owned)
        {
            Write(Error, "vulkan: texture: cannot allocate the image table");
            FreeTables();
            return false;
        }

        s.device   = h->device;
        s.physical = h->physical;
        s.dapi     = h->dapi;
        s.iapi     = h->iapi;

        VkPhysicalDeviceProperties props = {};
        s.iapi->GetPhysicalDeviceProperties(s.physical, &props);
        s.maxDimension     = props.limits.maxImageDimension2D;
        s.maxDimensionCube = props.limits.maxImageDimensionCube;
        s.transferBits     = props.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0);

        s.ready = true;
        Write(Debug, "vulkan: texture: ready (images up to %ux%u, cubes up to %u, at most %u tracked at once)",
              s.maxDimension, s.maxDimension, s.maxDimensionCube, kMaxImages);
        return true;
    }

    void Shutdown()
    {
        if (!s.ready) return;
        if (s.ownedCount)
            Write(Warn, "vulkan: texture: Shutdown() with %u image(s) still alive; they are forgotten", s.ownedCount);
        FreeTables();
        s = State();
    }

    bool CreateMipped(uint32_t width, uint32_t height, uint32_t levels, VkFormat format, Image& out)
    {
        return CreateImage(width, height, levels, false, format, out);
    }

    bool CreateCube(uint32_t size, uint32_t levels, VkFormat format, Image& out)
    {
        // Square is a requirement of CUBE_COMPATIBLE, and the cube limit is its own, often smaller.
        if (s.ready && size > s.maxDimensionCube)
        {
            Write(Warn, "vulkan: texture: refusing a %u-texel cube map; this device allows 1..%u on a side", size, s.maxDimensionCube);
            ++s.stats.refused;
            return false;
        }
        return CreateImage(size, size, levels, true, format, out);
    }

    bool CreateTarget(uint32_t width, uint32_t height, uint32_t levels, bool cube, VkFormat format, Image& out)
    {
        if (cube && (width != height || (s.ready && width > s.maxDimensionCube))) return false;
        return CreateImage(width, height, levels, cube, format, out, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    }

    bool CreateStorage(uint32_t width, uint32_t height, VkFormat format, Image& out)
    {
        return CreateImage(width, height, 1, false, format, out, VK_IMAGE_USAGE_STORAGE_BIT);
    }

    bool CreateDepthCopy(uint32_t width, uint32_t height, VkFormat format, Image& out)
    {
        if (!IsDepth(format)) return false;
        return CreateImage(width, height, 1, false, format, out);
    }

    bool RegionBytes(const Image& img, uint32_t width, uint32_t height, uint64_t& rowBytes, uint32_t& rowCount)
    {
        const FormatInfo fmt = Describe(img.format);
        if (fmt.unitBytes == 0 || width == 0 || height == 0) return false;
        rowBytes = RowBytes(fmt, width);
        rowCount = RowCount(fmt, height);
        return true;
    }

    bool CanGenerateMips(VkFormat format)
    {
        if (!s.ready) return false;
        const FormatInfo fmt = Describe(format);
        // A blit cannot write a block-compressed level (BLIT_DST is never offered for BC).
        if (fmt.unitBytes == 0 || fmt.compressed) return false;
        VkFormatProperties props = {};
        s.iapi->GetPhysicalDeviceFormatProperties(s.physical, format, &props);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        return (props.optimalTilingFeatures & need) == need;
    }

    void DestroyIdle(Image& img)
    {
        if (img.handle == VK_NULL_HANDLE)
        {
            img = Image();
            return;
        }
        const uint32_t i = FindOwned(img.handle);
        if (i == kMaxImages || !s.dapi || !s.device)
        {
            img = Image();   // not one of ours: forgetting is the only safe answer
            return;
        }
        if (s.owned[i].view != VK_NULL_HANDLE) s.dapi->DestroyImageView(s.device, s.owned[i].view, nullptr);
        s.dapi->DestroyImage(s.device, s.owned[i].handle, nullptr);
        memory::Free(s.owned[i].block);
        RemoveOwned(i);
        img = Image();
        ++s.stats.destroyed;
        s.stats.live = s.ownedCount;
    }

    uint32_t RecordRegions(VkCommandBuffer cmd, Image& img, VkBuffer src, const RegionCopy* regions, uint32_t count)
    {
        if (!s.ready || !cmd || src == VK_NULL_HANDLE || img.handle == VK_NULL_HANDLE || !regions || count == 0) return 0;
        const FormatInfo fmt = Describe(img.format);
        if (fmt.unitBytes == 0) return 0;

        constexpr uint32_t kBatch = 64;
        VkBufferImageCopy  copies[kBatch];
        uint32_t           n        = 0;
        uint32_t           recorded = 0;
        bool               began    = false;
        const bool         sampled  = img.layout == kSampled;
        const auto flush = [&]() {
            if (n == 0) return;
            s.dapi->CmdCopyBufferToImage(cmd, src, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, copies);
            recorded += n;
            n = 0;
        };
        for (uint32_t i = 0; i < count; ++i)
        {
            const RegionCopy& g = regions[i];
            if (g.level >= img.levels || g.layer >= img.layers) continue;
            const uint32_t levelW = LevelSide(img.extent.width, g.level);
            const uint32_t levelH = LevelSide(img.extent.height, g.level);
            if (g.width == 0 || g.height == 0 || uint64_t(g.x) + g.width > levelW || uint64_t(g.y) + g.height > levelH) continue;
            if (fmt.compressed)
            {
                // A copy may not start or stop inside a 4x4 block, except that an extent may run to
                // the level's edge (the last levels of a chain are narrower than a block). The
                // driver does not report a violation; it copies garbage.
                const bool originOk = g.x % kBlock == 0 && g.y % kBlock == 0;
                const bool widthOk  = g.width % kBlock == 0 || g.x + g.width == levelW;
                const bool heightOk = g.height % kBlock == 0 || g.y + g.height == levelH;
                if (!originOk || !widthOk || !heightOk) continue;
            }
            // bufferOffset must be a multiple of 4 and of the texel block size.
            if (g.srcOffset % 4 != 0 || g.srcOffset % fmt.unitBytes != 0) continue;
            if (!began)
            {
                // One barrier for the batch: every earlier read of the image (this buffer's draws,
                // frames in flight: write-after-read needs only the execution dependency) and an
                // earlier copy or clear (TRANSFER, with its writes) before the copies. Contents
                // outside the regions are kept, so the old layout is the tracked one; UNDEFINED
                // only for a never-filled image.
                VkPipelineStageFlags srcStage = sampled ? (kReaders | VK_PIPELINE_STAGE_TRANSFER_BIT) : VK_PIPELINE_STAGE_TRANSFER_BIT;
                if (img.layout != VK_IMAGE_LAYOUT_UNDEFINED && !sampled && img.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                    && img.layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
                    srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                Transition(cmd, img.handle, img.levels, img.layers, img.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           srcStage, img.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                began = true;
            }
            VkBufferImageCopy& c = copies[n++];
            c = VkBufferImageCopy{};
            c.bufferOffset                    = g.srcOffset;
            c.bufferRowLength                 = 0;   // tightly packed
            c.bufferImageHeight               = 0;
            c.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.mipLevel       = g.level;
            c.imageSubresource.baseArrayLayer = g.layer;
            c.imageSubresource.layerCount     = 1;
            c.imageOffset = VkOffset3D{ static_cast<int32_t>(g.x), static_cast<int32_t>(g.y), 0 };
            c.imageExtent = VkExtent3D{ g.width, g.height, 1 };
            if (n == kBatch) flush();
        }
        if (!began) return 0;
        flush();
        Transition(cmd, img.handle, img.levels, img.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kSampled,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   kReaders | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        img.layout = kSampled;
        s.stats.regions += recorded;
        ++s.stats.batches;
        return recorded;
    }

    bool RecordClear(VkCommandBuffer cmd, Image& img, const float* rgba)
    {
        if (!s.ready || !cmd || img.handle == VK_NULL_HANDLE) return false;
        // From UNDEFINED (the contents are discarded), after every earlier use of the image at any
        // stage: a recorded clear may follow anything in the buffer.
        Transition(cmd, img.handle, img.levels, img.layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        // Opaque mid grey unless a colour is named: a level that stays this way is recognisable
        // and neutral at a distance.
        VkClearColorValue colour = {};
        colour.float32[0] = 0.5f;
        colour.float32[1] = 0.5f;
        colour.float32[2] = 0.5f;
        colour.float32[3] = 1.0f;
        if (rgba)
            for (unsigned i = 0; i < 4; ++i) colour.float32[i] = rgba[i];
        const VkImageSubresourceRange range = WholeColour(img.levels, img.layers);
        s.dapi->CmdClearColorImage(cmd, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
        Transition(cmd, img.handle, img.levels, img.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kSampled,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   kReaders | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        img.layout = kSampled;
        ++s.stats.clears;
        return true;
    }

    bool RecordCopy(VkCommandBuffer cmd, Image& src, Image& dst)
    {
        if (!s.ready || !cmd || src.handle == VK_NULL_HANDLE || dst.handle == VK_NULL_HANDLE || src.layout != kSampled
            || src.format != dst.format || src.extent.width != dst.extent.width || src.extent.height != dst.extent.height
            || src.levels != dst.levels || src.layers != dst.layers || src.levels > 16)
            return false;
        Transition(cmd, src.handle, src.levels, src.layers, kSampled, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   kReaders | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        Transition(cmd, dst.handle, dst.levels, dst.layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy copies[16];
        const uint32_t levels = src.levels;
        for (uint32_t i = 0; i < levels; ++i)
        {
            VkImageCopy& c = copies[i];
            c = VkImageCopy{};
            c.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.srcSubresource.mipLevel   = i;
            c.srcSubresource.layerCount = src.layers;
            c.dstSubresource            = c.srcSubresource;
            c.extent = VkExtent3D{ LevelSide(src.extent.width, i), LevelSide(src.extent.height, i), 1 };
        }
        s.dapi->CmdCopyImage(cmd, src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, copies);
        Transition(cmd, src.handle, src.levels, src.layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kSampled,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, kReaders, VK_ACCESS_SHADER_READ_BIT);
        Transition(cmd, dst.handle, dst.levels, dst.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kSampled,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   kReaders | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        dst.layout = kSampled;
        return true;
    }

    bool RecordGenerateMips(VkCommandBuffer cmd, Image& img)
    {
        if (!s.ready || !cmd || img.handle == VK_NULL_HANDLE || img.levels < 2) return false;

        // One subresource-range barrier for the chain's per-level steps; Transition() covers the
        // whole image, the right shape only for the first and the last step.
        const auto levels = [&](uint32_t base, uint32_t count, VkImageLayout from, VkImageLayout to,
                                VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
            VkImageMemoryBarrier b = {};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask       = srcAccess;
            b.dstAccessMask       = dstAccess;
            b.oldLayout           = from;
            b.newLayout           = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = img.handle;
            b.subresourceRange    = WholeColour(img.levels, img.layers);
            b.subresourceRange.baseMipLevel = base;
            b.subresourceRange.levelCount   = count;
            s.dapi->CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        };

        // The whole image to TRANSFER_DST, contents kept, after every earlier sampler read and with
        // the level-0 upload's transfer write made available.
        Transition(cmd, img.handle, img.levels, img.layers, img.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   kReaders | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

        // Level by level, every layer at once (a blit's layer range maps layer i to layer i, so a
        // cube's six faces filter independently). Level l - 1 becomes the source once written.
        for (uint32_t l = 1; l < img.levels; ++l)
        {
            levels(l - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkImageBlit blit = {};
            blit.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel       = l - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount     = img.layers;
            blit.srcOffsets[1] = VkOffset3D{ static_cast<int32_t>(LevelSide(img.extent.width, l - 1)), static_cast<int32_t>(LevelSide(img.extent.height, l - 1)), 1 };
            blit.dstSubresource          = blit.srcSubresource;
            blit.dstSubresource.mipLevel = l;
            blit.dstOffsets[1] = VkOffset3D{ static_cast<int32_t>(LevelSide(img.extent.width, l)), static_cast<int32_t>(LevelSide(img.extent.height, l)), 1 };
            s.dapi->CmdBlitImage(cmd, img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }

        // Every level to the sampling layout: the sources from TRANSFER_SRC, the last from TRANSFER_DST.
        levels(0, img.levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kSampled, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, kReaders, VK_ACCESS_SHADER_READ_BIT);
        levels(img.levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kSampled, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, kReaders, VK_ACCESS_SHADER_READ_BIT);
        img.layout = kSampled;
        ++s.stats.chains;
        return true;
    }

    void GetStats(Stats& out) { out = s.stats; }
}
