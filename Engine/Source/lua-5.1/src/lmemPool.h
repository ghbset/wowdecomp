#ifndef lmemPool_h
#define lmemPool_h

#include <cstddef>
#include <cstdint>
#include <vector>
#include "lua.h"
#include "Engine/Source/Storm/H/stpl.h"

class MemChunk {
    public:
    /* 0000 */ void* m_memblocks;
    /* 0004 */ void* m_free;
    /* 0008 */ uint32_t m_size;
    /* 000C */ uint32_t m_blockSize;
    /* 0010 */ uint32_t m_numBlocks;
    // CUSTOM (Badgermilk0 port); the pool's list of chunks that have a free block
    MemChunk* m_nextAvailable;
    uint8_t m_available;

    MemChunk(uint32_t blockSize, uint32_t size);
    ~MemChunk();
};

class MemPool {
    public:
    /* 0000 */ TSFixedArray<MemChunk*> m_chunks;
    /* 000C */ uint32_t m_chunkSize;
    /* 0010 */ uint32_t m_blockSize;
    /* 0014 */ uint32_t m_numBlocks;
    // CUSTOM (Badgermilk0 port). The stock pool scanned every chunk on each allocation (for one with a free block) and
    // on each free (for the owner), so Lua allocation and garbage collection slowed down with the Lua heap. Chunks
    // with a free block are kept in a list, and the chunks sorted by address for a binary search on free.
    MemChunk* m_available;
    std::vector<MemChunk*> m_byAddress;

    MemPool();
    ~MemPool();
    void* MemAlloc();
    int32_t MemFree(void* ptr);
};

LUAI_FUNC void* luaM_initPool();
LUAI_FUNC void luaM_freePool(void* ud);
LUAI_FUNC void* luaM_reallocPool(void* ud, void* ptr, size_t osize, size_t nsize);

#endif
