// The allocator's memory types: lookup, resolution, the table line.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory/Internal.hpp"

namespace gxvk::vk::memory::detail
{
    State s;

    VkDeviceSize RoundUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) & ~(a - 1); }
    VkDeviceSize RoundDown(VkDeviceSize v, VkDeviceSize a) { return v & ~(a - 1); }

    unsigned long long MiB(VkDeviceSize v) { return static_cast<unsigned long long>(v / kMiB); }

    const char* WhereName(Where w)
    {
        return w == Where::HostVisible ? "HostVisible" : w == Where::Stream ? "Stream" : "DeviceLocal";
    }

    VkDeviceSize BlockSize(Where w) { return w != Where::DeviceLocal ? kBlockHostVisible : kBlockDeviceLocal; }

    /**
     * Memory property flags as names, for the type table.
     *
     * @param VkMemoryPropertyFlags f : the flags
     * @param char* buf : receives the text
     * @param size_t cap : capacity of buf
     * @return string buf
     */
    const char* PropertyText(VkMemoryPropertyFlags f, char* buf, size_t cap)
    {
        struct Name { VkMemoryPropertyFlags bit; const char* name; };
        static const Name names[] = {
            { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,     "DEVICE_LOCAL" },
            { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,     "HOST_VISIBLE" },
            { VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,    "HOST_COHERENT" },
            { VK_MEMORY_PROPERTY_HOST_CACHED_BIT,      "HOST_CACHED" },
            { VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZILY_ALLOCATED" },
            { VK_MEMORY_PROPERTY_PROTECTED_BIT,        "PROTECTED" },
        };

        buf[0] = '\0';
        size_t used = 0;
        VkMemoryPropertyFlags rest = f;
        for (const Name& n : names)
        {
            if (!(f & n.bit)) continue;
            rest &= ~n.bit;
            const int w = std::snprintf(buf + used, cap - used, "%s%s", used ? "|" : "", n.name);
            if (w > 0) used += static_cast<size_t>(w) < cap - used ? static_cast<size_t>(w) : cap - used - 1;
        }
        // Vendor bits (AMD's DEVICE_COHERENT/UNCACHED, NVIDIA's RDMA) as a number: they do not
        // change any choice made here, but a type the log cannot describe would be a blind spot.
        if (rest && used < cap)
            std::snprintf(buf + used, cap - used, "%s0x%X", used ? "|" : "", static_cast<unsigned>(rest));
        else if (!f)
            std::snprintf(buf, cap, "(none)");
        return buf;
    }

    /**
     * The first memory type, in the adapter's own order, allowed by `bits` and carrying `want`.
     *
     * First in order is not arbitrary: the spec requires the types to be listed so that a type
     * whose flags are a strict subset of another's comes first (among types with equal
     * performance). So a plain DEVICE_LOCAL type precedes a DEVICE_LOCAL|HOST_VISIBLE one (the
     * 256 MiB BAR window on many discrete cards, far too precious to fill with textures), and a
     * plain HOST_VISIBLE|HOST_COHERENT type precedes the same plus DEVICE_LOCAL. First-fit gets
     * the conservative answer for free.
     *
     * @param uint32 bits : VkMemoryRequirements::memoryTypeBits, or all ones for "any resource"
     * @param VkMemoryPropertyFlags want : flags that must all be present
     * @return uint32 type : the index, or kNoType
     */
    uint32_t FindType(uint32_t bits, VkMemoryPropertyFlags want)
    {
        for (uint32_t i = 0; i < s.props.memoryTypeCount; ++i)
        {
            if (!(bits & (1u << i))) continue;
            if ((s.props.memoryTypes[i].propertyFlags & want) == want) return i;
        }
        return kNoType;
    }

    /**
     * Resolves a Where against one resource's allowed types, applying both fallbacks.
     *
     * @param uint32 bits : the allowed types
     * @param Where where : the class
     * @param string& why : receives the reason, for the Init() log
     * @return uint32 type : the index, or kNoType
     */
    uint32_t Resolve(uint32_t bits, Where where, const char*& why)
    {
        if (where == Where::Stream)
        {
            // The BAR / ReBAR type, while what this module holds of its heap stays under half of it
            // (without ReBAR the window is 256 MiB, and the driver and the other processes use it
            // too); then the plain host-visible answer below.
            const VkMemoryPropertyFlags bar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                              | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            const uint32_t t = FindType(bits, bar);
            if (t != kNoType)
            {
                const uint32_t heap = s.props.memoryTypes[t].heapIndex;
                VkDeviceSize   held = 0;
                for (const Slot& slot : s.slots)
                    if (slot.memory != VK_NULL_HANDLE && s.props.memoryTypes[slot.typeIndex].heapIndex == heap)
                        held += slot.capacity;
                if (held + BlockSize(Where::Stream) <= s.props.memoryHeaps[heap].size / 2)
                {
                    why = "first type with DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT (the BAR window / ReBAR)";
                    return t;
                }
            }
            // Fall through: the host-visible answer.
        }
        if (where == Where::DeviceLocal)
        {
            uint32_t t = FindType(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            why = "first type with DEVICE_LOCAL";
            if (t != kNoType) return t;

            // An adapter with no DEVICE_LOCAL type the resource may use is not broken: a software
            // or unified-memory implementation can legitimately describe all its memory as plain
            // host memory. Declining there would turn "slower" into "no backend at all".
            t   = FindType(bits, 0);
            why = "FALLBACK: no DEVICE_LOCAL type allowed, using the first allowed type";
            return t;
        }

        uint32_t t = FindType(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        why = "first type with HOST_VISIBLE|HOST_COHERENT";
        if (t != kNoType) return t;

        // Accepted, but every block on this type is marked by its own flags as needing a flush,
        // and FlushHost() acts on exactly that.
        t   = FindType(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        why = "FALLBACK: no HOST_COHERENT type, using HOST_VISIBLE alone -- writes need FlushHost()";
        return t;
    }

    /**
     * Logs a failed call with its result.
     *
     * @param string what : the call, by name
     * @param VkResult r : what it returned
     * @return bool false, so a failure path can return it directly
     */
    bool Fail(const char* what, VkResult r)
    {
        Write(Warn, "vulkan: memory: %s failed: %s (%d)", what, ResultName(r), static_cast<int>(r));
        return false;
    }

    /**
     * Confirms every entry point this module calls is non-null, once, naming the missing ones.
     *
     * @param loader::Instance& i : the instance table
     * @param loader::Device& d : the device table
     * @return bool ok
     */
    bool HaveEntryPoints(const loader::Instance& i, const loader::Device& d)
    {
        struct Entry { const void* fn; const char* name; };
        const Entry entries[] = {
            { reinterpret_cast<const void*>(i.GetPhysicalDeviceMemoryProperties), "vkGetPhysicalDeviceMemoryProperties" },
            { reinterpret_cast<const void*>(i.GetPhysicalDeviceProperties),       "vkGetPhysicalDeviceProperties" },
            { reinterpret_cast<const void*>(d.DeviceWaitIdle),                    "vkDeviceWaitIdle" },
            { reinterpret_cast<const void*>(d.AllocateMemory),                    "vkAllocateMemory" },
            { reinterpret_cast<const void*>(d.FreeMemory),                        "vkFreeMemory" },
            { reinterpret_cast<const void*>(d.MapMemory),                         "vkMapMemory" },
            { reinterpret_cast<const void*>(d.UnmapMemory),                       "vkUnmapMemory" },
            { reinterpret_cast<const void*>(d.FlushMappedMemoryRanges),           "vkFlushMappedMemoryRanges" },
            { reinterpret_cast<const void*>(d.CreateBuffer),                      "vkCreateBuffer" },
            { reinterpret_cast<const void*>(d.DestroyBuffer),                     "vkDestroyBuffer" },
            { reinterpret_cast<const void*>(d.GetBufferMemoryRequirements),       "vkGetBufferMemoryRequirements" },
            { reinterpret_cast<const void*>(d.BindBufferMemory),                  "vkBindBufferMemory" },
        };

        unsigned missing = 0;
        for (const Entry& e : entries)
        {
            if (e.fn) continue;
            Write(Warn, "vulkan: memory: entry point missing: %s", e.name);
            ++missing;
        }
        if (missing) Write(Warn, "vulkan: memory: %u entry point(s) missing; declining", missing);
        return missing == 0;
    }

    /// Writes the memory-type table and the per-Where resolution. Called once, from Init().
    void LogTable()
    {
        Write(Debug, "vulkan: memory: %u type(s) over %u heap(s); bufferImageGranularity %llu, nonCoherentAtomSize %llu, maxMemoryAllocationCount %u",
              s.props.memoryTypeCount, s.props.memoryHeapCount, static_cast<unsigned long long>(s.granularity),
              static_cast<unsigned long long>(s.atom), s.maxAllocations);

        for (uint32_t h = 0; h < s.props.memoryHeapCount; ++h)
        {
            const VkMemoryHeap& heap = s.props.memoryHeaps[h];
            Write(Debug, "vulkan: memory:   heap %u: %llu MiB%s", h, MiB(heap.size),
                  (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "");
        }

        char text[128];
        for (uint32_t t = 0; t < s.props.memoryTypeCount; ++t)
        {
            const VkMemoryType& type = s.props.memoryTypes[t];
            Write(Debug, "vulkan: memory:   type %2u: heap %u (%llu MiB) %s", t, type.heapIndex,
                  MiB(s.props.memoryHeaps[type.heapIndex].size), PropertyText(type.propertyFlags, text, sizeof text));
        }
    }
}
