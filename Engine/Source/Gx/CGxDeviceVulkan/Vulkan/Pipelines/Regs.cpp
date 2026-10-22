// The uniform ring: the per-frame-slot buffers the translated interface's constant slices are
// written into, one set-1 descriptor per chunk, a dynamic offset per draw.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines/Internal.hpp"
#include "Engine/Source/Gx/GxLighting.h"

#include <cstring>

namespace
{
    using namespace gxvk::vk::pipelines::detail;

    /// LIGHTING: the distance between two copies of g_gxLightingGpu in a slot's buffer.
    uint32_t LightStride()
    {
        return (static_cast<uint32_t>(sizeof(GxLightingGpu)) + s.regsAlign - 1) / s.regsAlign * s.regsAlign;
    }

    /// A chunk of the ring: a mapped uniform buffer and its set-1 descriptor over the whole buffer
    /// with the larger slice class as the range; the per-draw dynamic offset selects the slice.
    bool CreateChunk(SlotSets& ss)
    {
        if (ss.regsCount >= kMaxRegsChunks) return false;
        const loader::Device& d = *s.dapi;
        // LIGHTING: the slot's GxLighting copies, made with its first chunk (every chunk's set points at them).
        if (ss.light.handle == VK_NULL_HANDLE
            && (!memory::CreateBuffer(kLightCopies * LightStride(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, ss.light) || !ss.light.block.mapped))
        {
            memory::DestroyBuffer(ss.light);
            if (!s.warnedRegs)
            {
                s.warnedRegs = true;
                Write(Error, "vulkan: pipelines: no host-visible GxLighting buffer; draws of the translated shaders are skipped (logged once)");
            }
            return false;
        }
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
        // Binding 0 the chunk, binding 1 the slot's GxLighting copies: one write of two descriptors runs on
        // into the next binding (same type and stages).
        VkDescriptorBufferInfo info[2] = {};
        info[0].buffer = b.handle;
        info[0].offset = 0;
        info[0].range  = kSliceBytesMax;
        info[1].buffer = ss.light.handle;
        info[1].offset = 0;
        info[1].range  = sizeof(GxLightingGpu);
        VkWriteDescriptorSet write = {};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 0;
        write.descriptorCount = 2;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.pBufferInfo     = info;
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
     * misc, then v[rows] from the vertex register file (all 256 rows, then pixel rows 16..pconst-1,
     * when the pixel record has pconst > 16), written at the next aligned offset of the
     * slot's ring (the ring is rewound when a new frame opens on the slot: its last frame has
     * completed). A slice identical to the one written just before is bound again rather than
     * written.
     *
     * @param VkDescriptorSet& set : receives the chunk's set-1 descriptor
     * @param uint32& offset : receives the dynamic offset of the slice
     * @param uint32& lightOffset : receives the dynamic offset of the GxLighting copy (binding 1)
     * @return bool ok : false when the ring is exhausted or could not be made (counted)
     */
    bool RegsSlice(uint32_t slot, uint64_t serial, const Draw& draw, VkDescriptorSet& set, uint32_t& offset, uint32_t& lightOffset)
    {
        if (slot >= frame::kFramesInFlight || !draw.vertexRegs || !draw.pixelRegs) return false;
        SlotSets& ss = s.slots[slot];
        const uint32_t extRows = draw.pixelRegsFull && draw.pixelRows > kPixelRows
            ? (draw.pixelRows < kPixelRowsMax ? draw.pixelRows : kPixelRowsMax) - kPixelRows : 0;
        const uint32_t rows  = (extRows || draw.vertexRows == kVertexRowsLarge) ? kVertexRowsLarge : kVertexRowsSmall;
        const uint32_t bytes = kSliceHeadBytes + rows * 16 + extRows * 16;

        if (ss.regsSerial != serial)
        {
            ss.regsSerial     = serial;
            ss.regsUsed       = 0;
            ss.regsCursor     = 0;
            ss.lastSliceBytes = 0;
            ss.lastSliceSet   = VK_NULL_HANDLE;
            ss.lastVertexGen  = 0;
            ss.lightUsed      = 0;
        }

        // LIGHTING: g_gxLightingGpu as it is now, a new copy whenever its serial changed (and on the frame's
        // first draw): the draws recorded before keep theirs.
        if (ss.regsCount == 0 && !CreateChunk(ss))
        {
            ++s.stats.regsFull;
            return false;
        }
        if (ss.lightUsed == 0 || ss.lightSerial != g_gxLightingGpuSerial)
        {
            // ponytail: past kLightCopies changes in one frame the last copy is rewritten under the draws that read it
            const uint32_t copy = ss.lightUsed < kLightCopies ? ss.lightUsed++ : kLightCopies - 1;
            ss.lightOffset = copy * LightStride();
            std::memcpy(static_cast<uint8_t*>(ss.light.block.mapped) + ss.lightOffset, &g_gxLightingGpu, sizeof(GxLightingGpu));
            memory::FlushHost(ss.light.block);
            ss.lightSerial = g_gxLightingGpuSerial;
        }
        lightOffset = ss.lightOffset;

        // The slice's pieces, compared in place against the last one written (no copy when it is
        // reused, the common case); a new slice is assembled once, in lastSlice, and copied out.
        struct Piece { uint32_t at; const void* src; uint32_t bytes; };
        const Piece pieces[4] = {
            { 0, draw.pixelRegs, kPixelRows * 16 },
            { kPixelRows * 16, draw.misc, 16 },
            { kSliceHeadBytes, draw.vertexRegs, rows * 16 },
            { kSliceBytesLarge, extRows ? draw.pixelRegsFull + kPixelRows * 4 : nullptr, extRows * 16 },
        };
        bool same = ss.lastSliceSet != VK_NULL_HANDLE && ss.lastSliceBytes == bytes;
        // The vertex rows (up to 4 KiB) unchanged since the last slice by their generation: not compared.
        const bool vertexSame = draw.vertexRegsGen != 0 && draw.vertexRegsGen == ss.lastVertexGen;
        for (uint32_t i = 0; same && i < 4; ++i)
            same = !pieces[i].bytes || (i == 2 && vertexSame) || std::memcmp(ss.lastSlice + pieces[i].at, pieces[i].src, pieces[i].bytes) == 0;
        if (same)
        {
            set    = ss.lastSliceSet;
            offset = ss.lastSliceOffset;
            ++s.stats.regsReused;
            return true;
        }
        ss.lastSliceSet = VK_NULL_HANDLE;   // lastSlice is overwritten before the write can fail
        for (const Piece& piece : pieces)
            if (piece.bytes) std::memcpy(ss.lastSlice + piece.at, piece.src, piece.bytes);
        const uint8_t* slice = ss.lastSlice;

        // The slice at the next aligned offset of the current chunk; the next chunk when it does
        // not fit (the descriptor's range must stay inside the buffer, so kSliceBytesMax bounds
        // the fit whatever the slice's own size).
        while (true)
        {
            if (ss.regsUsed > 0)
            {
                const uint32_t at = (ss.regsCursor + s.regsAlign - 1) / s.regsAlign * s.regsAlign;
                if (at + kSliceBytesMax <= kRegsChunkBytes)
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

        ss.lastSliceBytes  = bytes;
        ss.lastSliceSet    = set;
        ss.lastSliceOffset = offset;
        ss.lastVertexGen   = draw.vertexRegsGen;
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
        memory::DestroyBuffer(ss.light);
        ss.lightUsed  = 0;
    }
}
