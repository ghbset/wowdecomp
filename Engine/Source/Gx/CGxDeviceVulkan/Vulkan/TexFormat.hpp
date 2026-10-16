// The engine's texture formats as Vulkan sees them: the VkFormat each EGxTexFormat is, the size of a
// level, and a decoder to RGBA8 for the ones Vulkan 1.0 core has no byte-identical format for.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

// Pure: no Vulkan call, no device, no allocation, no logging. Everything here is a function of its
// arguments and a constant table. The engine side of each row is CGxDeviceD3d::s_GxTexFmtToD3dFmt,
// the D3DFORMAT the D3D backend stores an EGxTexFormat in; each row names the VkFormat with the
// same byte layout, or says there is none in 1.0 core (`needsDecode`).
//
// Every colour format is UNORM, never sRGB: D3D9 samples all of these as UNORM and the engine does
// its own gamma (the device's gamma ramp), so an sRGB view would apply a second curve on top.
namespace gxvk::vk::texformat
{
    /// What one engine format translates to.
    struct Info
    {
        VkFormat format;        ///< VK_FORMAT_UNDEFINED when there is no 1.0 core equivalent
        uint32_t blockWidth;    ///< 1 for uncompressed, 4 for DXT
        uint32_t blockHeight;   ///< 1 or 4
        uint32_t blockBytes;    ///< bytes per texel for uncompressed, per 4x4 block for DXT
        bool     compressed;    ///< BC1/2/3
        bool     needsDecode;   ///< no usable VkFormat: the caller decodes to RGBA8 first
        bool     uploadable;    ///< false for Unknown and D24X8
        const char* name;       ///< "Abgr8888", "Dxt1", ...
    };

    /**
     * The table row for an engine format: a reference into a constant table, so a caller can keep
     * the pointer for the life of a texture.
     *
     * @param uint32 gxFormat : an EGxTexFormat value
     * @return const Info& info : out-of-range values map to the Unknown row
     */
    const Info& Of(uint32_t gxFormat);

    /**
     * Bytes per row of tightly packed data: for DXT a row of blocks, ceil(w/4) * blockBytes.
     *
     * @param uint32 gxFormat : an EGxTexFormat value
     * @param uint32 width : texels
     * @return uint32 bytes : 0 for a non-uploadable format or a zero width
     */
    uint32_t RowBytes(uint32_t gxFormat, uint32_t width);

    /**
     * Rows of data a level occupies: height for uncompressed, ceil(h/4) for DXT.
     *
     * @param uint32 gxFormat : an EGxTexFormat value
     * @param uint32 height : texels
     * @return uint32 rows : 0 for a non-uploadable format or a zero height
     */
    uint32_t RowCount(uint32_t gxFormat, uint32_t height);

    /**
     * Size of one mip level's tightly packed data. 64-bit: the engine's largest textures times 16
     * bytes a texel do not fit in 32 bits.
     *
     * @return uint64 bytes : 0 for a non-uploadable format or an empty extent
     */
    uint64_t LevelBytes(uint32_t gxFormat, uint32_t width, uint32_t height);

    /**
     * Decodes one level of any uploadable colour format to tightly packed R8G8B8A8_UNORM (memory
     * order R, G, B, A). BC1 keeps the colour-3-is-transparent rule, BC2 and BC3 their alpha
     * blocks; Argb4444 and Argb1555 expand their channels by bit replication; V8U8 is biased into
     * the unsigned range (sign lost); the float formats are clamped to [0, 1] and quantised. The
     * two last are display approximations: a backend that must sample them exactly uploads them
     * in their own VkFormat and never comes here.
     *
     * No heap allocation: the DXT paths decode one block into a stack array and copy out the texels
     * that fall inside the level, which also handles a width or height that is not a multiple of 4.
     *
     * @param uint32 gxFormat : an EGxTexFormat value
     * @param uint32 width, height : texels, non-zero
     * @param const void* src : the level data, `srcPitch` bytes per row (per block row for DXT)
     * @param uint32 srcPitch : bytes from one row (block row) to the next; at least RowBytes()
     * @param uint8* dst : width * height * 4 bytes
     * @return bool ok : false for Unknown, D24X8, a null pointer, an empty extent or a pitch shorter
     *         than a row; `dst` is then untouched
     */
    bool DecodeToRgba8(uint32_t gxFormat, uint32_t width, uint32_t height, const void* src, uint32_t srcPitch, uint8_t* dst);
}
