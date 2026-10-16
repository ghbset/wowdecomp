// What the bring-up costs in the process's address space, measured rather than estimated.
//
// A Vulkan ICD is a large, eager consumer of address space (an unused driver has been measured at
// about 230 MB). On this x64 client that is no hazard, but the bring-up log reports the figures
// below because "what did vkCreateDevice cost" is a question that keeps coming back, and the walk
// is well under a millisecond and happens four times.
//
//  - committed bytes: pages the process has actually asked the OS to back.
//  - reserved bytes: address space taken, committed or not.
//  - largest free block: the single number that would predict a large-allocation failure.
//
// All three come from a VirtualQuery walk: it needs nothing on the link line and is the same source
// Task Manager's committed figure is derived from. The upper bound comes from GetSystemInfo, which
// is right on both a 64-bit and a 32-bit build of this tree.
#pragma once

#include <windows.h>

#include <cstdint>

namespace gxvk::vk::footprint
{
    /// One walk's result. All sizes in bytes.
    struct Snapshot
    {
        uint64_t reserved    = 0; ///< MEM_RESERVE + MEM_COMMIT region bytes
        uint64_t committed   = 0; ///< MEM_COMMIT region bytes
        uint64_t largestFree = 0; ///< biggest single MEM_FREE run
        uint32_t regions     = 0; ///< regions the walk saw, as a sanity check on the walk itself
    };

    /**
     * Walks the process's own user-mode address space.
     *
     * @return Snapshot snapshot
     */
    inline Snapshot Take()
    {
        SYSTEM_INFO info = {};
        ::GetSystemInfo(&info);

        Snapshot snapshot;
        auto address = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
        const auto limit = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);

        while (address < limit)
        {
            MEMORY_BASIC_INFORMATION region = {};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof region) != sizeof region)
                break;

            // RegionSize of 0 would make this loop forever; it should not happen, but a diagnostic
            // helper must not be able to hang the render thread.
            if (region.RegionSize == 0) break;

            ++snapshot.regions;
            if (region.State == MEM_FREE)
            {
                if (region.RegionSize > snapshot.largestFree) snapshot.largestFree = region.RegionSize;
            }
            else
            {
                snapshot.reserved += region.RegionSize;
                if (region.State == MEM_COMMIT) snapshot.committed += region.RegionSize;
            }

            address += region.RegionSize;
        }
        return snapshot;
    }

    /// Bytes as whole MiB, for a log line that does not need the digits.
    inline unsigned Mib(uint64_t bytes) { return static_cast<unsigned>(bytes / (1024u * 1024u)); }

    /// A signed MiB delta, so a teardown that gives space back reads as a negative number.
    inline int MibDelta(uint64_t after, uint64_t before)
    {
        const int64_t delta = static_cast<int64_t>(after) - static_cast<int64_t>(before);
        return static_cast<int>(delta / (1024 * 1024));
    }
}
