// The swapchain-derived objects: the image list, the per-image views and semaphores, the scene image
// at the swapchain's size and the present pass over them, and the acquire.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame/Internal.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Present.hpp"

namespace gxvk::vk::frame::detail
{
    namespace present = gxvk::vk::present;

    /// Destroys what names the swapchain: the views, the semaphores, the present pass's framebuffers,
    /// the scene image and the framebuffers over it. The caller has waited for the device.
    void DropImageObjects()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        present::Drop();
        if (s.scene.handle != VK_NULL_HANDLE)
        {
            DropFramebuffersOf(s.scene.handle, false);
            texture::DestroyIdle(s.scene);
            DestroyMsImages();
        }
        for (uint32_t i = 0; i < kMaxImages; ++i)
        {
            if (s.views[i] != VK_NULL_HANDLE) d.DestroyImageView(s.device, s.views[i], nullptr);
            if (s.renderFinished[i] != VK_NULL_HANDLE) d.DestroySemaphore(s.device, s.renderFinished[i], nullptr);
            s.views[i]          = VK_NULL_HANDLE;
            s.renderFinished[i] = VK_NULL_HANDLE;
            s.imagesInFlight[i] = VK_NULL_HANDLE;
            s.images[i]         = VK_NULL_HANDLE;
        }
        s.imageCount = 0;
        s.extent     = VkExtent2D{ 0, 0 };
        s.swapGen    = UINT32_MAX;
    }

    /// The image list, the per-image views and semaphores, the scene image and the present pass for
    /// the swapchain that stands now.
    bool EnsureImageObjects(const context::Handles& h)
    {
        if (s.swapGen == context::SwapchainGeneration() && s.imageCount) return true;
        const loader::Device& d = *h.dapi;
        if (s.imageCount || s.scene.handle != VK_NULL_HANDLE)
        {
            // Earlier frames may still be presenting from these images or drawing into the scene.
            // Only the submitted frames' retirements are collected: the open frame may have named
            // a retired resource in commands not yet submitted.
            Drain();   // RENDERTHREAD: the thread may be acquiring from or presenting these images
            d.DeviceWaitIdle(s.device);
            s.completed = s.submitted;
            CollectRetired(false);
            DropImageObjects();
        }
        if (!ChooseDepthFormat(h)) return false;

        uint32_t count = 0;
        VkResult r = d.GetSwapchainImagesKHR(s.device, h.swapchain, &count, nullptr);
        if (r != VK_SUCCESS || count == 0 || count > kMaxImages)
        {
            Write(Error, "vulkan: frame: the swapchain has %u image(s) (%s); at most %u are tracked", count, ResultName(r), kMaxImages);
            s.failed = true;
            return false;
        }
        r = d.GetSwapchainImagesKHR(s.device, h.swapchain, &count, s.images);
        if (r != VK_SUCCESS)
        {
            Fail("vkGetSwapchainImagesKHR", r);
            return false;
        }

        VkSemaphoreCreateInfo sem = {};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < count; ++i)
        {
            VkImageViewCreateInfo view = {};
            view.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image    = s.images[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format   = h.format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            r = d.CreateImageView(s.device, &view, nullptr, &s.views[i]);
            if (r != VK_SUCCESS)
            {
                s.views[i] = VK_NULL_HANDLE;
                Fail("vkCreateImageView", r);
                return false;
            }
            // Per IMAGE, not per frame: only re-acquiring an image proves its present consumed it.
            r = d.CreateSemaphore(s.device, &sem, nullptr, &s.renderFinished[i]);
            if (r != VK_SUCCESS)
            {
                s.renderFinished[i] = VK_NULL_HANDLE;
                Fail("vkCreateSemaphore (render finished)", r);
                return false;
            }
        }
        s.imageCount = count;
        s.extent     = h.extent;
        s.format     = h.format;
        s.swapGen    = context::SwapchainGeneration();

        // The scene: the engine's back buffer, an offscreen image of the swapchain's size that every
        // draw lands on and the present pass shows. Born UNDEFINED: its first pass clears it. Without
        // it (the textures are not up: the draw path failed) nothing is drawn or presented, and the
        // next frame tries again.
        if (!texture::CreateTarget(h.extent.width, h.extent.height, 1, false, kSceneFormat, s.scene))
        {
            if (!s.loggedNoScene)
            {
                s.loggedNoScene = true;
                Write(Error, "vulkan: frame: the scene image (%ux%u) could not be made; nothing reaches the window", h.extent.width, h.extent.height);
            }
            DropImageObjects();
            return false;
        }
        CreateMsImages(h);   // MSAA (gxMultisample): single-sampled when they cannot be had
        CreateNormalImages(h);   // GBUFFER: none when they cannot be had
        if (!present::Rebuild(h.format, h.extent, s.views, count, s.scene.view) && !s.loggedNoPresent)
        {
            s.loggedNoPresent = true;
            Write(Error, "vulkan: frame: the present pass could not be built; the scene is drawn but never shown");
        }
        return true;
    }

    /// The acquire, once a frame, for the present.
    bool Acquire()
    {
        if (s.acquired) return true;
        if (s.noImage) return false;
        const context::Handles* h = context::Get();
        FrameSlot&              slot = s.slots[s.fi];
        if (!h || h->swapchain == VK_NULL_HANDLE || h->extent.width == 0 || h->extent.height == 0)
        {
            s.noImage = true;
            if (h) s.rebuild = true;
            ++s.stats.noImage;
            return false;
        }
        if (!EnsureImageObjects(*h))
        {
            s.noImage = true;
            ++s.stats.noImage;
            return false;
        }
        // A bounded wait: a surface in a fullscreen handoff (the window minimised or restored by
        // the system) may hold the acquire; the frame then goes on without an image and tries
        // again next time. OUT_OF_DATE is a rebuild; SURFACE_LOST a rebuild of the surface as well.
        // The semaphore stays unsignalled on every failure, so the slot keeps it.
        uint32_t image = 0;
        VkResult r = s.dapi->AcquireNextImageKHR(s.device, h->swapchain, kAcquireTimeoutNs, slot.imageAvailable, VK_NULL_HANDLE, &image);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR || r == VK_TIMEOUT || r == VK_NOT_READY)
        {
            s.noImage = true;
            if (r != VK_TIMEOUT && r != VK_NOT_READY) s.rebuild = true;
            if (r == VK_ERROR_SURFACE_LOST_KHR) s.surfaceLost = true;
            ++s.stats.noImage;
            if (r != VK_ERROR_OUT_OF_DATE_KHR) Write(Debug, "vulkan: frame: vkAcquireNextImageKHR answered %s; no image this frame", ResultName(r));
            return false;
        }
        if (r == VK_SUBOPTIMAL_KHR) s.rebuild = true;
        else if (r != VK_SUCCESS)
        {
            Fail("vkAcquireNextImageKHR", r);
            return false;
        }
        if (image >= s.imageCount)
        {
            s.failed = true;
            return false;
        }
        if (s.imagesInFlight[image] != VK_NULL_HANDLE && s.imagesInFlight[image] != slot.fence)
            s.dapi->WaitForFences(s.device, 1, &s.imagesInFlight[image], VK_TRUE, UINT64_MAX);
        s.imagesInFlight[image] = slot.fence;
        s.imageSerial[image]    = s.serial;
        s.image    = image;
        s.acquired = true;
        return true;
    }
}
