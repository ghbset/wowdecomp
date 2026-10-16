// The engine's texture formats as Vulkan sees them: the format table and the RGBA8 decoder.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/TexFormat.hpp"

#include "Engine/Source/Gx/Types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
    using gxvk::vk::texformat::Info;

    // One row per EGxTexFormat, in enum order, each worked out from the D3DFORMAT the engine's
    // s_GxTexFmtToD3dFmt maps it to. D3D9 names a format most-significant-first within its word
    // (D3DFMT_A8R8G8B8 is the dword 0xAARRGGBB, bytes B, G, R, A in memory); Vulkan's unpacked
    // formats name components in memory order, its _PACK16 formats most-significant-first like D3D.
    //
    // BC1/2/3 are legal only when the device was created with textureCompressionBC; `needsDecode`
    // is NOT set for them because that is a property of the device, not the format: the caller
    // checks the feature and decodes itself.
    //
    // Aggregate order: format, blockWidth, blockHeight, blockBytes, compressed, needsDecode,
    // uploadable, name.
    constexpr Info kTable[] =
    {
        // 0 Unknown: D3DFMT_UNKNOWN.
        { VK_FORMAT_UNDEFINED, 1, 1, 0, false, false, false, "Unknown" },

        // 1 Abgr8888: D3DFMT_A8B8G8R8, bytes R, G, B, A: Vulkan's R8G8B8A8. What every BLP decodes to.
        { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, false, false, true, "Abgr8888" },

        // 2 Argb8888: D3DFMT_A8R8G8B8, bytes B, G, R, A: Vulkan's B8G8R8A8.
        { VK_FORMAT_B8G8R8A8_UNORM, 1, 1, 4, false, false, true, "Argb8888" },

        // 3 Argb4444: D3DFMT_A4R4G4B4, A in bits 15..12. Both 1.0 core 4444 formats put A in bits
        //   3..0; A4R4G4B4_UNORM_PACK16 is VK_EXT_4444_formats (1.3 core), so this row decodes.
        { VK_FORMAT_UNDEFINED, 1, 1, 2, false, true, true, "Argb4444" },

        // 4 Argb1555: D3DFMT_A1R5G5B5 = A1R5G5B5_UNORM_PACK16 bit for bit.
        { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 1, 1, 2, false, false, true, "Argb1555" },

        // 5 Rgb565: D3DFMT_R5G6B5 = R5G6B5_UNORM_PACK16 bit for bit.
        { VK_FORMAT_R5G6B5_UNORM_PACK16, 1, 1, 2, false, false, true, "Rgb565" },

        // 6 Dxt1: 8-byte 4x4 blocks. BC1_RGBA rather than BC1_RGB: D3D9's DXT1 has the colour-3-is-
        //   transparent rule in its c0 <= c1 mode and the engine's 1-bit-alpha BLPs rely on it.
        { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8, true, false, true, "Dxt1" },

        // 7 Dxt3: 16-byte blocks, explicit 4-bit alpha.
        { VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16, true, false, true, "Dxt3" },

        // 8 Dxt5: 16-byte blocks, interpolated alpha.
        { VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16, true, false, true, "Dxt5" },

        // 9 Uv88: D3DFMT_V8U8, bytes U then V, both signed: R8G8_SNORM.
        { VK_FORMAT_R8G8_SNORM, 1, 1, 2, false, false, true, "Uv88" },

        // 10 Gr1616F: D3DFMT_G16R16F, the R half then the G half in memory: R16G16_SFLOAT.
        { VK_FORMAT_R16G16_SFLOAT, 1, 1, 4, false, false, true, "Gr1616F" },

        // 11 R32F: one IEEE single.
        { VK_FORMAT_R32_SFLOAT, 1, 1, 4, false, false, true, "R32F" },

        // 12 D24X8: depth in bits 23..0: X8_D24_UNORM_PACK32. Only ever rendered into, never uploaded.
        { VK_FORMAT_X8_D24_UNORM_PACK32, 1, 1, 4, false, false, false, "D24X8" },
    };

    constexpr uint32_t kRows = static_cast<uint32_t>(sizeof kTable / sizeof kTable[0]);
    static_assert(kRows == GxTexFormats_Last, "one row per EGxTexFormat, in enum order");

    constexpr bool SameName(const char* a, const char* b)
    {
        while (*a && *a == *b) { ++a; ++b; }
        return *a == *b;
    }

    // The enum and the table are maintained apart; pin rows to their enumerators so an inserted row
    // cannot silently shift the formats after it.
    static_assert(SameName(kTable[GxTex_Unknown].name, "Unknown"), "row 0");
    static_assert(SameName(kTable[GxTex_Abgr8888].name, "Abgr8888"), "row 1");
    static_assert(SameName(kTable[GxTex_Argb4444].name, "Argb4444"), "row 3");
    static_assert(SameName(kTable[GxTex_Dxt1].name, "Dxt1"), "row 6");
    static_assert(SameName(kTable[GxTex_Dxt5].name, "Dxt5"), "row 8");
    static_assert(SameName(kTable[GxTex_Uv88].name, "Uv88"), "row 9");
    static_assert(SameName(kTable[GxTex_D24X8].name, "D24X8"), "row 12");
    static_assert(kTable[GxTex_Dxt1].blockBytes == 8 && kTable[GxTex_Dxt3].blockBytes == 16 && kTable[GxTex_Dxt5].blockBytes == 16, "DXT block sizes");
    static_assert(!kTable[GxTex_Unknown].uploadable && !kTable[GxTex_D24X8].uploadable, "Unknown and D24X8 are never uploaded");

    /// ceil(n / 4), the block count along one axis.
    constexpr uint32_t Blocks4(uint32_t n) { return (n + 3) / 4; }

    /// An n-bit UNORM value widened to 8 bits by bit replication: 0 maps to 0 and all-ones to 255
    /// exactly, as hardware does; a plain shift would leave every white texel a touch grey.
    constexpr uint8_t Expand5(uint32_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
    constexpr uint8_t Expand6(uint32_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }
    constexpr uint8_t Expand4(uint32_t v) { return static_cast<uint8_t>((v << 4) | v); }

    /// A little-endian word read byte-wise: a block row at an odd pitch is not 2-aligned.
    inline uint32_t Read16(const uint8_t* p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
    }

    inline uint32_t Read32(const uint8_t* p)
    {
        return Read16(p) | (Read16(p + 2) << 16);
    }

    inline void Unpack565(uint32_t c, uint8_t* rgb)
    {
        rgb[0] = Expand5((c >> 11) & 0x1F);
        rgb[1] = Expand6((c >> 5) & 0x3F);
        rgb[2] = Expand5(c & 0x1F);
    }

    /// IEEE binary16 to binary32, bit-exact including subnormals, infinities and NaN.
    inline float HalfToFloat(uint32_t h)
    {
        const uint32_t sign = (h >> 15) & 1;
        uint32_t exp  = (h >> 10) & 0x1F;
        uint32_t mant = h & 0x3FF;
        uint32_t bits;
        if (exp == 0)
        {
            if (mant == 0)
            {
                bits = sign << 31;
            }
            else
            {
                exp = 127 - 15 + 1;
                while ((mant & 0x400) == 0) { mant <<= 1; --exp; }
                mant &= 0x3FF;
                bits = (sign << 31) | (exp << 23) | (mant << 13);
            }
        }
        else if (exp == 31)
        {
            bits = (sign << 31) | 0x7F800000u | (mant << 13);
        }
        else
        {
            bits = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
        float f;
        std::memcpy(&f, &bits, sizeof f);
        return f;
    }

    /// Clamp to [0, 1] and quantise; negatives and NaN go to 0.
    inline uint8_t ToByte(float x)
    {
        if (!(x > 0.0f)) return 0;
        if (x >= 1.0f) return 255;
        return static_cast<uint8_t>(x * 255.0f + 0.5f);
    }

    // --- block decoders: one whole 4x4 block into 16 RGBA quads in raster order; the caller clips ---

    /// The four BC1 colours of a pair of 565 endpoints. Interpolants rounded to nearest, as the
    /// reference decoders. BC2 and BC3 colour blocks are always four-colour whatever the endpoint
    /// order (`fourColourAlways`); the c0 <= c1 rule exists only in BC1, where it carries the 1-bit alpha.
    inline void Bc1Palette(uint32_t c0, uint32_t c1, bool fourColourAlways, uint8_t palette[4][4])
    {
        Unpack565(c0, palette[0]);
        Unpack565(c1, palette[1]);
        palette[0][3] = 255;
        palette[1][3] = 255;

        if (c0 > c1 || fourColourAlways)
        {
            for (int i = 0; i < 3; ++i)
            {
                const uint32_t a = palette[0][i];
                const uint32_t b = palette[1][i];
                palette[2][i] = static_cast<uint8_t>((2 * a + b + 1) / 3);
                palette[3][i] = static_cast<uint8_t>((a + 2 * b + 1) / 3);
            }
            palette[2][3] = 255;
            palette[3][3] = 255;
        }
        else
        {
            for (int i = 0; i < 3; ++i)
            {
                const uint32_t a = palette[0][i];
                const uint32_t b = palette[1][i];
                palette[2][i] = static_cast<uint8_t>((a + b + 1) / 2);
                palette[3][i] = 0;
            }
            palette[2][3] = 255;
            palette[3][3] = 0;   // colour 3 is transparent black: the 1-bit alpha
        }
    }

    /// The 8-byte BC1 colour block at `p`: two 565 endpoints, then 16 two-bit indices, texel 0 in
    /// the low bits of the first index byte.
    inline void DecodeColourBlock(const uint8_t* p, bool fourColourAlways, uint8_t texels[16][4])
    {
        uint8_t palette[4][4];
        Bc1Palette(Read16(p), Read16(p + 2), fourColourAlways, palette);
        const uint32_t indices = Read32(p + 4);
        for (uint32_t i = 0; i < 16; ++i)
        {
            const uint8_t* c = palette[(indices >> (2 * i)) & 3];
            texels[i][0] = c[0];
            texels[i][1] = c[1];
            texels[i][2] = c[2];
            texels[i][3] = c[3];
        }
    }

    void DecodeBc1(const uint8_t* p, uint8_t texels[16][4])
    {
        DecodeColourBlock(p, false, texels);
    }

    /// BC2: 8 bytes of 4-bit alpha, one 16-bit word per row with texel 0 in the low nibble, then
    /// the colour block.
    void DecodeBc2(const uint8_t* p, uint8_t texels[16][4])
    {
        DecodeColourBlock(p + 8, true, texels);
        for (uint32_t y = 0; y < 4; ++y)
        {
            const uint32_t row = Read16(p + 2 * y);
            for (uint32_t x = 0; x < 4; ++x)
                texels[4 * y + x][3] = Expand4((row >> (4 * x)) & 0xF);
        }
    }

    /// BC3: two alpha endpoints, 48 bits of 3-bit indices (texel 0 in the low bits), then the
    /// colour block. a0 > a1 selects eight interpolated alphas, else six plus 0 and 255.
    void DecodeBc3(const uint8_t* p, uint8_t texels[16][4])
    {
        DecodeColourBlock(p + 8, true, texels);

        const uint32_t a0 = p[0];
        const uint32_t a1 = p[1];
        uint8_t alpha[8];
        alpha[0] = static_cast<uint8_t>(a0);
        alpha[1] = static_cast<uint8_t>(a1);
        if (a0 > a1)
        {
            for (uint32_t i = 1; i < 7; ++i)
                alpha[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1 + 3) / 7);
        }
        else
        {
            for (uint32_t i = 1; i < 5; ++i)
                alpha[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1 + 2) / 5);
            alpha[6] = 0;
            alpha[7] = 255;
        }

        // Two 24-bit halves of eight indices each: a 64-bit read would run two bytes past the block.
        for (uint32_t half = 0; half < 2; ++half)
        {
            const uint8_t* q = p + 2 + 3 * half;
            const uint32_t bits = static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) | (static_cast<uint32_t>(q[2]) << 16);
            for (uint32_t i = 0; i < 8; ++i)
                texels[8 * half + i][3] = alpha[(bits >> (3 * i)) & 7];
        }
    }

    /// Copies the texels of one decoded block that fall inside the level.
    inline void WriteBlock(const uint8_t texels[16][4], uint32_t bx, uint32_t by, uint32_t width, uint32_t height, uint8_t* dst)
    {
        for (uint32_t y = 0; y < 4; ++y)
        {
            const uint32_t py = 4 * by + y;
            if (py >= height) break;
            for (uint32_t x = 0; x < 4; ++x)
            {
                const uint32_t px = 4 * bx + x;
                if (px >= width) break;
                std::memcpy(dst + (static_cast<size_t>(py) * width + px) * 4, texels[4 * y + x], 4);
            }
        }
    }

    using BlockFn = void (*)(const uint8_t*, uint8_t[16][4]);

    void DecodeBlocks(BlockFn decode, uint32_t blockBytes, uint32_t width, uint32_t height, const uint8_t* src, uint32_t srcPitch, uint8_t* dst)
    {
        const uint32_t bw = Blocks4(width);
        const uint32_t bh = Blocks4(height);
        for (uint32_t by = 0; by < bh; ++by)
        {
            const uint8_t* row = src + static_cast<size_t>(by) * srcPitch;
            for (uint32_t bx = 0; bx < bw; ++bx)
            {
                uint8_t texels[16][4];
                decode(row + static_cast<size_t>(bx) * blockBytes, texels);
                WriteBlock(texels, bx, by, width, height, dst);
            }
        }
    }

    // --- uncompressed row decoders ------------------------------------------------------------------

    using RowFn = void (*)(const uint8_t* src, uint32_t width, uint8_t* dst);

    void RowAbgr8888(const uint8_t* src, uint32_t width, uint8_t* dst)
    {
        std::memcpy(dst, src, static_cast<size_t>(width) * 4);
    }

    void RowArgb8888(const uint8_t* src, uint32_t width, uint8_t* dst)
    {
        for (uint32_t x = 0; x < width; ++x, src += 4, dst += 4)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    }

    void RowArgb4444(const uint8_t* src, uint32_t width, uint8_t* dst)
    {
        for (uint32_t x = 0; x < width; ++x, src += 2, dst += 4)
        {
            const uint32_t c = Read16(src);
            dst[0] = Expand4((c >> 8) & 0xF);
            dst[1] = Expand4((c >> 4) & 0xF);
            dst[2] = Expand4(c & 0xF);
            dst[3] = Expand4(c >> 12);
        }
    }

    void RowArgb1555(const uint8_t* src, uint32_t width, uint8_t* dst)
    {
        for (uint32_t x = 0; x < width; ++x, src += 2, dst += 4)
        {
            const uint32_t c = Read16(src);
            dst[0] = Expand5((c >> 10) & 0x1F);
            dst[1] = Expand5((c >> 5) & 0x1F);
            dst[2] = Expand5(c & 0x1F);
            dst[3] = static_cast<uint8_t>((c & 0x8000) ? 255 : 0);
        }
    }

    void RowRgb565(const uint8_t* src, uint32_t width, uint8_t* dst)
    {
        for (uint32_t x = 0; x < width; ++x, src += 2, dst += 4)
        {
            Unpack565(Read16(src), dst);
            dst[3] = 255;
        }
    }

    /// Signed U, V biased into the unsigned range, a flat normal in B.
    void RowUv88(const uint8_t* src, uint32_t width, uint8_t* dst)
    {
        for (uint32_t x = 0; x < width; ++x, src += 2, dst += 4)
        {
            dst[0] = static_cast<uint8_t>(static_cast<int8_t>(src[0]) + 128);
            dst[1] = static_cast<uint8_t>(static_cast<int8_t>(src[1]) + 128);
            dst[2] = 128;
            dst[3] = 255;
        }
    }

    void RowGr1616F(const uint8_t* src, uint32_t width, uint8_t* dst)
    {
        for (uint32_t x = 0; x < width; ++x, src += 4, dst += 4)
        {
            dst[0] = ToByte(HalfToFloat(Read16(src)));
            dst[1] = ToByte(HalfToFloat(Read16(src + 2)));
            dst[2] = 0;
            dst[3] = 255;
        }
    }

    /// A single channel shown as grey.
    void RowR32F(const uint8_t* src, uint32_t width, uint8_t* dst)
    {
        for (uint32_t x = 0; x < width; ++x, src += 4, dst += 4)
        {
            const uint32_t bits = Read32(src);
            float f;
            std::memcpy(&f, &bits, sizeof f);
            const uint8_t v = ToByte(f);
            dst[0] = v;
            dst[1] = v;
            dst[2] = v;
            dst[3] = 255;
        }
    }

    RowFn RowDecoder(uint32_t gxFormat)
    {
        switch (gxFormat)
        {
            case GxTex_Abgr8888: return RowAbgr8888;
            case GxTex_Argb8888: return RowArgb8888;
            case GxTex_Argb4444: return RowArgb4444;
            case GxTex_Argb1555: return RowArgb1555;
            case GxTex_Rgb565:   return RowRgb565;
            case GxTex_Uv88:     return RowUv88;
            case GxTex_Gr1616F:  return RowGr1616F;
            case GxTex_R32F:     return RowR32F;
            default:             return nullptr;
        }
    }

    BlockFn BlockDecoder(uint32_t gxFormat)
    {
        switch (gxFormat)
        {
            case GxTex_Dxt1: return DecodeBc1;
            case GxTex_Dxt3: return DecodeBc2;
            case GxTex_Dxt5: return DecodeBc3;
            default:         return nullptr;
        }
    }
}

namespace gxvk::vk::texformat
{
    const Info& Of(uint32_t gxFormat)
    {
        return kTable[gxFormat < kRows ? gxFormat : static_cast<uint32_t>(GxTex_Unknown)];
    }

    uint32_t RowBytes(uint32_t gxFormat, uint32_t width)
    {
        const Info& f = Of(gxFormat);
        if (!f.uploadable || width == 0) return 0;
        return (f.compressed ? Blocks4(width) : width) * f.blockBytes;
    }

    uint32_t RowCount(uint32_t gxFormat, uint32_t height)
    {
        const Info& f = Of(gxFormat);
        if (!f.uploadable || height == 0) return 0;
        return f.compressed ? Blocks4(height) : height;
    }

    uint64_t LevelBytes(uint32_t gxFormat, uint32_t width, uint32_t height)
    {
        return static_cast<uint64_t>(RowBytes(gxFormat, width)) * RowCount(gxFormat, height);
    }

    bool DecodeToRgba8(uint32_t gxFormat, uint32_t width, uint32_t height, const void* src, uint32_t srcPitch, uint8_t* dst)
    {
        if (!src || !dst || width == 0 || height == 0) return false;
        const Info& f = Of(gxFormat);
        if (!f.uploadable) return false;
        // A pitch shorter than a row would read the next row's bytes as this one's: the usual way
        // to get one is a pitch of 0 from a callback that did not fill it, and a decode that
        // "works" on that hides the bug.
        if (srcPitch < RowBytes(gxFormat, width)) return false;

        const auto* bytes = static_cast<const uint8_t*>(src);

        if (f.compressed)
        {
            const BlockFn decode = BlockDecoder(gxFormat);
            if (!decode) return false;
            DecodeBlocks(decode, f.blockBytes, width, height, bytes, srcPitch, dst);
            return true;
        }

        const RowFn row = RowDecoder(gxFormat);
        if (!row) return false;
        for (uint32_t y = 0; y < height; ++y)
            row(bytes + static_cast<size_t>(y) * srcPitch, width, dst + static_cast<size_t>(y) * width * 4);
        return true;
    }
}
