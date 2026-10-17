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
        const VkFormat candidates[3] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM };
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
        a[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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

    /// The four pass variants of a colour format, made on first use.
    const PassSet* PassesFor(VkFormat colour)
    {
        for (uint32_t i = 0; i < s.passCount; ++i)
            if (s.passes[i].colour == colour) return &s.passes[i];
        if (s.passCount == kMaxPassFormats || s.depthFormat == VK_FORMAT_UNDEFINED)
        {
            Write(Warn, "vulkan: frame: no render pass for colour format %s (%u formats held)", FormatName(colour), s.passCount);
            return nullptr;
        }
        PassSet& set = s.passes[s.passCount];
        set.colour = colour;
        for (uint32_t v = 0; v < 4; ++v)
        {
            if (!CreatePass(colour, (v & 1) != 0, (v & 2) != 0, set.pass[v]))
            {
                for (uint32_t u = 0; u < v; ++u) s.dapi->DestroyRenderPass(s.device, set.pass[u], nullptr);
                set = PassSet();
                return nullptr;
            }
        }
        ++s.passCount;
        Write(Debug, "vulkan: frame: render passes for colour %s over depth %s", FormatName(colour), FormatName(s.depthFormat));
        return &set;
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

        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = s.depthFormat;
        info.extent        = VkExtent3D{ extent.width, extent.height, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC: CopyDepthFromScene (VFOG) copies the scene's depth into a sampled image.
        info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        DepthBuffer d;
        VkResult r = s.dapi->CreateImage(s.device, &info, nullptr, &d.image);
        if (r != VK_SUCCESS)
        {
            Fail("vkCreateImage (depth)", r);
            return UINT32_MAX;
        }
        VkMemoryRequirements req = {};
        s.dapi->GetImageMemoryRequirements(s.device, d.image, &req);
        if (!memory::Allocate(req, memory::Where::DeviceLocal, d.block))
        {
            Write(Error, "vulkan: frame: no device memory for a %ux%u depth buffer", extent.width, extent.height);
            s.dapi->DestroyImage(s.device, d.image, nullptr);
            return UINT32_MAX;
        }
        r = s.dapi->BindImageMemory(s.device, d.image, d.block.memory, d.block.offset);
        if (r == VK_SUCCESS)
        {
            VkImageViewCreateInfo view = {};
            view.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image                           = d.image;
            view.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            view.format                          = s.depthFormat;
            view.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT | (s.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            view.subresourceRange.levelCount     = 1;
            view.subresourceRange.layerCount     = 1;
            r = s.dapi->CreateImageView(s.device, &view, nullptr, &d.view);
        }
        if (r != VK_SUCCESS)
        {
            Fail("vkBindImageMemory / vkCreateImageView (depth)", r);
            s.dapi->DestroyImage(s.device, d.image, nullptr);
            memory::Free(d.block);
            return UINT32_MAX;
        }
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
            if (s.fbs[i].image == image && s.fbs[i].layer == t.layer) return i;

        const uint32_t depth = DepthFor(extent);
        if (depth == UINT32_MAX) return UINT32_MAX;
        const PassSet* passes = PassesFor(format);
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
        view.subresourceRange.baseMipLevel   = 0;
        view.subresourceRange.levelCount     = 1;
        view.subresourceRange.baseArrayLayer = t.layer;
        view.subresourceRange.layerCount     = 1;
        VkResult r = s.dapi->CreateImageView(s.device, &view, nullptr, &fb.view);
        if (r != VK_SUCCESS)
        {
            Fail("vkCreateImageView (target)", r);
            return UINT32_MAX;
        }
        const VkImageView attachments[2] = { fb.view, s.depths[depth].view };
        VkFramebufferCreateInfo info = {};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = passes->pass[0];
        info.attachmentCount = 2;
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
            if (t.image->handle == VK_NULL_HANDLE || t.layer >= t.image->layers) return false;
            image  = t.image;
            format = t.image->format;
            extent = t.image->extent;
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
