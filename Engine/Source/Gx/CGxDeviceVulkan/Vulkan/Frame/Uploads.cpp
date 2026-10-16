// The frame's upload side: the pre-pass and outside-the-pass command buffers, and the staging ring
// a frame's uploads are written into.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame/Internal.hpp"

using namespace gxvk::vk::frame::detail;

namespace gxvk::vk::frame::detail
{
    /// The slot's ring at the frame's open: every chunk free again (the slot's fence was waited,
    /// so the frame that wrote them has completed), and a chunk nobody needed for a long run of
    /// frames given back.
    void StagingFrameBegin(FrameSlot& slot)
    {
        if (slot.chunkCount > slot.chunkUsed && slot.chunkCount > 1)
        {
            if (++slot.idleFrames >= kStagingTrimFrames)
            {
                memory::DestroyBuffer(slot.chunks[--slot.chunkCount]);
                --s.stats.stagingChunks;
                slot.idleFrames = 0;
            }
        }
        else
        {
            slot.idleFrames = 0;
        }
        slot.chunkUsed = 0;
        slot.cursor    = 0;
    }

    /// Before the submit: the host writes of this frame's staging made visible to the device, the
    /// dedicated buffers retired with the frame.
    void StagingFrameEnd(FrameSlot& slot)
    {
        for (uint32_t i = 0; i < slot.chunkUsed && i < slot.chunkCount; ++i) memory::FlushHost(slot.chunks[i].block);
        for (uint32_t i = 0; i < slot.dedicatedCount; ++i)
        {
            memory::FlushHost(slot.dedicated[i].block);
            RetireBuffer(slot.dedicated[i], s.serial);
        }
        slot.dedicatedCount = 0;
    }

    /// Everything the slot's ring holds; the caller has made the device idle.
    void StagingDestroy(FrameSlot& slot)
    {
        for (uint32_t i = 0; i < slot.chunkCount; ++i) memory::DestroyBuffer(slot.chunks[i]);
        for (uint32_t i = 0; i < slot.dedicatedCount; ++i) memory::DestroyBuffer(slot.dedicated[i]);
        slot.chunkCount     = 0;
        slot.chunkUsed      = 0;
        slot.cursor         = 0;
        slot.dedicatedCount = 0;
    }
}

namespace
{
    /// A staging buffer: host-visible, a copy source, mapped.
    bool CreateStaging(VkDeviceSize bytes, memory::Buffer& out)
    {
        if (!memory::CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, memory::Where::HostVisible, out) || !out.block.mapped)
        {
            memory::DestroyBuffer(out);
            return false;
        }
        return true;
    }
}

namespace gxvk::vk::frame
{
    VkCommandBuffer Outside()
    {
        if (!EnsureFrame()) return nullptr;
        // An active query pins the pass: the transfer goes ahead of the frame instead.
        if (s.passOpen && s.queryActive) return s.slots[s.fi].pre;
        // The next Pass() reopens on the same target with its contents loaded.
        if (s.passOpen) EndPass();
        return s.slots[s.fi].cmd;
    }

    VkCommandBuffer PrePass()
    {
        if (!EnsureFrame()) return nullptr;
        return s.slots[s.fi].pre;
    }

    bool Stage(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceSize& offset, void*& mapped)
    {
        if (bytes == 0 || !EnsureFrame()) return false;
        FrameSlot& slot = s.slots[s.fi];

        if (bytes <= kStagingChunk)
        {
            // The current chunk when the request fits past the cursor, else the next chunk of the
            // ring (made on first use), up to the ring's cap.
            while (true)
            {
                if (slot.chunkUsed > 0)
                {
                    const VkDeviceSize at = (slot.cursor + kStagingAlign - 1) / kStagingAlign * kStagingAlign;
                    if (at + bytes <= kStagingChunk)
                    {
                        memory::Buffer& c = slot.chunks[slot.chunkUsed - 1];
                        buffer      = c.handle;
                        offset      = at;
                        mapped      = static_cast<uint8_t*>(c.block.mapped) + at;
                        slot.cursor = at + bytes;
                        s.stats.stagedBytes += bytes;
                        return true;
                    }
                }
                if (slot.chunkUsed == slot.chunkCount)
                {
                    if (slot.chunkCount == kMaxStagingChunks) break;
                    if (!CreateStaging(kStagingChunk, slot.chunks[slot.chunkCount]))
                    {
                        if (!s.loggedStagingFail)
                        {
                            s.loggedStagingFail = true;
                            Write(Error, "vulkan: frame: no host-visible staging chunk of %llu bytes; uploads past the ring take buffers of their own (logged once)",
                                  static_cast<unsigned long long>(kStagingChunk));
                        }
                        break;
                    }
                    ++slot.chunkCount;
                    ++s.stats.stagingChunks;
                    if (s.stats.stagingChunks > s.stats.stagingPeak) s.stats.stagingPeak = s.stats.stagingChunks;
                }
                ++slot.chunkUsed;
                slot.cursor = 0;
            }
        }

        // Larger than a chunk, or the ring is full: a buffer for this request alone, retired with
        // the frame.
        if (slot.dedicatedCount == kMaxDedicated)
        {
            if (!s.loggedStagingFail)
            {
                s.loggedStagingFail = true;
                Write(Error, "vulkan: frame: a frame asked for more than %u dedicated staging buffers; further uploads of the frame are refused (logged once)", kMaxDedicated);
            }
            return false;
        }
        memory::Buffer& d = slot.dedicated[slot.dedicatedCount];
        if (!CreateStaging(bytes, d))
        {
            if (!s.loggedStagingFail)
            {
                s.loggedStagingFail = true;
                Write(Error, "vulkan: frame: no host-visible staging buffer of %llu bytes; the upload is refused (logged once)", static_cast<unsigned long long>(bytes));
            }
            return false;
        }
        ++slot.dedicatedCount;
        buffer = d.handle;
        offset = 0;
        mapped = d.block.mapped;
        s.stats.stagedBytes += bytes;
        ++s.stats.stagedDedicated;
        return true;
    }
}
