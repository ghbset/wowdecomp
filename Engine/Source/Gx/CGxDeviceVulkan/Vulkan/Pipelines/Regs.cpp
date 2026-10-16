// The uniform ring: the per-frame-slot buffers the translated interface's constant slices are
// written into, one set-1 descriptor per chunk, a dynamic offset per draw.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines/Internal.hpp"

#include <cstring>

namespace
{
    using namespace gxvk::vk::pipelines::detail;

    /// A chunk of the ring: a mapped uniform buffer and its set-1 descriptor over the whole buffer
    /// with the larger slice class as the range; the per-draw dynamic offset selects the slice.
    bool CreateChunk(SlotSets& ss)
    {
        if (ss.regsCount >= kMaxRegsChunks) return false;
        const loader::Device& d = *s.dapi;
        memory::Buffer& b = ss.regs[ss.regsCount];
        if (!memory::CreateBuffer(kRegsChunkBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, b) || !b.block.mapped)
        {
            memory::DestroyBuffer(b);
            if (!s.warnedRegs)
            {
                s.warnedRegs = true;
                Write(Error, "vulkan: pipelines: no host-visible uniform chunk of %u KiB; draws of the translated shaders are skipped (logged once)", kRegsChunkBytes / 1024);
            }
            return false;
        }
        // Zeroed once: a row the engine never wrote reads as D3D's initial zero.
        std::memset(b.block.mapped, 0, kRegsChunkBytes);

        VkDescriptorSetAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = s.regsPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &s.regsLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = d.AllocateDescriptorSets(s.device, &alloc, &set);
        if (r != VK_SUCCESS)
        {
            memory::DestroyBuffer(b);
            if (!s.warnedRegs)
            {
                s.warnedRegs = true;
                Write(Error, "vulkan: pipelines: vkAllocateDescriptorSets (set 1) failed: %s (logged once)", ResultName(r));
            }
            return false;
        }
        VkDescriptorBufferInfo info = {};
        info.buffer = b.handle;
        info.offset = 0;
        info.range  = kSliceBytesLarge;
        VkWriteDescriptorSet write = {};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.pBufferInfo     = &info;
        d.UpdateDescriptorSets(s.device, 1, &write, 0, nullptr);

        ss.regsSet[ss.regsCount] = set;
        ++ss.regsCount;
        Write(Debug, "vulkan: pipelines: uniform ring chunk #%u (%u KiB) made for a frame slot", ss.regsCount, kRegsChunkBytes / 1024);
        return true;
    }
}

namespace gxvk::vk::pipelines::detail
{
    /**
     * One uniform slice for a draw of the translated family: p[16] from the pixel register file,
     * misc, then v[rows] from the vertex register file, written at the next aligned offset of the
     * slot's ring (the ring is rewound when a new frame opens on the slot: its last frame has
     * completed). A slice identical to the one written just before is bound again rather than
     * written.
     *
     * @param VkDescriptorSet& set : receives the chunk's set-1 descriptor
     * @param uint32& offset : receives the dynamic offset of the slice
     * @return bool ok : false when the ring is exhausted or could not be made (counted)
     */
    bool RegsSlice(uint32_t slot, uint64_t serial, const Draw& draw, VkDescriptorSet& set, uint32_t& offset)
    {
        if (slot >= frame::kFramesInFlight || !draw.vertexRegs || !draw.pixelRegs) return false;
        SlotSets& ss = s.slots[slot];
        const uint32_t rows  = draw.vertexRows == kVertexRowsLarge ? kVertexRowsLarge : kVertexRowsSmall;
        const uint32_t bytes = kSliceHeadBytes + rows * 16;

        if (ss.regsSerial != serial)
        {
            ss.regsSerial     = serial;
            ss.regsUsed       = 0;
            ss.regsCursor     = 0;
            ss.lastSliceBytes = 0;
            ss.lastSliceSet   = VK_NULL_HANDLE;
        }

        uint8_t slice[kSliceBytesLarge];
        std::memcpy(slice, draw.pixelRegs, kPixelRows * 16);
        std::memcpy(slice + kPixelRows * 16, draw.misc, 16);
        std::memcpy(slice + kSliceHeadBytes, draw.vertexRegs, rows * 16);

        if (ss.lastSliceSet != VK_NULL_HANDLE && ss.lastSliceBytes == bytes && std::memcmp(ss.lastSlice, slice, bytes) == 0)
        {
            set    = ss.lastSliceSet;
            offset = ss.lastSliceOffset;
            ++s.stats.regsReused;
            return true;
        }

        // The slice at the next aligned offset of the current chunk; the next chunk when it does
        // not fit (the descriptor's range must stay inside the buffer, so the larger class bounds
        // the fit whatever the slice's own size).
        while (true)
        {
            if (ss.regsUsed > 0)
            {
                const uint32_t at = (ss.regsCursor + s.regsAlign - 1) / s.regsAlign * s.regsAlign;
                if (at + kSliceBytesLarge <= kRegsChunkBytes)
                {
                    memory::Buffer& b = ss.regs[ss.regsUsed - 1];
                    std::memcpy(static_cast<uint8_t*>(b.block.mapped) + at, slice, bytes);
                    memory::FlushHost(b.block);
                    ss.regsCursor = at + bytes;
                    set    = ss.regsSet[ss.regsUsed - 1];
                    offset = at;
                    break;
                }
            }
            if (ss.regsUsed == ss.regsCount && !CreateChunk(ss))
            {
                ++s.stats.regsFull;
                return false;
            }
            ++ss.regsUsed;
            ss.regsCursor = 0;
        }

        std::memcpy(ss.lastSlice, slice, bytes);
        ss.lastSliceBytes  = bytes;
        ss.lastSliceSet    = set;
        ss.lastSliceOffset = offset;
        ++s.stats.regsSlices;
        s.stats.regsBytes += bytes;
        return true;
    }

    /// Every chunk of a slot's ring; the caller has made the device idle. The sets go with the pool.
    void DestroyRegs(SlotSets& ss)
    {
        for (uint32_t i = 0; i < ss.regsCount; ++i)
        {
            memory::DestroyBuffer(ss.regs[i]);
            ss.regsSet[i] = VK_NULL_HANDLE;
        }
        ss.regsCount  = 0;
        ss.regsUsed   = 0;
        ss.regsCursor = 0;
    }
}
