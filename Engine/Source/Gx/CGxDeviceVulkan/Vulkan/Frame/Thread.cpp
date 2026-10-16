// RENDERTHREAD: the render thread (CVar renderThread, opt-in). The main thread records a frame's
// pre-pass and main command buffers exactly as before, ends them, resets the slot's fence and hands
// the slot to this thread; this thread acquires the swapchain image, records the present pass into
// a third per-slot buffer from a command pool of its own, submits the three, and presents. The main
// thread goes on recording the next frame into the other slot meanwhile, and only blocks on this
// thread when it comes back to a slot (kFramesInFlight frames later), or when something needs the
// queue or the swapchain to itself (Drain()).
//
// What this thread touches, and why it is safe:
//   - VkQueue (submit, present), the swapchain (acquire, present): only here while the thread runs;
//     every main-thread path that uses them (Flush, ReadScene, WaitSerial, a swapchain rebuild,
//     DeviceWaitIdle, the release and the teardown) drains first.
//   - slot fi's buffers, fence and acquire semaphore: the main thread does not touch slot fi again
//     until RtWait(slot.serial) said this thread is done with it.
//   - s.images / views / renderFinished / imagesInFlight / swapGen and the present pass's state:
//     changed by the main thread only after a drain (EnsureImageObjects, DropImageObjects).
//   - Logging is not: a failure or a rebuild is a flag the main thread picks up and logs.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame/Internal.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Present.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace gxvk::vk::frame::detail;

namespace
{
    namespace present = gxvk::vk::present;

    struct Job
    {
        uint32_t fi      = 0;
        uint64_t serial  = 0;
        bool     present = false;
    };

    constexpr uint32_t kQueue = kFramesInFlight;   // the main thread never has more slots handed off

    struct Rt
    {
        bool                    wanted  = false;   ///< the CVar
        bool                    gaveUp  = false;   ///< the thread could not start; off until the CVar is turned off and on
        bool                    running = false;
        std::thread             thread;
        std::mutex              m;
        std::condition_variable work;
        std::condition_variable done;
        Job                     q[kQueue];
        uint32_t                head = 0, count = 0;
        uint64_t                finished = 0;      ///< the last serial this thread is done with
        bool                    stop = false;

        VkCommandPool   pool = VK_NULL_HANDLE;
        VkCommandBuffer post[kFramesInFlight] = {};

        std::atomic<bool>     rebuild{ false };
        std::atomic<bool>     surfaceLost{ false };
        std::atomic<bool>     failed{ false };
        const char*           failWhat   = nullptr;   ///< written before `failed` is set
        VkResult              failResult = VK_SUCCESS;
        std::atomic<uint64_t> presented{ 0 };
    };
    Rt rt;

    void RtFail(const char* what, VkResult r)
    {
        rt.failWhat   = what;
        rt.failResult = r;
        rt.failed.store(true);
    }

    /// One handed-off frame: acquire, the present pass, the submit, the present. Mirrors
    /// Acquire() + FinishFrame(true) on the main thread.
    void RunJob(const Job& job)
    {
        const context::Handles* h    = context::Get();
        const loader::Device&   d    = *s.dapi;
        FrameSlot&              slot = s.slots[job.fi];

        bool     presenting = false;
        uint32_t image      = 0;
        if (!h) return RtFail("the context (render thread)", VK_ERROR_DEVICE_LOST);
        if (job.present && h && h->swapchain != VK_NULL_HANDLE && s.imageCount && s.swapGen == context::SwapchainGeneration())
        {
            VkResult r = d.AcquireNextImageKHR(s.device, h->swapchain, kAcquireTimeoutNs, slot.imageAvailable, VK_NULL_HANDLE, &image);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
            {
                if (r == VK_ERROR_SURFACE_LOST_KHR) rt.surfaceLost.store(true);
                rt.rebuild.store(true);
            }
            else if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
            {
                if (r == VK_SUBOPTIMAL_KHR) rt.rebuild.store(true);
                if (image < s.imageCount)
                {
                    if (s.imagesInFlight[image] != VK_NULL_HANDLE && s.imagesInFlight[image] != slot.fence)
                    {
                        // The other slot's fence, unless the main thread has reset it for a newer
                        // frame since (it waited the old one first, so that frame is done).
                        std::lock_guard<std::mutex> lock(gFenceLock);
                        for (const FrameSlot& other : s.slots)
                            if (other.fence == s.imagesInFlight[image] && other.serial == s.imageSerial[image])
                                d.WaitForFences(s.device, 1, &other.fence, VK_TRUE, UINT64_MAX);
                    }
                    s.imagesInFlight[image] = slot.fence;
                    s.imageSerial[image]    = job.serial;
                    VkCommandBufferBeginInfo begin = {};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    // As on the main thread: a present pass that cannot be recorded presents the image as it is.
                    if (d.BeginCommandBuffer(rt.post[job.fi], &begin) != VK_SUCCESS) return RtFail("vkBeginCommandBuffer (render thread)", VK_ERROR_UNKNOWN);
                    present::Record(rt.post[job.fi], image, job.fi);
                    if (d.EndCommandBuffer(rt.post[job.fi]) != VK_SUCCESS) return RtFail("vkEndCommandBuffer (render thread)", VK_ERROR_UNKNOWN);
                    presenting = true;
                }
                else return RtFail("vkAcquireNextImageKHR (an image past the swapchain's, render thread)", VK_ERROR_UNKNOWN);
            }
            else if (r != VK_TIMEOUT && r != VK_NOT_READY) return RtFail("vkAcquireNextImageKHR (render thread)", r);
        }

        const VkCommandBuffer      buffers[3] = { slot.pre, slot.cmd, rt.post[job.fi] };
        const VkPipelineStageFlags wait       = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = presenting ? 3 : 2;
        submit.pCommandBuffers    = buffers;
        if (presenting)
        {
            submit.waitSemaphoreCount   = 1;
            submit.pWaitSemaphores      = &slot.imageAvailable;
            submit.pWaitDstStageMask    = &wait;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores    = &s.renderFinished[image];
        }
        VkResult r = d.QueueSubmit(h->graphicsQueue, 1, &submit, slot.fence);
        if (r != VK_SUCCESS) return RtFail("vkQueueSubmit (render thread)", r);
        gSubmitted.store(job.serial);

        if (presenting)
        {
            VkPresentInfoKHR info = {};
            info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores    = &s.renderFinished[image];
            info.swapchainCount     = 1;
            info.pSwapchains        = &h->swapchain;
            info.pImageIndices      = &image;
            r = d.QueuePresentKHR(h->presentQueue, &info);
            rt.presented.fetch_add(1);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) rt.rebuild.store(true);
            else if (r == VK_ERROR_SURFACE_LOST_KHR)
            {
                rt.surfaceLost.store(true);
                rt.rebuild.store(true);
            }
            else if (r != VK_SUCCESS) RtFail("vkQueuePresentKHR (render thread)", r);
        }
    }

    void Loop()
    {
        std::unique_lock<std::mutex> lock(rt.m);
        for (;;)
        {
            rt.work.wait(lock, [] { return rt.stop || rt.count; });
            if (!rt.count) return;   // stop, and nothing left
            const Job job = rt.q[rt.head];
            lock.unlock();
            // After a failure nothing is submitted any more; the main thread sees the flag before it
            // waits a fence this thread never submitted.
            if (!rt.failed.load()) RunJob(job);
            lock.lock();
            rt.head = (rt.head + 1) % kQueue;
            --rt.count;
            rt.finished = job.serial;
            rt.done.notify_all();
        }
    }

    bool Start()
    {
        VkCommandPoolCreateInfo pool = {};
        pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = context::Get()->graphicsFamily;
        if (s.dapi->CreateCommandPool(s.device, &pool, nullptr, &rt.pool) != VK_SUCCESS)
        {
            rt.pool = VK_NULL_HANDLE;
            return false;
        }
        VkCommandBufferAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool        = rt.pool;
        alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = kFramesInFlight;
        if (s.dapi->AllocateCommandBuffers(s.device, &alloc, rt.post) != VK_SUCCESS)
        {
            s.dapi->DestroyCommandPool(s.device, rt.pool, nullptr);
            rt.pool = VK_NULL_HANDLE;
            return false;
        }
        rt.stop     = false;
        rt.head     = 0;
        rt.count    = 0;
        rt.finished = 0;
        rt.failed.store(false);
        rt.thread  = std::thread(Loop);
        rt.running = true;
        Write(Info, "vulkan: frame: render thread started (submit + present off the main thread)");
        return true;
    }
}

namespace gxvk::vk::frame::detail
{
    std::atomic<uint64_t> gSubmitted{ 0 };
    std::mutex            gFenceLock;

    bool RtWanted() { return rt.wanted; }

    void RtWait(uint64_t serial)
    {
        if (!rt.running) return;
        std::unique_lock<std::mutex> lock(rt.m);
        rt.done.wait(lock, [serial] { return rt.finished >= serial || !rt.count; });
    }

    void RtCollect()
    {
        if (rt.rebuild.exchange(false)) s.rebuild = true;
        if (rt.surfaceLost.exchange(false))
        {
            s.surfaceLost = true;
            Write(Debug, "vulkan: frame: the render thread saw VK_ERROR_SURFACE_LOST_KHR; the surface is remade");
        }
        if (rt.failed.load() && !s.failed) Fail(rt.failWhat ? rt.failWhat : "the render thread", rt.failResult);
    }

    bool RtHandOff(uint32_t fi, uint64_t serial, bool wantPresent)
    {
        if (!rt.running && !rt.wanted) return false;
        if (!rt.running && !Start())
        {
            rt.wanted = false;
            rt.gaveUp = true;
            Write(Warn, "vulkan: frame: the render thread's command pool could not be made; frames are submitted on the main thread");
            return false;
        }
        std::unique_lock<std::mutex> lock(rt.m);
        rt.done.wait(lock, [] { return rt.count < kQueue; });
        rt.q[(rt.head + rt.count) % kQueue] = Job{ fi, serial, wantPresent };
        ++rt.count;
        rt.work.notify_one();
        return true;
    }

    void RtStop(bool destroy)
    {
        if (!rt.running) return;
        {
            std::lock_guard<std::mutex> lock(rt.m);
            rt.stop = true;
        }
        rt.work.notify_one();
        rt.thread.join();   // the loop drains the queue first
        rt.running = false;
        if (destroy && rt.pool != VK_NULL_HANDLE) s.dapi->DestroyCommandPool(s.device, rt.pool, nullptr);
        rt.pool = VK_NULL_HANDLE;
        for (VkCommandBuffer& b : rt.post) b = nullptr;
        RtCollect();
        Write(Info, "vulkan: frame: render thread stopped");
    }

    uint64_t RtPresented() { return rt.presented.load(); }
}

namespace gxvk::vk::frame
{
    void SetThreaded(bool on)
    {
        if (!on) rt.gaveUp = false;
        if (on == rt.wanted || rt.gaveUp) return;
        rt.wanted = on;
        if (!on && rt.running)
        {
            Drain();
            RtStop(true);
        }
    }

    void Drain()
    {
        if (!rt.running) return;
        {
            std::unique_lock<std::mutex> lock(rt.m);
            rt.done.wait(lock, [] { return !rt.count; });
        }
        RtCollect();
    }
}
