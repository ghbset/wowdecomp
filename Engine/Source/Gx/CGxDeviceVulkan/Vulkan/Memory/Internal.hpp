// The allocator's shared internals: its constants, its slots and spans, its state.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"

#include <cstdio>
#include <cstring>
#include <new>

// The allocator lives in Vulkan/Memory/ over this one header; its public face is Vulkan/Memory.hpp
// (Vulkan/Memory.cpp).
//
//   Types  the memory types: lookup, resolution, the table line
//   Slots  the blocks and their spans: carve, release, grow, drop
namespace gxvk::vk::memory::detail
{
    namespace context = gxvk::vk::context;
    namespace loader  = gxvk::vk::loader;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;
    using gxvk::vk::memory::Block;
    using gxvk::vk::memory::Buffer;
    using gxvk::vk::memory::Where;

    constexpr VkDeviceSize kMiB = 1024ull * 1024ull;

    /// The size of an ordinary device-local block.
    ///
    /// 16 MiB. The allocation-count floor is what sets the lower bound: 4096 allocations of 16 MiB
    /// is 64 GiB, so the count can no longer be the thing that runs out. The upper bound is waste:
    /// a block is held whole from its first sub-allocation, and the last block of a class is on
    /// average half empty, so 64 MiB blocks would strand ~32 MiB of VRAM per memory type for nothing.
    constexpr VkDeviceSize kBlockDeviceLocal = 16 * kMiB;

    /// The size of an ordinary host-visible block.
    ///
    /// 8 MiB, half the device-local figure: every host-visible block is mapped for its whole life
    /// (see NewSlot), and host-visible memory here is staging, per-frame data and the engine's
    /// vertex pools, not resident assets, so a smaller block strands less mapped address space in
    /// its unused half while keeping the allocation count negligible.
    constexpr VkDeviceSize kBlockHostVisible = 8 * kMiB;

    /// How many VkDeviceMemory objects this module will hold at once.
    ///
    /// A fixed table rather than a std::vector: static storage against heap churn. 1024 slots of
    /// 16 MiB is 16 GiB of device-local memory, a quarter of the usual maxMemoryAllocationCount
    /// (4096); an HD texture set filled a 128-slot table on entering the world. The slots themselves
    /// are ~90 KiB of static storage. A full table is a logged failure, not an overwrite.
    constexpr uint32_t kMaxSlots = 1024;

    /// Free spans a block's list holds before its first growth. Spans are coalesced on every Free(),
    /// so a block in steady use sits at one or two; a block of small textures freed in load order can
    /// fragment into hundreds. The list doubles on demand (GrowSpans), so no free loses bytes.
    constexpr uint32_t kFirstSpans = 64;

    /// How many presents a non-dedicated block may stay empty before it is given back to the
    /// driver: about five seconds at 60 fps, long enough that a zone change which frees and refills
    /// the same blocks does not pay a vkFreeMemory / vkAllocateMemory pair per block.
    constexpr uint64_t kReturnAfterTicks = 300;

    /// Empty blocks kept per (memory type, class) however long they stay empty, so a class that
    /// empties and refills often keeps one block warm.
    constexpr uint32_t kSpareBlocks = 1;

    /// The staging buffer's first size. Big enough for a 1024x1024 RGBA8 mip chain in one go.
    constexpr VkDeviceSize kStagingFloor = 4 * kMiB;

    /// Past this, a staging growth is logged as a warning: the staging buffer is permanently
    /// mapped, and 64 MiB of address space held for the rest of the session is a cost worth seeing.
    /// It is a warning and not a refusal -- a caller that needs a bigger upload needs it.
    constexpr VkDeviceSize kStagingLarge = 64 * kMiB;

    constexpr uint32_t kNoType = 0xFFFFFFFFu;

    struct Span
    {
        VkDeviceSize offset;
        VkDeviceSize size;
    };

    /// One VkDeviceMemory and the free spans inside it, kept sorted by offset so a Free() can
    /// coalesce with both neighbours in one look.
    struct Slot
    {
        VkDeviceMemory        memory     = VK_NULL_HANDLE;
        void*                 base       = nullptr; ///< the whole block's mapping, or null
        VkDeviceSize          capacity   = 0;
        VkDeviceSize          inUse      = 0;       ///< padded bytes handed out
        VkMemoryPropertyFlags flags      = 0;       ///< of the block's memory type
        uint32_t              typeIndex  = 0;
        Where                 where      = Where::DeviceLocal;
        bool                  dedicated  = false;
        uint32_t              spanCount  = 0;
        uint32_t              spanCap    = 0;
        Span*                 spans      = nullptr; ///< heap, spanCap entries
        uint64_t              emptySince = 0;       ///< State::tick when inUse last reached 0
    };

    struct State
    {
        bool                  ready  = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;

        VkPhysicalDeviceMemoryProperties props = {};
        VkDeviceSize granularity    = 1; ///< VkPhysicalDeviceLimits::bufferImageGranularity
        VkDeviceSize atom           = 1; ///< VkPhysicalDeviceLimits::nonCoherentAtomSize
        uint32_t     maxAllocations = 0;

        Slot     slots[kMaxSlots];
        /// Bumped every time a slot is emptied: a Block remembers the generation of the slot it
        /// came from, so a Free() of a block that outlived its slot -- a dedicated block released
        /// and its slot reused -- is recognised and ignored instead of returning someone else's
        /// bytes to the free list.
        uint32_t generation[kMaxSlots] = {};

        Buffer staging;

        uint64_t allocateCalls  = 0; ///< since load
        uint64_t reserved       = 0;
        uint64_t inUse          = 0;
        uint64_t mapped         = 0; ///< bytes of `reserved` that are mapped: address space
        uint64_t reservedPeak   = 0;
        uint64_t mappedPeak     = 0;
        uint64_t tick           = 0; ///< Tick() calls (presents)
        uint64_t returnedBlocks = 0;
        uint64_t returnedBytes  = 0;
        uint64_t dedicatedFreed = 0;
        uint64_t spanGrowths    = 0;
        uint32_t spanPeak       = 0;
        bool     spanGrowWarned = false;
    };
    extern State s;

    // Types.cpp
    VkDeviceSize RoundUp(VkDeviceSize v, VkDeviceSize a);
    VkDeviceSize RoundDown(VkDeviceSize v, VkDeviceSize a);
    unsigned long long MiB(VkDeviceSize v);
    const char* WhereName(Where w);
    VkDeviceSize BlockSize(Where w);
    const char* PropertyText(VkMemoryPropertyFlags f, char* buf, size_t cap);
    uint32_t FindType(uint32_t bits, VkMemoryPropertyFlags want);
    uint32_t Resolve(uint32_t bits, Where where, const char*& why);
    bool Fail(const char* what, VkResult r);
    bool HaveEntryPoints(const loader::Instance& i, const loader::Device& d);
    void LogTable();

    // Slots.cpp
    bool GrowSpans(Slot& slot);
    bool Carve(Slot& slot, VkDeviceSize size, VkDeviceSize align, VkDeviceSize& offset);
    void Release(Slot& slot, VkDeviceSize offset, VkDeviceSize size);
    void DropSlot(const loader::Device& d, uint32_t i);
    uint32_t NewSlot(uint32_t type, Where where, VkDeviceSize capacity, bool dedicated);
}
