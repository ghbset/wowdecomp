// MD5 (RFC 1321) of a byte range, as lowercase hex.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Md5.hpp"

#include <cstring>

namespace
{
    constexpr uint32_t kS[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };

    constexpr uint32_t kK[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };

    inline uint32_t Rotl(uint32_t x, uint32_t c) { return (x << c) | (x >> (32 - c)); }

    inline uint32_t Load32(const uint8_t* p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    /// One 64-byte block into the running state.
    void Block(uint32_t state[4], const uint8_t block[64])
    {
        uint32_t m[16];
        for (uint32_t i = 0; i < 16; ++i) m[i] = Load32(block + 4 * i);

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        for (uint32_t i = 0; i < 64; ++i)
        {
            uint32_t f, g;
            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
            else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
            const uint32_t t = d;
            d = c;
            c = b;
            b = b + Rotl(a + f + kK[i] + m[g], kS[i]);
            a = t;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }
}

namespace gxvk::md5
{
    void Hex(const void* data, size_t size, char out[33])
    {
        uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t remaining = size;
        while (remaining >= 64)
        {
            Block(state, p);
            p += 64;
            remaining -= 64;
        }

        // The tail: the remaining bytes, 0x80, zeros, the bit length as 64 bits little-endian.
        uint8_t tail[128];
        std::memset(tail, 0, sizeof tail);
        if (remaining) std::memcpy(tail, p, remaining);
        tail[remaining] = 0x80;
        const size_t   tailBlocks = remaining + 1 + 8 <= 64 ? 1 : 2;
        const uint64_t bits       = static_cast<uint64_t>(size) * 8;
        for (uint32_t i = 0; i < 8; ++i) tail[tailBlocks * 64 - 8 + i] = static_cast<uint8_t>(bits >> (8 * i));
        Block(state, tail);
        if (tailBlocks == 2) Block(state, tail + 64);

        static const char kDigits[] = "0123456789abcdef";
        for (uint32_t i = 0; i < 4; ++i)
        {
            for (uint32_t j = 0; j < 4; ++j)
            {
                const uint8_t byte = static_cast<uint8_t>(state[i] >> (8 * j));
                out[8 * i + 2 * j]     = kDigits[byte >> 4];
                out[8 * i + 2 * j + 1] = kDigits[byte & 15];
            }
        }
        out[32] = '\0';
    }
}
