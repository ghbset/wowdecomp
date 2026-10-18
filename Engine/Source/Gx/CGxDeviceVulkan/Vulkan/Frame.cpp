// The frame: opening a frame slot and its render pass on the current target, the clear, the target
// switch, the submit and the present, the frame serials and the retirement of resources by serial.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame/Internal.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Present.hpp"

using namespace gxvk::vk::frame::detail;

namespace
{
    namespace present = gxvk::vk::present;

    /// vkCmdClearAttachments over `rect` (clamped to the target) or the whole target, colour and
    /// depth as the mask says.
    void ClearInPass(uint32_t mask, const float colour[4], const VkRect2D* rect)
    {
        if (s.passFb >= s.fbCount) return;
        const VkExtent2D extent = s.fbs[s.passFb].extent;
        VkClearAttachment a[2] = {};
        uint32_t          n    = 0;
        if (mask & 1u)
        {
            a[n].aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
            a[n].colorAttachment = 0;
            for (uint32_t i = 0; i < 4; ++i) a[n].clearValue.color.float32[i] = colour[i];
            ++n;
        }
        if (mask & 2u)
        {
            a[n].aspectMask                      = VK_IMAGE_ASPECT_DEPTH_BIT;
            a[n].clearValue.depthStencil.depth   = 1.0f;
            a[n].clearValue.depthStencil.stencil = 0;
            ++n;
        }
        if ((mask & 4u) && gxvk::vk::HasStencil(s.depthFormat))
        {
            if (mask & 2u) a[n - 1].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            else
            {
                a[n].aspectMask                      = VK_IMAGE_ASPECT_STENCIL_BIT;
                a[n].clearValue.depthStencil.stencil = 0;
                ++n;
            }
        }
        if (n == 0) return;
        VkClearRect r = {};
        r.rect.extent = extent;
        if (rect)
        {
            int32_t x0 = rect->offset.x < 0 ? 0 : rect->offset.x;
            int32_t y0 = rect->offset.y < 0 ? 0 : rect->offset.y;
            int64_t x1 = static_cast<int64_t>(rect->offset.x) + rect->extent.width;
            int64_t y1 = static_cast<int64_t>(rect->offset.y) + rect->extent.height;
            if (x1 > extent.width) x1 = extent.width;
            if (y1 > extent.height) y1 = extent.height;
            if (x1 <= x0 || y1 <= y0) return;
            r.rect.offset.x      = x0;
            r.rect.offset.y      = y0;
            r.rect.extent.width  = static_cast<uint32_t>(x1 - x0);
            r.rect.extent.height = static_cast<uint32_t>(y1 - y0);
        }
        r.layerCount = 1;
        s.dapi->CmdClearAttachments(s.slots[s.fi].cmd, n, a, 1, &r);
        ++s.stats.clearsInPass;
    }

    /// Whether a rectangle covers the whole extent (or there is no rectangle).
    bool Whole(const VkRect2D* rect, VkExtent2D extent)
    {
        return !rect || (rect->offset.x <= 0 && rect->offset.y <= 0
                         && static_cast<int64_t>(rect->offset.x) + rect->extent.width >= extent.width
                         && static_cast<int64_t>(rect->offset.y) + rect->extent.height >= extent.height);
    }
}

namespace gxvk::vk::frame::detail
{
    /// Opens the frame on the next slot: waits for the GPU to be done with that slot's last frame,
    /// destroys what that completion released, resets the slot's staging ring, begins its two
    /// command buffers.
    bool EnsureFrame()
    {
        if (s.failed) return false;
        if (s.open) return true;
        const context::Handles* h = context::Get();
        if (!h || !h->device || !h->dapi) return false;
        if (!EnsureDeviceObjects(*h)) return false;

        const loader::Device& d    = *s.dapi;
        FrameSlot&            slot = s.slots[s.fi];
        // RENDERTHREAD: the slot may still be with the render thread; its fence is only submitted
        // once the thread is done with it, and never after the thread failed.
        RtWait(slot.serial);
        RtCollect();
        if (s.failed) return false;
        VkResult r = d.WaitForFences(s.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS)
        {
            Fail("vkWaitForFences (frame slot)", r);
            return false;
        }
        if (slot.serial > s.completed) s.completed = slot.serial;
        Poll();
        CollectRetired(false);
        StagingFrameBegin(slot);

        s.serial = s.serial + 1;

        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = d.BeginCommandBuffer(slot.pre, &begin);
        if (r == VK_SUCCESS) r = d.BeginCommandBuffer(slot.cmd, &begin);
        if (r != VK_SUCCESS)
        {
            Fail("vkBeginCommandBuffer", r);
            return false;
        }
        s.open               = true;
        s.passOpen           = false;
        s.passFb             = UINT32_MAX;
        s.acquired           = false;
        s.noImage            = false;
        s.sceneTouched       = false;
        s.queryActive        = false;
        s.queryPool          = VK_NULL_HANDLE;
        s.clearColourPending = false;
        s.clearDepthPending  = false;
        s.rtPresent          = false;
        return true;
    }

    /**
     * Begins the render pass on the current target: the variant that clears what a clear is pending
     * for (or what holds nothing yet: a colour image not in its resting layout, a depth buffer no
     * pass has written) and loads the rest.
     */
    bool BeginPass()
    {
        if (s.passOpen) return true;
        const texture::Image* image  = nullptr;
        VkFormat              format = VK_FORMAT_UNDEFINED;
        VkExtent2D            extent = { 0, 0 };
        if (!TargetView(s.target, image, format, extent))
        {
            if (!s.target.image) s.noImage = true;
            ++s.stats.noImage;
            return false;
        }
        const uint32_t fbi = FramebufferFor(s.target, format, extent);
        if (fbi == UINT32_MAX) return false;
        const bool     msaa   = !s.target.image && s.sceneSamples > 1;
        const PassSet* passes = PassesFor(format, msaa ? s.sceneSamples : 1u);
        if (!passes) return false;
        Framebuffer& fb    = s.fbs[fbi];
        DepthBuffer& depth = s.depths[fb.depth];
        // MSAA: the multisampled pair is what the pass loads; the single-sample depth is only its resolve.
        DepthBuffer& loaded = msaa ? s.msDepth : depth;

        const bool colourClear = s.clearColourPending || image->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const bool depthClear  = s.clearDepthPending || !loaded.initialised;
        VkClearValue clears[4] = {};
        for (uint32_t i = 0; i < 4; ++i) clears[0].color.float32[i] = s.clearColourPending ? s.clearColour[i] : 0.0f;
        clears[1].depthStencil.depth   = 1.0f;
        clears[1].depthStencil.stencil = 0;

        VkRenderPassBeginInfo begin = {};
        begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass        = passes->pass[(colourClear ? 1u : 0u) | (depthClear ? 2u : 0u)];
        begin.framebuffer       = fb.fb;
        begin.renderArea.extent = extent;
        begin.clearValueCount   = msaa ? 4u : 2u;
        begin.pClearValues      = clears;
        s.dapi->CmdBeginRenderPass(s.slots[s.fi].cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

        // The pass leaves the colour image in its resting layout; recorded now, since nothing else
        // touches the image before the pass ends.
        const_cast<texture::Image*>(image)->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depth.initialised  = true;
        depth.lastUsed     = s.serial;
        loaded.initialised = true;
        fb.lastUsed       = s.serial;
        s.passOpen           = true;
        s.passFb             = fbi;
        s.clearColourPending = false;
        s.clearDepthPending  = false;
        if (!s.target.image) s.sceneTouched = true;
        else ++s.stats.targetPasses;
        ++s.stats.passes;
        if (!colourClear) ++s.stats.passResumes;
        return true;
    }

    void EndPass()
    {
        if (!s.passOpen) return;
        // A query begun in this pass instance has to end in it.
        if (s.queryActive)
        {
            s.dapi->CmdEndQuery(s.slots[s.fi].cmd, s.queryPool, s.queryIndex);
            s.queryActive = false;
            ++s.stats.queriesCut;
        }
        s.dapi->CmdEndRenderPass(s.slots[s.fi].cmd);
        s.passOpen = false;
        s.passFb   = UINT32_MAX;
    }

    /// Ends, submits and -- with `present`, when an image was acquired -- presents the open frame.
    void FinishFrame(bool present)
    {
        if (!s.open) return;
        const context::Handles* h    = context::Get();
        const loader::Device&   d    = *s.dapi;
        FrameSlot&              slot = s.slots[s.fi];

        // A pending clear is applied, not dropped: the next frame starts with none (a Flush or a
        // read-back cuts the frame here and goes on on the same targets).
        if (s.clearColourPending || s.clearDepthPending) BeginPass();
        EndPass();
        StagingFrameEnd(slot);

        VkResult r = d.EndCommandBuffer(slot.pre);
        if (r == VK_SUCCESS) r = d.EndCommandBuffer(slot.cmd);
        if (r != VK_SUCCESS) return Fail("vkEndCommandBuffer", r);

        {
            // RENDERTHREAD: the render thread may be waiting this fence for a swapchain image it
            // last drew; the serial goes with the reset, so the thread can tell it was reused.
            std::lock_guard<std::mutex> lock(gFenceLock);
            r           = d.ResetFences(s.device, 1, &slot.fence);
            slot.serial = s.serial;
        }
        if (r != VK_SUCCESS) return Fail("vkResetFences", r);

        // RENDERTHREAD: a presented frame goes to the render thread (acquire, present pass, submit,
        // present); the main thread moves on to the other slot. Everything else -- a flush, a
        // read-back, the release -- is submitted here, behind whatever the thread still holds.
        if (present && !s.acquired && RtHandOff(s.fi, s.serial, s.rtPresent))
        {
            s.submitted = s.serial;
            ++s.stats.frames;
            s.open      = false;
            s.rtPresent = false;
            s.fi        = (s.fi + 1) % kFramesInFlight;
            return;
        }
        Drain();

        // The pre-pass first, then the main buffer. The present pass's first use of the swapchain
        // image is its colour output, so that stage waits for the acquire.
        const VkCommandBuffer      buffers[2] = { slot.pre, slot.cmd };
        const VkPipelineStageFlags wait       = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        // Acquire() runs only in Present, right before FinishFrame(true): an acquired image is
        // always presented here.
        const bool                 presenting = present && s.acquired;
        VkSubmitInfo submit = {};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 2;
        submit.pCommandBuffers    = buffers;
        if (presenting)
        {
            submit.waitSemaphoreCount   = 1;
            submit.pWaitSemaphores      = &slot.imageAvailable;
            submit.pWaitDstStageMask    = &wait;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores    = &s.renderFinished[s.image];
        }
        r = d.QueueSubmit(h->graphicsQueue, 1, &submit, slot.fence);
        if (r != VK_SUCCESS) return Fail("vkQueueSubmit", r);
        gSubmitted.store(s.serial);
        s.submitted = s.serial;
        ++s.stats.frames;

        if (presenting)
        {
            VkPresentInfoKHR info = {};
            info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores    = &s.renderFinished[s.image];
            info.swapchainCount     = 1;
            info.pSwapchains        = &h->swapchain;
            info.pImageIndices      = &s.image;
            r = d.QueuePresentKHR(h->presentQueue, &info);
            ++s.presentedCount;
            ++s.stats.presented;
            // The present's semaphore wait still happens on a rejected present, so nothing is leaked.
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) s.rebuild = true;
            else if (r == VK_ERROR_SURFACE_LOST_KHR)
            {
                s.rebuild     = true;
                s.surfaceLost = true;
                Write(Debug, "vulkan: frame: vkQueuePresentKHR answered VK_ERROR_SURFACE_LOST_KHR; the surface is remade");
            }
            else if (r != VK_SUCCESS) Fail("vkQueuePresentKHR", r);
            if (s.presentedCount == 1)
                Write(Debug, "vulkan: frame: first frame presented (%ux%u, %u images)", s.extent.width, s.extent.height, s.imageCount);
        }
        s.open     = false;
        s.acquired = false;
        s.fi       = (s.fi + 1) % kFramesInFlight;
    }
}

namespace gxvk::vk::frame
{
    void SetTarget(const texture::Image* image, uint32_t layer, uint32_t level)
    {
        if (s.target.image == image && s.target.layer == layer && s.target.level == level) return;
        // A clear asked for the old target before its pass opened is applied to it first.
        if (s.open && (s.clearColourPending || s.clearDepthPending)) BeginPass();
        if (s.open) EndPass();
        s.target.image       = image;
        s.target.layer       = layer;
        s.target.level       = level;
        s.clearColourPending = false;
        s.clearDepthPending  = false;
    }

    const texture::Image* Scene() { return s.scene.handle != VK_NULL_HANDLE ? &s.scene : nullptr; }

    const texture::Image* Target() { return s.target.image; }

    VkFormat DepthFormat() { return s.depthFormat; }

    void ForgetImage(const texture::Image& image)
    {
        if (image.handle == VK_NULL_HANDLE || !s.dapi) return;
        if (s.target.image == &image)
        {
            if (s.open) EndPass();
            s.target = detail::Target();
        }
        DropFramebuffersOf(image.handle, true);
    }

    void Clear(uint32_t mask, const float colour[4], const VkRect2D* rect)
    {
        if (!(mask & 7u) || !EnsureFrame()) return;
        if (s.passOpen)
        {
            ClearInPass(mask, colour, rect);
            return;
        }
        const texture::Image* image  = nullptr;
        VkFormat              format = VK_FORMAT_UNDEFINED;
        VkExtent2D            extent = { 0, 0 };
        const bool known = TargetView(s.target, image, format, extent);
        // A clear of the whole target, or of a target that holds nothing yet, is the next pass's
        // load op; a partial clear of a target with contents opens the pass and clears in place.
        // A stencil-only clear has no load op of its own (the stencil loads with the depth): it opens the pass.
        if ((mask & 6u) != 4u && (!known || Whole(rect, extent) || image->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
        {
            if (mask & 1u)
            {
                s.clearColourPending = true;
                for (uint32_t i = 0; i < 4; ++i) s.clearColour[i] = colour[i];
            }
            if (mask & 2u) s.clearDepthPending = true;
            return;
        }
        if (!BeginPass()) return;
        ClearInPass(mask, colour, rect);
    }

    VkCommandBuffer Pass()
    {
        if (!EnsureFrame()) return nullptr;
        if (!BeginPass()) return nullptr;
        return s.slots[s.fi].cmd;
    }

    bool BeginQuery(VkQueryPool pool, uint32_t index, bool precise)
    {
        if (pool == VK_NULL_HANDLE || s.queryActive) return false;
        VkCommandBuffer cmd = Pass();
        if (!cmd) return false;
        s.dapi->CmdBeginQuery(cmd, pool, index, precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0u);
        s.queryActive = true;
        s.queryPool   = pool;
        s.queryIndex  = index;
        ++s.stats.queries;
        return true;
    }

    uint64_t EndQuery()
    {
        if (!s.open) return 0;
        if (s.queryActive && s.passOpen) s.dapi->CmdEndQuery(s.slots[s.fi].cmd, s.queryPool, s.queryIndex);
        s.queryActive = false;
        return s.serial;
    }

    bool QueryActive() { return s.open && s.queryActive; }

    void Flush()
    {
        Drain();   // RENDERTHREAD: what the thread holds is submitted before the open frame
        if (!s.open) return;
        FinishFrame(false);
        ++s.stats.flushes;
    }

    VkRenderPass RenderPass()
    {
        const texture::Image* image  = nullptr;
        VkFormat              format = VK_FORMAT_UNDEFINED;
        VkExtent2D            extent = { 0, 0 };
        if (s.target.image)
        {
            format = s.target.image->format;
        }
        else if (s.scene.handle != VK_NULL_HANDLE)
        {
            format = s.scene.format;
        }
        else if (!TargetView(s.target, image, format, extent))
            return VK_NULL_HANDLE;
        const PassSet* passes = PassesFor(format, PassSamples());
        return passes ? passes->pass[0] : VK_NULL_HANDLE;
    }

    uint32_t PassGeneration() { return s.passGen; }

    uint32_t PassSamples() { return s.target.image ? 1u : s.sceneSamples; }

    void SetSamples(uint32_t samples) { s.wantSamples = samples > 1 ? samples : 1u; }

    VkExtent2D Extent()
    {
        if (s.target.image) return s.target.image->extent;
        return s.scene.handle != VK_NULL_HANDLE ? s.scene.extent : s.extent;
    }

    uint32_t Slot() { return s.fi; }

    uint64_t Serial() { return s.open ? s.serial : s.serial + 1; }

    uint64_t Completed() { return s.completed; }

    // RENDERTHREAD: the serial vkQueueSubmit has seen, not the one handed to the thread: a query of
    // a frame still with the thread is not available yet, and Result() flushes (drains) for it.
    uint64_t Submitted() { return gSubmitted.load(); }

    bool UsedThisFrame(uint64_t serial) { return s.open && serial == s.serial; }

    bool WaitSerial(uint64_t serial)
    {
        if (serial <= s.completed) return true;
        Drain();   // RENDERTHREAD: a serial handed to the thread has its fence submitted only then
        if (s.failed || serial > s.submitted || !s.dapi) return false;
        for (FrameSlot& slot : s.slots)
        {
            if (slot.serial < serial) continue;
            const VkResult r = s.dapi->WaitForFences(s.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
            if (r != VK_SUCCESS)
            {
                Fail("vkWaitForFences (a retired serial)", r);
                return false;
            }
            ++s.stats.serialWaits;
            if (slot.serial > s.completed) s.completed = slot.serial;
            break;
        }
        Poll();
        return serial <= s.completed;
    }

    void RetireBuffer(memory::Buffer& buffer, uint64_t lastUsed)
    {
        if (buffer.handle == VK_NULL_HANDLE && buffer.block.memory == VK_NULL_HANDLE) return;
        Retired e;
        e.buffer = buffer;
        Retire(e, lastUsed);
        buffer = memory::Buffer();
    }

    void RetireImage(texture::Image& image, uint64_t lastUsed)
    {
        if (image.handle == VK_NULL_HANDLE) return;
        ForgetImage(image);
        Retired e;
        e.image = image;
        Retire(e, lastUsed);
        image = texture::Image();
    }

    bool Present(bool visible)
    {
        if (s.failed) return false;
        if (!EnsureFrame()) return false;

        if (visible)
        {
            // The scene gets a pass when nothing opened one (its clear colour is then what the
            // window shows), and the present pass puts it on the acquired swapchain image.
            if (!s.sceneTouched)
            {
                const detail::Target kept = s.target;
                SetTarget(nullptr, 0);
                BeginPass();
                EndPass();
                s.target = kept;
            }
            // A clear still pending on the current target (the scene touched earlier, then cleared
            // with no draw after it) goes in before the present pass reads the scene.
            if (s.clearColourPending || s.clearDepthPending) BeginPass();
            EndPass();
            if (RtWanted())
            {
                // RENDERTHREAD: the render thread acquires and records the present pass; here only
                // the swapchain's objects are made sure of (a rebuild of them drains the thread).
                const context::Handles* h = context::Get();
                if (s.scene.handle != VK_NULL_HANDLE && s.scene.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && h)
                {
                    if (h->swapchain == VK_NULL_HANDLE || h->extent.width == 0 || h->extent.height == 0) s.rebuild = true;
                    else s.rtPresent = EnsureImageObjects(*h) && present::Ready();
                }
            }
            else if (s.scene.handle != VK_NULL_HANDLE && s.scene.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && Acquire())
            {
                if (!present::Record(s.slots[s.fi].cmd, s.image, s.fi))
                {
                    // Nothing can be drawn on the acquired image: it is presented as it is rather
                    // than left acquired.
                    if (!s.loggedNoPresent)
                    {
                        s.loggedNoPresent = true;
                        Write(Error, "vulkan: frame: the present pass cannot be recorded; the window shows nothing");
                    }
                }
            }
        }

        FinishFrame(true);
        memory::Tick();
        RtCollect();   // RENDERTHREAD: a stale swapchain the thread saw is rebuilt a frame late

        if (s.rebuild && !s.failed)
        {
            Drain();   // RENDERTHREAD: nothing may be acquiring or presenting while the swapchain goes
            // Out of date, suboptimal, a lost surface, or a WM_SIZE: rebuilt once, here, between
            // frames. The caller does the context's half (Rebind) with the window it owns; a lost
            // surface is dropped first so Rebind remakes it with the swapchain.
            s.rebuild = false;
            if (s.dapi) s.dapi->DeviceWaitIdle(s.device);
            s.completed = s.submitted;
            CollectRetired(true);
            DropImageObjects();
            if (s.surfaceLost)
            {
                s.surfaceLost = false;
                context::ReleaseWindow();
            }
            return true;
        }
        return false;
    }

    void RequestRebuild()
    {
        // Also with nothing standing (the swapchain went down while the window was minimised): the
        // rebuild at the next Present is what brings it back at the restored size.
        if (s.dapi) s.rebuild = true;
    }

    void ReleaseSwapchain()
    {
        if (!s.dapi || !s.device || !context::Live()) return;
        FinishFrame(true);
        Drain();   // RENDERTHREAD
        s.dapi->DeviceWaitIdle(s.device);
        s.completed = s.submitted;
        CollectRetired(true);
        DropImageObjects();
        s.rebuild = false;
    }

    void Destroy()
    {
        if (!s.dapi || !s.device) return;
        if (context::Live())
        {
            ReleaseSwapchain();
            RtStop(true);   // RENDERTHREAD: drained by the release; joined before its fences go
            DestroyDeviceObjects();
        }
        else RtStop(false);
        delete[] s.retired;
        s = State();
        gSubmitted.store(0);
    }

    uint64_t Count() { return s.presentedCount; }

    bool Failed() { return s.failed; }

    void GetStats(Stats& out)
    {
        out = s.stats;
        out.presented += RtPresented();
    }
}
