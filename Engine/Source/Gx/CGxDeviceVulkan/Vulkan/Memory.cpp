// Device memory: the memory-type choice, a sub-allocator over a few large blocks, and buffers.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory/Internal.hpp"

using namespace gxvk::vk::memory::detail;

namespace gxvk::vk::memory
{
    bool Init()
    {
        if (s.ready) return true;

        const context::Handles* h = context::Get();
        if (!h || !h->iapi || !h->dapi)
        {
            Write(Warn, "vulkan: memory: Init() without a device; declining");
            return false;
        }
        if (!HaveEntryPoints(*h->iapi, *h->dapi)) return false;

        s.device = h->device;
        s.dapi   = h->dapi;

        h->iapi->GetPhysicalDeviceMemoryProperties(h->physical, &s.props);

        VkPhysicalDeviceProperties props = {};
        h->iapi->GetPhysicalDeviceProperties(h->physical, &props);
        // Both are powers of two by spec; the floor of 1 only protects RoundUp() from a zero a
        // broken driver might report, which would turn every mask into "align to everything".
        s.granularity    = props.limits.bufferImageGranularity ? props.limits.bufferImageGranularity : 1;
        s.atom           = props.limits.nonCoherentAtomSize ? props.limits.nonCoherentAtomSize : 1;
        s.maxAllocations = props.limits.maxMemoryAllocationCount;

        LogTable();

        // Resolved against "any resource" here, for the log and for the decline. Each Allocate()
        // resolves again against its own memoryTypeBits, because a resource is allowed to exclude
        // the type that suits most others, and the answer for it may then differ.
        bool ok = true;
        const Where wheres[] = { Where::DeviceLocal, Where::HostVisible, Where::Stream };
        for (Where w : wheres)
        {
            const char*    why  = "";
            const uint32_t type = Resolve(0xFFFFFFFFu, w, why);
            if (type == kNoType)
            {
                Write(Warn, "vulkan: memory: %s -> no usable memory type; declining", WhereName(w));
                ok = false;
                continue;
            }

            char text[128];
            const VkMemoryType& t = s.props.memoryTypes[type];
            const bool fallback   = std::strncmp(why, "FALLBACK", 8) == 0;
            Write(fallback ? Warn : Debug, "vulkan: memory: %s -> type %u (heap %u, %llu MiB, %s): %s", WhereName(w), type,
                  t.heapIndex, MiB(s.props.memoryHeaps[t.heapIndex].size), PropertyText(t.propertyFlags, text, sizeof text), why);
        }

        if (!ok)
        {
            s.device = nullptr;
            s.dapi   = nullptr;
            return false;
        }

        s.ready = true;
        return true;
    }

    void Shutdown()
    {
        if (!s.ready) return;
        DestroyBuffer(s.staging);
        const loader::Device& d = *s.dapi;
        uint32_t live = 0;
        for (uint32_t i = 0; i < kMaxSlots; ++i)
        {
            if (s.slots[i].memory == VK_NULL_HANDLE) continue;
            if (s.slots[i].inUse) ++live;
            DropSlot(d, i);
        }
        if (live) Write(Warn, "vulkan: memory: %u block(s) still held sub-allocations at Shutdown(); their memory is returned anyway", live);
        s = State();
    }

    bool Allocate(const VkMemoryRequirements& req, Where where, Block& out)
    {
        if (!s.ready)
        {
            Write(Warn, "vulkan: memory: Allocate() before Init()");
            return false;
        }
        if (req.size == 0)
        {
            Write(Warn, "vulkan: memory: Allocate() of zero bytes");
            return false;
        }

        const char*    why  = "";
        const uint32_t type = Resolve(req.memoryTypeBits, where, why);
        if (type == kNoType)
        {
            Write(Warn, "vulkan: memory: no %s type in memoryTypeBits 0x%08X", WhereName(where), req.memoryTypeBits);
            return false;
        }

        // Alignment. req.alignment is the offset the resource must be bound at, and it is not a
        // suggestion: a buffer bound at a misaligned offset is invalid usage that most drivers
        // accept silently, after which the GPU reads from the rounded-down address.
        //
        // bufferImageGranularity. Allocate() is not told whether the caller is binding a buffer or
        // an image, so no two sub-allocations may ever share a granularity page: every start is
        // aligned to the granularity and every size padded to it. Then a linear buffer and an
        // optimal-tiling image can never be neighbours within one page, whichever the caller made.
        // On NVIDIA the granularity is 1 and this costs nothing; where it is larger it costs at most
        // one page of slack per allocation.
        //
        // nonCoherentAtomSize is not folded in: FlushHost() rounds its own range to the atom and
        // clamps to the block, so it needs no help from placement.
        const VkDeviceSize align  = req.alignment > s.granularity ? req.alignment : s.granularity;
        const VkDeviceSize padded = RoundUp(req.size, s.granularity);

        uint32_t     slot   = kMaxSlots;
        VkDeviceSize offset = 0;

        if (padded > BlockSize(where))
        {
            // Bigger than a block: its own VkDeviceMemory, sized to the request rather than rounded
            // to a block multiple, because the slack of a dedicated block is never reused.
            slot = NewSlot(type, where, padded, true);
            if (slot == kMaxSlots) return false;
            if (!Carve(s.slots[slot], padded, align, offset))
            {
                DropSlot(*s.dapi, slot);
                return false;
            }
        }
        else
        {
            for (uint32_t i = 0; i < kMaxSlots; ++i)
            {
                Slot& candidate = s.slots[i];
                if (candidate.memory == VK_NULL_HANDLE || candidate.dedicated) continue;
                if (candidate.where != where || candidate.typeIndex != type) continue;
                if (candidate.capacity - candidate.inUse < padded) continue;
                if (Carve(candidate, padded, align, offset))
                {
                    slot = i;
                    break;
                }
            }

            if (slot == kMaxSlots)
            {
                slot = NewSlot(type, where, BlockSize(where), false);
                if (slot == kMaxSlots) return false;
                if (!Carve(s.slots[slot], padded, align, offset))
                {
                    // Unreachable for a fresh block -- padded <= capacity and offset 0 is aligned to
                    // anything -- but cheap to keep honest.
                    DropSlot(*s.dapi, slot);
                    return false;
                }
            }
        }

        const Slot& owner = s.slots[slot];
        out.memory = owner.memory;
        out.offset = offset;
        out.size   = req.size;
        out.mapped = owner.base ? static_cast<char*>(owner.base) + static_cast<size_t>(offset) : nullptr;
        out.owner  = slot;
        out.index  = s.generation[slot];
        return true;
    }

    void Free(const Block& block)
    {
        if (block.memory == VK_NULL_HANDLE || block.size == 0) return;
        if (!s.ready || block.owner >= kMaxSlots) return;

        Slot& slot = s.slots[block.owner];
        if (slot.memory != block.memory || s.generation[block.owner] != block.index)
        {
            // A block from a dedicated slot since released and reused: returning its span would
            // hand someone else's live bytes to the next Allocate().
            Write(Warn, "vulkan: memory: Free() of a stale block ignored");
            return;
        }

        // The same padding Allocate() applied; see the bufferImageGranularity comment there.
        Release(slot, block.offset, RoundUp(block.size, s.granularity));

        // A dedicated block is released as soon as it empties: its size was the one resource's, so
        // it is unlikely to be reused. An ordinary block that empties waits for Tick().
        if (slot.dedicated && slot.inUse == 0)
        {
            DropSlot(*s.dapi, block.owner);
            ++s.dedicatedFreed;
        }
    }

    void Tick()
    {
        if (!s.ready) return;
        ++s.tick;
        // A block that has stayed empty for kReturnAfterTicks presents goes back to the driver,
        // except the kSpareBlocks lowest-indexed empty ones per (type, class). A dedicated block
        // never waits here (Free() releases it the moment it empties).
        for (uint32_t i = 0; i < kMaxSlots; ++i)
        {
            const Slot& slot = s.slots[i];
            if (slot.memory == VK_NULL_HANDLE || slot.dedicated || slot.inUse != 0) continue;
            if (s.tick - slot.emptySince < kReturnAfterTicks) continue;

            uint32_t sparesBefore = 0;
            for (uint32_t k = 0; k < i; ++k)
            {
                const Slot& o = s.slots[k];
                if (o.memory != VK_NULL_HANDLE && !o.dedicated && o.inUse == 0 && o.typeIndex == slot.typeIndex
                    && o.where == slot.where)
                    ++sparesBefore;
            }
            if (sparesBefore < kSpareBlocks) continue;

            const VkDeviceSize bytes = slot.capacity;
            DropSlot(*s.dapi, i);
            ++s.returnedBlocks;
            s.returnedBytes += bytes;
        }
    }

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Where where, Buffer& out)
    {
        if (!s.ready)
        {
            Write(Warn, "vulkan: memory: CreateBuffer() before Init()");
            return false;
        }
        if (size == 0)
        {
            Write(Warn, "vulkan: memory: CreateBuffer() of zero bytes");
            return false;
        }

        const loader::Device& d = *s.dapi;

        VkBufferCreateInfo info = {};
        info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size        = size;
        info.usage       = usage;
        // EXCLUSIVE: every user is on the graphics family.
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer handle = VK_NULL_HANDLE;
        VkResult r = d.CreateBuffer(s.device, &info, nullptr, &handle);
        if (r != VK_SUCCESS) return Fail("vkCreateBuffer", r);

        VkMemoryRequirements req = {};
        d.GetBufferMemoryRequirements(s.device, handle, &req);

        Block block;
        if (!Allocate(req, where, block))
        {
            d.DestroyBuffer(s.device, handle, nullptr);
            Write(Warn, "vulkan: memory: no memory for a %llu-byte %s buffer", static_cast<unsigned long long>(size),
                  WhereName(where));
            return false;
        }

        r = d.BindBufferMemory(s.device, handle, block.memory, block.offset);
        if (r != VK_SUCCESS)
        {
            Free(block);
            d.DestroyBuffer(s.device, handle, nullptr);
            return Fail("vkBindBufferMemory", r);
        }

        out.handle = handle;
        out.block  = block;
        out.size   = size;
        return true;
    }

    void DestroyBuffer(Buffer& buf)
    {
        if (buf.handle == VK_NULL_HANDLE && buf.block.memory == VK_NULL_HANDLE) return;

        if (buf.handle != VK_NULL_HANDLE)
        {
            // Through the context, and only while vulkan-1.dll is still mapped: a buffer destroyed
            // from a static destructor at process exit must not call into an unloaded loader.
            const context::Handles* h = context::Get();
            if (h && h->dapi && h->dapi->DestroyBuffer && loader::StillMapped())
                h->dapi->DestroyBuffer(h->device, buf.handle, nullptr);
        }

        Free(buf.block);
        buf = Buffer();
    }

    bool Staging(VkDeviceSize size, Buffer*& out)
    {
        if (s.staging.handle != VK_NULL_HANDLE && s.staging.size >= size)
        {
            out = &s.staging;
            return true;
        }
        if (!s.ready)
        {
            Write(Warn, "vulkan: memory: Staging() before Init()");
            return false;
        }

        // Grow geometrically while that stays modest, so a sequence of slightly larger uploads does
        // not regrow on every call; past kStagingLarge, grow to exactly what is asked.
        VkDeviceSize want = RoundUp(size ? size : 1, kMiB);
        if (want < kStagingFloor) want = kStagingFloor;
        const VkDeviceSize doubled = s.staging.size * 2;
        if (doubled > want && doubled <= kStagingLarge) want = doubled;

        Buffer fresh;
        if (!CreateBuffer(want, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Where::HostVisible, fresh))
        {
            // The old buffer, if any, is left as it was: a failed growth does not take away the
            // capacity a smaller upload could still use.
            Write(Warn, "vulkan: memory: staging growth to %llu MiB failed", MiB(want));
            return false;
        }

        const VkDeviceSize was = s.staging.size;
        DestroyBuffer(s.staging);
        s.staging = fresh;

        Write(want > kStagingLarge ? Warn : Debug, "vulkan: memory: staging buffer %llu -> %llu MiB (persistently mapped)",
              MiB(was), MiB(want));
        out = &s.staging;
        return true;
    }

    bool FlushHost(const Block& block)
    {
        if (block.memory == VK_NULL_HANDLE || !s.ready || block.owner >= kMaxSlots) return true;

        const Slot& slot = s.slots[block.owner];
        if (slot.memory != block.memory || !slot.base) return true;
        if (slot.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return true;

        // The range rules for a non-coherent flush: offset a multiple of nonCoherentAtomSize, and
        // size either a multiple of it or reaching exactly the end of the allocation. Round out to
        // the atom, and use VK_WHOLE_SIZE when rounding would pass the end -- a block's capacity
        // need not itself be a multiple of the atom.
        const VkDeviceSize start = RoundDown(block.offset, s.atom);
        const VkDeviceSize end   = RoundUp(block.offset + block.size, s.atom);

        VkMappedMemoryRange range = {};
        range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = slot.memory;
        range.offset = start;
        range.size   = end >= slot.capacity ? VK_WHOLE_SIZE : end - start;

        const VkResult r = s.dapi->FlushMappedMemoryRanges(s.device, 1, &range);
        if (r != VK_SUCCESS) return Fail("vkFlushMappedMemoryRanges", r);
        return true;
    }

    void Stats(uint64_t& vkAllocateMemoryCalls, uint64_t& bytesReserved, uint64_t& bytesInUse)
    {
        vkAllocateMemoryCalls = s.allocateCalls;
        bytesReserved         = s.reserved;
        bytesInUse            = s.inUse;
    }

    void GetDetail(Detail& out)
    {
        out = Detail();
        out.reservedPeak   = s.reservedPeak;
        out.mapped         = s.mapped;
        out.mappedPeak     = s.mappedPeak;
        out.returnedBlocks = s.returnedBlocks;
        out.returnedBytes  = s.returnedBytes;
        out.dedicatedFreed = s.dedicatedFreed;
        out.spanGrowths    = s.spanGrowths;
        out.spanPeak       = s.spanPeak;
        for (const Slot& slot : s.slots)
        {
            if (slot.memory == VK_NULL_HANDLE) continue;
            ++out.blocks;
            if (slot.dedicated) ++out.dedicatedBlocks;
            else if (slot.inUse == 0) ++out.emptyBlocks;
            if (slot.where == Where::DeviceLocal) out.deviceLocal += slot.capacity;
            if (slot.typeIndex < s.props.memoryTypeCount)
            {
                const uint32_t heap = s.props.memoryTypes[slot.typeIndex].heapIndex;
                if (heap < VK_MAX_MEMORY_HEAPS) out.heapReserved[heap] += slot.capacity;
            }
        }
    }
}
