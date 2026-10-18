// The frame's targets: the render passes per colour format, the depth buffers per target size, the
// framebuffers per target, and the resolution of the current target.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame/Internal.hpp"

namespace gxvk::vk::frame::detail
{
    /// The depth format every target gets: D24S8 (the depth quantisation D3D's default D24S8 has),
    /// else D32, else D16 -- the first the adapter offers as an optimal-tiling depth attachment.
    bool ChooseDepthFormat(const context::Handles& h)
    {
        if (s.depthFormat != VK_FORMAT_UNDEFINED) return true;
        // D32S8 before D32: AMD has no D24S8, and the stencil is a Gx state (GxRs_Stencil*).
        const VkFormat candidates[4] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM };
        for (VkFormat f : candidates)
        {
            VkFormatProperties props = {};
            h.iapi->GetPhysicalDeviceFormatProperties(h.physical, f, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            {
                s.depthFormat = f;
                Write(Debug, "vulkan: frame: depth buffers are %s", FormatName(f));
                return true;
            }
        }
        Write(Error, "vulkan: frame: the adapter offers no depth attachment format");
        s.failed = true;
        return false;
    }

    /**
     * One pass variant: a colour attachment of `colour` that rests in SHADER_READ_ONLY_OPTIMAL and a
     * depth attachment that rests in DEPTH_STENCIL_ATTACHMENT_OPTIMAL, each CLEARed from UNDEFINED
     * or LOADed from its resting layout. The incoming dependency waits for every earlier read of the
     * colour image (a draw that sampled it) and every earlier attachment write; the outgoing one
     * makes the pass's writes visible to the fragment shaders and transfers that sample or copy the
     * target afterwards.
     */
    static bool CreatePass(VkFormat colour, bool clearColour, bool clearDepth, VkRenderPass& out)
    {
        VkAttachmentDescription a[2] = {};
        a[0].format         = colour;
        a[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        a[0].loadOp         = clearColour ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[0].initialLayout  = clearColour ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        a[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        a[1].format         = s.depthFormat;
        a[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        a[1].loadOp         = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a[1].stencilLoadOp  = HasStencil(s.depthFormat) ? a[1].loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[1].stencilStoreOp = HasStencil(s.depthFormat) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[1].initialLayout  = clearDepth ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colourRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef  = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = 1;
        sub.pColorAttachments       = &colourRef;
        sub.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency dep[2] = {};
        dep[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass    = 0;
        dep[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                             | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dep[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass    = 0;
        dep[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo info = {};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 2;
        info.pAttachments    = a;
        info.subpassCount    = 1;
        info.pSubpasses      = &sub;
        info.dependencyCount = 2;
        info.pDependencies   = dep;
        const VkResult r = s.dapi->CreateRenderPass(s.device, &info, nullptr, &out);
        if (r != VK_SUCCESS)
        {
            out = VK_NULL_HANDLE;
            Fail("vkCreateRenderPass", r);
            return false;
        }
        return true;
    }

    /**
     * MSAA (the scene only): a multisampled colour (0) and depth (1) that rest in their attachment
     * layouts, CLEARed or LOADed and always STOREd (a pass is cut by every copy and target switch),
     * resolved at the end of every pass instance -- the colour into the scene (2), the depth, sample 0,
     * into the single-sample depth buffer of the scene's size (3) -- so every copy, read-back, vfog
     * depth snapshot and the present pass read single-sample images as without MSAA.
     */
    static bool CreatePassMs(VkFormat colour, uint32_t samples, bool clearColour, bool clearDepth, VkRenderPass& out)
    {
        const VkSampleCountFlagBits n = static_cast<VkSampleCountFlagBits>(samples);
        VkAttachmentDescription2 a[4] = {};
        for (VkAttachmentDescription2& x : a)
        {
            x.sType          = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            x.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            x.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            x.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        }
        a[0].format        = colour;
        a[0].samples       = n;
        a[0].loadOp        = clearColour ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a[0].initialLayout = clearColour ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a[1].format        = s.depthFormat;
        a[1].samples       = n;
        a[1].loadOp        = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a[1].initialLayout = clearDepth ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a[1].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a[1].stencilLoadOp  = HasStencil(s.depthFormat) ? a[1].loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[1].stencilStoreOp = HasStencil(s.depthFormat) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[2].format        = colour;
        a[2].samples       = VK_SAMPLE_COUNT_1_BIT;
        a[2].loadOp        = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // the resolve writes the whole render area
        a[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a[2].finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        a[3].format        = s.depthFormat;
        a[3].samples       = VK_SAMPLE_COUNT_1_BIT;
        a[3].loadOp        = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a[3].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a[3].stencilStoreOp = HasStencil(s.depthFormat) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;   // the stencil resolve

        const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(s.depthFormat) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        VkAttachmentReference2 ref[4] = {};
        const VkImageLayout layouts[4] = { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        for (uint32_t i = 0; i < 4; ++i)
        {
            ref[i].sType      = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
            ref[i].attachment = i;
            ref[i].layout     = layouts[i];
            ref[i].aspectMask = (i & 1) ? depthAspect : VK_IMAGE_ASPECT_COLOR_BIT;
        }
        // SAMPLE_ZERO for depth and stencil alike: required of every 1.2 device, and legal without
        // independentResolve (the two modes are the same). Sample 0 is what D3D11's vfog copy reads.
        VkSubpassDescriptionDepthStencilResolve resolve = {};
        resolve.sType                          = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
        resolve.depthResolveMode               = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        resolve.stencilResolveMode             = depthAspect & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
        resolve.pDepthStencilResolveAttachment = &ref[3];
        VkSubpassDescription2 sub = {};
        sub.sType                   = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        sub.pNext                   = &resolve;
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount    = 1;
        sub.pColorAttachments       = &ref[0];
        sub.pResolveAttachments     = &ref[2];
        sub.pDepthStencilAttachment = &ref[1];

        // The single-sample pass's dependencies; the resolves run in COLOR_ATTACHMENT_OUTPUT, which
        // the outgoing one already waits on.
        VkSubpassDependency2 dep[2] = {};
        for (uint32_t i = 0; i < 2; ++i) dep[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        dep[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass    = 0;
        dep[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                             | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dep[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass    = 0;
        dep[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo2 info = {};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        info.attachmentCount = 4;
        info.pAttachments    = a;
        info.subpassCount    = 1;
        info.pSubpasses      = &sub;
        info.dependencyCount = 2;
        info.pDependencies   = dep;
        const VkResult r = s.dapi->CreateRenderPass2(s.device, &info, nullptr, &out);
        if (r != VK_SUCCESS)
        {
            out = VK_NULL_HANDLE;
            Fail("vkCreateRenderPass2 (MSAA)", r);
            return false;
        }
        return true;
    }

    /// The four pass variants of a colour format and sample count, made on first use.
    const PassSet* PassesFor(VkFormat colour, uint32_t samples)
    {
        for (uint32_t i = 0; i < s.passCount; ++i)
            if (s.passes[i].colour == colour && s.passes[i].samples == samples) return &s.passes[i];
        if (s.passCount == kMaxPassFormats || s.depthFormat == VK_FORMAT_UNDEFINED)
        {
            Write(Warn, "vulkan: frame: no render pass for colour format %s (%u formats held)", FormatName(colour), s.passCount);
            return nullptr;
        }
        PassSet& set = s.passes[s.passCount];
        set.colour  = colour;
        set.samples = samples;
        for (uint32_t v = 0; v < 4; ++v)
        {
            const bool ok = samples > 1 ? CreatePassMs(colour, samples, (v & 1) != 0, (v & 2) != 0, set.pass[v])
                                        : CreatePass(colour, (v & 1) != 0, (v & 2) != 0, set.pass[v]);
            if (!ok)
            {
                for (uint32_t u = 0; u < v; ++u) s.dapi->DestroyRenderPass(s.device, set.pass[u], nullptr);
                set = PassSet();
                return nullptr;
            }
        }
        ++s.passCount;
        Write(Debug, "vulkan: frame: render passes for colour %s over depth %s, %u sample(s)", FormatName(colour), FormatName(s.depthFormat), samples);
        return &set;
    }

    /// A device-local 2D image of one level and its view: a depth buffer (a depth format) or the MSAA
    /// scene's colour. False, with nothing left behind, when it cannot be had.
    static bool MakeImage(VkFormat format, VkExtent2D extent, uint32_t samples, VkImageUsageFlags usage, DepthBuffer& d)
    {
        const bool isDepth = format == s.depthFormat;
        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = format;
        info.extent        = VkExtent3D{ extent.width, extent.height, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = static_cast<VkSampleCountFlagBits>(samples);
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = usage;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = s.dapi->CreateImage(s.device, &info, nullptr, &d.image);
        if (r != VK_SUCCESS)
        {
            Fail("vkCreateImage (depth / MSAA)", r);
            return false;
        }
        VkMemoryRequirements req = {};
        s.dapi->GetImageMemoryRequirements(s.device, d.image, &req);
        if (!memory::Allocate(req, memory::Where::DeviceLocal, d.block))
        {
            Write(Error, "vulkan: frame: no device memory for a %ux%u x%u %s image", extent.width, extent.height, samples, FormatName(format));
            s.dapi->DestroyImage(s.device, d.image, nullptr);
            d.image = VK_NULL_HANDLE;
            return false;
        }
        r = s.dapi->BindImageMemory(s.device, d.image, d.block.memory, d.block.offset);
        if (r == VK_SUCCESS)
        {
            VkImageViewCreateInfo view = {};
            view.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image                           = d.image;
            view.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            view.format                          = format;
            view.subresourceRange.aspectMask     = !isDepth ? VK_IMAGE_ASPECT_COLOR_BIT
                                                 : VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            view.subresourceRange.levelCount     = 1;
            view.subresourceRange.layerCount     = 1;
            r = s.dapi->CreateImageView(s.device, &view, nullptr, &d.view);
        }
        if (r != VK_SUCCESS)
        {
            Fail("vkBindImageMemory / vkCreateImageView (depth / MSAA)", r);
            s.dapi->DestroyImage(s.device, d.image, nullptr);
            memory::Free(d.block);
            d = DepthBuffer();
            return false;
        }
        return true;
    }

    /// The MSAA scene's colour and depth, at the scene's size and the wanted sample count clamped
    /// to what the adapter can render (and to 1 without vkCreateRenderPass2, which the depth resolve
    /// needs). Made with the scene; sceneSamples is 1 when there are none.
    bool CreateMsImages(const context::Handles& h)
    {
        s.sceneSamples = 1;
        if (s.wantSamples <= 1) return true;
        VkPhysicalDeviceProperties props = {};
        h.iapi->GetPhysicalDeviceProperties(h.physical, &props);
        const VkSampleCountFlags ok = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
        uint32_t n = 8;
        while (n > s.wantSamples || (n > 1 && !(ok & n))) n >>= 1;
        if (!s.dapi->CreateRenderPass2 || context::Caps().deviceVersion < VK_API_VERSION_1_2)
        {
            Write(Warn, "vulkan: frame: gxMultisample %u needs a Vulkan 1.2 device (depth resolve); the scene stays single-sampled", s.wantSamples);
            return true;
        }
        if (n <= 1) return true;
        const VkExtent2D extent = s.scene.extent;
        if (!MakeImage(kSceneFormat, extent, n, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, s.msColour)) return true;
        if (!MakeImage(s.depthFormat, extent, n, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, s.msDepth))
        {
            DestroyMsImages();
            return true;
        }
        s.msColour.extent = s.msDepth.extent = extent;
        s.sceneSamples = n;
        Write(Info, "vulkan: frame: MSAA %ux (gxMultisample %u) on the %ux%u scene", n, s.wantSamples, extent.width, extent.height);
        return true;
    }

    /// The MSAA images; the caller has made the device idle (they go with the scene).
    void DestroyMsImages()
    {
        for (DepthBuffer* d : { &s.msColour, &s.msDepth })
        {
            if (d->view != VK_NULL_HANDLE) s.dapi->DestroyImageView(s.device, d->view, nullptr);
            if (d->image != VK_NULL_HANDLE) s.dapi->DestroyImage(s.device, d->image, nullptr);
            if (d->image != VK_NULL_HANDLE) memory::Free(d->block);
            *d = DepthBuffer();
        }
        s.sceneSamples = 1;
    }

    static void DestroyDepth(DepthBuffer& d, bool retire)
    {
        if (retire)
        {
            Retired e;
            e.view     = d.view;
            e.rawImage = d.image;
            e.rawBlock = d.block;
            Retire(e, d.lastUsed);
        }
        else
        {
            if (d.view != VK_NULL_HANDLE) s.dapi->DestroyImageView(s.device, d.view, nullptr);
            if (d.image != VK_NULL_HANDLE) s.dapi->DestroyImage(s.device, d.image, nullptr);
            memory::Free(d.block);
        }
        d = DepthBuffer();
        --s.stats.depthBuffers;
    }

    /// The depth buffer of a size, made on first use; the table's least recently used entry gives
    /// way when it is full (retired with the frame that last used it, its framebuffers with it).
    static uint32_t DepthFor(VkExtent2D extent)
    {
        for (uint32_t i = 0; i < s.depthCount; ++i)
            if (s.depths[i].extent.width == extent.width && s.depths[i].extent.height == extent.height) return i;

        uint32_t slot = s.depthCount;
        if (slot == kMaxDepthBuffers)
        {
            slot = UINT32_MAX;
            for (uint32_t i = 0; i < s.depthCount; ++i)
                if (!UsedThisFrame(s.depths[i].lastUsed) && (slot == UINT32_MAX || s.depths[i].lastUsed < s.depths[slot].lastUsed)) slot = i;
            if (slot == UINT32_MAX) return UINT32_MAX;
            for (uint32_t i = 0; i < s.fbCount;)
            {
                if (s.fbs[i].depth == slot)
                {
                    Retired e;
                    e.framebuffer = s.fbs[i].fb;
                    e.view        = s.fbs[i].view;
                    Retire(e, s.fbs[i].lastUsed);
                    s.fbs[i] = s.fbs[--s.fbCount];
                    --s.stats.framebuffers;
                    continue;
                }
                ++i;
            }
            DestroyDepth(s.depths[slot], true);
        }

        DepthBuffer d;
        // TRANSFER_SRC: CopyDepthFromScene (VFOG) copies the scene's depth into a sampled image.
        if (!MakeImage(s.depthFormat, extent, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, d)) return UINT32_MAX;
        d.extent      = extent;
        d.initialised = false;
        d.lastUsed    = s.serial;
        s.depths[slot] = d;
        if (slot == s.depthCount) ++s.depthCount;
        ++s.stats.depthBuffers;
        Write(Debug, "vulkan: frame: depth buffer %ux%u made (%u held)", extent.width, extent.height, s.depthCount);
        return slot;
    }

    /// The framebuffer over a target and the depth buffer of its size, made on first use; the least
    /// recently used entry gives way when the table is full.
    uint32_t FramebufferFor(const Target& t, VkFormat format, VkExtent2D extent)
    {
        const VkImage image = t.image ? t.image->handle : s.scene.handle;
        for (uint32_t i = 0; i < s.fbCount; ++i)
            if (s.fbs[i].image == image && s.fbs[i].layer == t.layer && s.fbs[i].level == t.level) return i;

        const uint32_t depth = DepthFor(extent);
        if (depth == UINT32_MAX) return UINT32_MAX;
        const uint32_t samples = t.image ? 1u : s.sceneSamples;
        const PassSet* passes = PassesFor(format, samples);
        if (!passes) return UINT32_MAX;

        uint32_t slot = s.fbCount;
        if (slot == kMaxFramebuffers)
        {
            slot = UINT32_MAX;
            for (uint32_t i = 0; i < s.fbCount; ++i)
                if (!UsedThisFrame(s.fbs[i].lastUsed) && (slot == UINT32_MAX || s.fbs[i].lastUsed < s.fbs[slot].lastUsed)) slot = i;
            if (slot == UINT32_MAX) return UINT32_MAX;
            Retired e;
            e.framebuffer = s.fbs[slot].fb;
            e.view        = s.fbs[slot].view;
            Retire(e, s.fbs[slot].lastUsed);
            s.fbs[slot] = Framebuffer();
            --s.stats.framebuffers;
        }

        Framebuffer fb;
        VkImageViewCreateInfo view = {};
        view.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image                           = image;
        view.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        view.format                          = format;
        view.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel   = t.level;
        view.subresourceRange.levelCount     = 1;
        view.subresourceRange.baseArrayLayer = t.layer;
        view.subresourceRange.layerCount     = 1;
        VkResult r = s.dapi->CreateImageView(s.device, &view, nullptr, &fb.view);
        if (r != VK_SUCCESS)
        {
            Fail("vkCreateImageView (target)", r);
            return UINT32_MAX;
        }
        // MSAA: the multisampled pair first, the resolve targets (the scene, its depth) after.
        const VkImageView single[2] = { fb.view, s.depths[depth].view };
        const VkImageView multi[4]  = { s.msColour.view, s.msDepth.view, fb.view, s.depths[depth].view };
        const VkImageView* attachments = samples > 1 ? multi : single;
        VkFramebufferCreateInfo info = {};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = passes->pass[0];
        info.attachmentCount = samples > 1 ? 4u : 2u;
        info.pAttachments    = attachments;
        info.width           = extent.width;
        info.height          = extent.height;
        info.layers          = 1;
        r = s.dapi->CreateFramebuffer(s.device, &info, nullptr, &fb.fb);
        if (r != VK_SUCCESS)
        {
            Fail("vkCreateFramebuffer", r);
            s.dapi->DestroyImageView(s.device, fb.view, nullptr);
            return UINT32_MAX;
        }
        fb.image    = image;
        fb.layer    = t.layer;
        fb.level    = t.level;
        fb.depth    = depth;
        fb.format   = format;
        fb.extent   = extent;
        fb.lastUsed = s.serial;
        s.fbs[slot] = fb;
        if (slot == s.fbCount) ++s.fbCount;
        ++s.stats.framebuffers;
        return slot;
    }

    /// The image, format and extent of a target; for the scene, the swapchain's objects are made
    /// first. False when the target cannot be had.
    bool TargetView(const Target& t, const texture::Image*& image, VkFormat& format, VkExtent2D& extent)
    {
        if (t.image)
        {
            if (t.image->handle == VK_NULL_HANDLE || t.layer >= t.image->layers || t.level >= t.image->levels) return false;
            image  = t.image;
            format = t.image->format;
            // RTT MIPS: the level's extent, so the framebuffer, render area, whole-target clear and paired depth
            // buffer (DepthFor) are that level's size. The pass's load op (a whole clear) and its finalLayout touch
            // only this level's view; every level stays SHADER_READ_ONLY between passes, which is the one layout the
            // image tracks (RecordClear moves a new target's whole chain there in the pre-pass).
            extent.width  = t.image->extent.width >> t.level ? t.image->extent.width >> t.level : 1u;
            extent.height = t.image->extent.height >> t.level ? t.image->extent.height >> t.level : 1u;
            return true;
        }
        const context::Handles* h = context::Get();
        if (!h || h->swapchain == VK_NULL_HANDLE || h->extent.width == 0 || h->extent.height == 0)
        {
            if (h) s.rebuild = true;
            return false;
        }
        if (!EnsureImageObjects(*h) || s.scene.handle == VK_NULL_HANDLE) return false;
        image  = &s.scene;
        format = s.scene.format;
        extent = s.scene.extent;
        return true;
    }

    /// Every framebuffer over an image: destroyed at once or retired with the frame that last used it.
    void DropFramebuffersOf(VkImage image, bool retire)
    {
        for (uint32_t i = 0; i < s.fbCount;)
        {
            if (s.fbs[i].image != image)
            {
                ++i;
                continue;
            }
            if (s.passFb == i) s.passFb = UINT32_MAX;
            if (retire)
            {
                Retired e;
                e.framebuffer = s.fbs[i].fb;
                e.view        = s.fbs[i].view;
                Retire(e, s.fbs[i].lastUsed);
            }
            else
            {
                s.dapi->DestroyFramebuffer(s.device, s.fbs[i].fb, nullptr);
                s.dapi->DestroyImageView(s.device, s.fbs[i].view, nullptr);
            }
            s.fbs[i] = s.fbs[--s.fbCount];
            if (s.passFb == s.fbCount) s.passFb = i;
            --s.stats.framebuffers;
        }
    }

    /// Every pass, depth buffer and framebuffer; the caller has made the device idle.
    void DestroyTargets()
    {
        for (uint32_t i = 0; i < s.fbCount; ++i)
        {
            s.dapi->DestroyFramebuffer(s.device, s.fbs[i].fb, nullptr);
            s.dapi->DestroyImageView(s.device, s.fbs[i].view, nullptr);
            s.fbs[i] = Framebuffer();
        }
        s.fbCount            = 0;
        s.stats.framebuffers = 0;
        for (uint32_t i = 0; i < s.depthCount; ++i) DestroyDepth(s.depths[i], false);
        s.depthCount         = 0;
        s.stats.depthBuffers = 0;
        for (uint32_t i = 0; i < s.passCount; ++i)
        {
            for (VkRenderPass p : s.passes[i].pass)
                if (p != VK_NULL_HANDLE) s.dapi->DestroyRenderPass(s.device, p, nullptr);
            s.passes[i] = PassSet();
        }
        s.passCount = 0;
        ++s.passGen;
    }
}
