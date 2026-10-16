// The frame's own objects: the command pool and buffers, fences, semaphores; the retirement list.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame/Internal.hpp"

#include <new>

namespace gxvk::vk::frame::detail
{
    State s;

    void Fail(const char* what, VkResult r)
    {
        s.failed = true;
        Write(Error, "vulkan: frame: %s failed (%s); the frame stops, the client keeps running with nothing on its window",
              what, ResultName(r));
    }

    bool EnsureDeviceObjects(const context::Handles& h)
    {
        if (s.pool != VK_NULL_HANDLE && s.device == h.device) return true;
        if (s.pool != VK_NULL_HANDLE)
        {
            // A new VkDevice: the old one is gone, so its objects are forgotten, never destroyed.
            Write(Warn, "vulkan: frame: the Vulkan device changed; the frame's old objects are forgotten, not destroyed");
            delete[] s.retired;
            s = State();
        }
        const loader::Device& d = *h.dapi;
        s.device = h.device;
        s.dapi   = h.dapi;

        VkCommandPoolCreateInfo pool = {};
        pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // RESET_COMMAND_BUFFER so each vkBeginCommandBuffer resets implicitly: the loader table has
        // no vkResetCommandBuffer and needs none with this flag.
        pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = h.graphicsFamily;
        VkResult r = d.CreateCommandPool(h.device, &pool, nullptr, &s.pool);
        if (r != VK_SUCCESS)
        {
            s.pool = VK_NULL_HANDLE;
            Fail("vkCreateCommandPool", r);
            return false;
        }

        // Two buffers per slot: the pre-pass, then the main one.
        VkCommandBuffer buffers[2 * kFramesInFlight] = {};
        VkCommandBufferAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool        = s.pool;
        alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 2 * kFramesInFlight;
        r = d.AllocateCommandBuffers(h.device, &alloc, buffers);
        if (r != VK_SUCCESS)
        {
            Fail("vkAllocateCommandBuffers", r);
            return false;
        }

        VkSemaphoreCreateInfo sem = {};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fence = {};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // the first wait on each slot returns at once
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            s.slots[i].pre = buffers[2 * i];
            s.slots[i].cmd = buffers[2 * i + 1];
            r = d.CreateSemaphore(h.device, &sem, nullptr, &s.slots[i].imageAvailable);
            if (r == VK_SUCCESS) r = d.CreateFence(h.device, &fence, nullptr, &s.slots[i].fence);
            if (r != VK_SUCCESS)
            {
                Fail("vkCreateSemaphore / vkCreateFence", r);
                return false;
            }
        }
        return ChooseDepthFormat(h);
    }

    /// The slots (their staging rings included), the targets and the pool. The caller has waited
    /// for the device and dropped the image objects.
    void DestroyDeviceObjects()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        DestroyTargets();
        for (FrameSlot& slot : s.slots)
        {
            StagingDestroy(slot);
            if (slot.imageAvailable != VK_NULL_HANDLE) d.DestroySemaphore(s.device, slot.imageAvailable, nullptr);
            if (slot.fence != VK_NULL_HANDLE) d.DestroyFence(s.device, slot.fence, nullptr);
            slot = FrameSlot();
        }
        // vkDestroyCommandPool frees every buffer allocated from the pool.
        if (s.pool != VK_NULL_HANDLE) d.DestroyCommandPool(s.device, s.pool, nullptr);
        s.pool = VK_NULL_HANDLE;
    }

    /// Learns, without blocking, which submitted frames have finished: one fence status per slot.
    void Poll()
    {
        if (!s.dapi || !s.device || !s.dapi->GetFenceStatus) return;
        for (FrameSlot& slot : s.slots)
            if (slot.serial > s.completed && slot.fence != VK_NULL_HANDLE
                && s.dapi->GetFenceStatus(s.device, slot.fence) == VK_SUCCESS)
                s.completed = slot.serial;
    }

    /// Destroys what a retired entry holds.
    static void DestroyRetired(Retired& e)
    {
        memory::DestroyBuffer(e.buffer);
        texture::DestroyIdle(e.image);
        if (e.framebuffer != VK_NULL_HANDLE) s.dapi->DestroyFramebuffer(s.device, e.framebuffer, nullptr);
        if (e.view != VK_NULL_HANDLE) s.dapi->DestroyImageView(s.device, e.view, nullptr);
        if (e.rawImage != VK_NULL_HANDLE) s.dapi->DestroyImage(s.device, e.rawImage, nullptr);
        memory::Free(e.rawBlock);
        ++s.stats.retired;
    }

    /**
     * Destroys the retired resources whose frame has completed; with `all`, every one (the caller
     * has waited for the device).
     */
    void CollectRetired(bool all)
    {
        uint32_t keep = 0;
        for (uint32_t i = 0; i < s.retiredCount; ++i)
        {
            Retired& e = s.retired[i];
            if (all || e.serial <= s.completed)
            {
                DestroyRetired(e);
                continue;
            }
            if (keep != i) s.retired[keep] = e;
            ++keep;
        }
        for (uint32_t i = keep; i < s.retiredCount; ++i) s.retired[i] = Retired();
        s.retiredCount = keep;
    }

    /**
     * Destroys the entry's resource at once when the frame that last named it has completed, else
     * lists it for a later CollectRetired(). The open frame's serial is never completed, so a
     * resource it named waits for it; the fences are polled only when the serial was submitted.
     */
    void Retire(Retired& entry, uint64_t lastUsed)
    {
        if (lastUsed > s.completed && lastUsed <= s.submitted) Poll();
        if (lastUsed <= s.completed)
        {
            DestroyRetired(entry);
            return;
        }
        if (s.retiredCount == s.retiredCap)
        {
            if (s.retiredCap >= kMaxRetired)
            {
                ++s.stats.retireLeaked;
                return;
            }
            const uint32_t cap   = s.retiredCap ? s.retiredCap * 2 : kRetiredFirst;
            Retired*       fresh = new (std::nothrow) Retired[cap];
            if (!fresh)
            {
                ++s.stats.retireLeaked;
                return;
            }
            for (uint32_t i = 0; i < s.retiredCount; ++i) fresh[i] = s.retired[i];
            delete[] s.retired;
            s.retired    = fresh;
            s.retiredCap = cap;
        }
        Retired& e = s.retired[s.retiredCount++];
        e        = entry;
        e.serial = lastUsed;
        if (s.retiredCount > s.stats.retirePeak) s.stats.retirePeak = s.retiredCount;
    }
}
