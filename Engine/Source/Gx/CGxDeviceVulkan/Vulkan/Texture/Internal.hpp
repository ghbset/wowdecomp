// The textures' shared internals: the owned-image table, the format descriptions, the state.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"

#include <cstdint>
#include <cstring>
#include <new>

// The textures live in Vulkan/Texture/ over this one header; the public face is Vulkan/Texture.hpp
// (and Texture.cpp holds the recorded fills).
//
//   Registry  the owned-image table, the format descriptions, the layouts and barriers
//   Create    the one create path (2D, mipped, cube, target)
namespace gxvk::vk::texture::detail
{
    namespace context = gxvk::vk::context;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;
    using gxvk::vk::FormatName;
    using gxvk::vk::ResultName;
    using Image = gxvk::vk::texture::Image;

    /// The most images tracked at once: a fixed table, so a full table is a logged refusal rather
    /// than silent growth. A zone holds a few thousand. Heap-allocated at Init().
    constexpr uint32_t kMaxImages = 16384;

    /// The index over the table, twice its size (a load factor of at most one half keeps linear
    /// probing short), a power of two for the mask.
    constexpr uint32_t kHashSlots = 2 * kMaxImages;
    static_assert((kHashSlots & (kHashSlots - 1)) == 0, "the probe masks with kHashSlots - 1");

    /// A VkImage handle mixed down to a slot: the finaliser of MurmurHash3's 64-bit variant.
    inline uint32_t HashOf(VkImage handle)
    {
        uint64_t v = reinterpret_cast<uint64_t>(handle);
        v ^= v >> 33;
        v *= 0xFF51AFD7ED558CCDull;
        v ^= v >> 33;
        return static_cast<uint32_t>(v) & (kHashSlots - 1);
    }

    /// One image this module created and has not yet destroyed.
    struct Owned
    {
        VkImage       handle = VK_NULL_HANDLE;
        VkImageView   view   = VK_NULL_HANDLE;
        memory::Block block;
    };

    struct State
    {
        VkDevice                device   = nullptr;
        VkPhysicalDevice        physical = nullptr;
        const loader::Device*   dapi     = nullptr;
        const loader::Instance* iapi     = nullptr;

        uint32_t maxDimension     = 0;     ///< VkPhysicalDeviceLimits::maxImageDimension2D
        uint32_t maxDimensionCube = 0;     ///< VkPhysicalDeviceLimits::maxImageDimensionCube
        bool     transferBits     = false; ///< the driver is 1.1+, so FORMAT_FEATURE_TRANSFER_* are meaningful

        Owned*    owned      = nullptr;    ///< kMaxImages entries, dense
        uint32_t  ownedCount = 0;
        uint32_t  nextId     = 0;          ///< Image::id of the last image made; never reused
        /// An open-addressed index over `owned` (kHashSlots entries, dense index + 1, 0 empty), so
        /// FindOwned() is a probe and not a scan: a zone change destroys thousands of images.
        uint32_t* hash       = nullptr;

        bool ready = false;
        gxvk::vk::texture::Stats stats;
    };
    extern State s;

    /// What this module knows about one VkFormat: the unit a row is made of, and its size. For a
    /// block-compressed format the unit is a 4x4 block and a "row" is a row of blocks.
    struct FormatInfo
    {
        uint32_t    unitBytes  = 0;       ///< bytes per texel, or per 4x4 block; 0 = not accepted
        bool        compressed = false;
        const char* name       = nullptr;
    };

    /// The side of a block, in texels: BC1/2/3 are all 4x4.
    constexpr uint32_t kBlock = 4;

    // Registry.cpp
    void Fail(const char* what, VkResult r);
    FormatInfo Describe(VkFormat f);
    const char* Name(VkFormat f);
    uint32_t LevelSide(uint32_t side, uint32_t level);
    uint32_t MaxLevels(uint32_t width, uint32_t height);
    uint32_t RowCount(const FormatInfo& info, uint32_t height);
    uint64_t RowBytes(const FormatInfo& info, uint32_t width);
    bool HaveEntryPoints(const loader::Device& d, const loader::Instance& i);
    uint32_t FindOwned(VkImage handle);
    void HashInsert(uint32_t index);
    void RemoveOwned(uint32_t i);
    void FreeTables();
    VkImageSubresourceRange WholeColour(uint32_t levels, uint32_t layers);
    void Transition(VkCommandBuffer cmd, VkImage image, uint32_t levels, uint32_t layers, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);

    /// VFOG: a depth (or depth-stencil) format: only CreateDepthCopy() makes images of one.
    inline bool IsDepth(VkFormat f)
    {
        return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT
            || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    // Create.cpp
    bool CreateImage(uint32_t width, uint32_t height, uint32_t levels, bool cube, VkFormat format, Image& out, VkImageUsageFlags extraUsage = 0);
}
