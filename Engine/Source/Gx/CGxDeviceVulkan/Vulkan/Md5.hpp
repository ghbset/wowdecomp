// MD5 of a byte range, as the 32 lowercase hex characters the shader manifest keys its records by.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gxvk::md5
{
    /**
     * The MD5 digest of `size` bytes at `data`, as lowercase hex.
     *
     * @param const void* data : the bytes (null allowed when size is 0)
     * @param size_t size : how many
     * @param char out[33] : receives 32 hex characters and a NUL
     */
    void Hex(const void* data, size_t size, char out[33]);
}
