#include <algorithm>
#include <cstring>
#include "lmemPool.h"
#include "Engine/Source/Storm/H/storm.h"

// STATIC: 0x00A47FBC
static const uint32_t s_blocksizes[] = { 16, 24, 32, 40, 64, 80, 128, 160, 256 };

// VERIFY: a5f551b2 manual
// OFFSET: 0x00855570
MemChunk::MemChunk(uint32_t blockSize, uint32_t size) {
    this->m_blockSize = blockSize;
    this->m_numBlocks = size / blockSize;
    this->m_size = blockSize * this->m_numBlocks;
    this->m_memblocks = SMemAlloc(this->m_size, __FILE__, __LINE__, 0);
    // CUSTOM (Badgermilk0 port)
    this->m_nextAvailable = nullptr;
    this->m_available = 0;

    char* block = static_cast<char*>(this->m_memblocks);
    this->m_free = block;

    while (block < static_cast<char*>(this->m_memblocks) + this->m_size - this->m_blockSize) {
        *reinterpret_cast<void**>(block) = block + this->m_blockSize;
        block += this->m_blockSize;
    }

    *reinterpret_cast<void**>(block) = nullptr;
}

// INLINED
MemChunk::~MemChunk() {
    SMemFree(this->m_memblocks, __FILE__, __LINE__, 0);
}

// INLINED
MemPool::MemPool() {
    // CUSTOM (Badgermilk0 port)
    this->m_available = nullptr;
}

// VERIFY: 403cd95b auto
// OFFSET: 0x00855670
int32_t MemPool::MemFree(void* ptr) {
    // CUSTOM (Badgermilk0 port); binary search for the chunk holding ptr instead of scanning all of them
    auto it = std::upper_bound(this->m_byAddress.begin(), this->m_byAddress.end(), ptr, [](void* p, const MemChunk* chunk) {
        return static_cast<char*>(p) < static_cast<char*>(chunk->m_memblocks);
    });

    if (it == this->m_byAddress.begin()) {
        return 0;
    }

    MemChunk* chunk = *(it - 1);

    if (ptr >= chunk->m_memblocks && ptr < static_cast<char*>(chunk->m_memblocks) + chunk->m_size) {
        *static_cast<void**>(ptr) = chunk->m_free;
        chunk->m_numBlocks++;
        chunk->m_free = ptr;

        if (!chunk->m_available) {
            chunk->m_available = 1;
            chunk->m_nextAvailable = this->m_available;
            this->m_available = chunk;
        }

        return 1;
    }

    return 0;
}

// VERIFY: d5bd1438 manual
// OFFSET: 0x008556E0
MemPool::~MemPool() {
    for (uint32_t i = this->m_chunks.Count(); i; i--) {
        delete this->m_chunks[i - 1];
    }
}

// VERIFY: 2bca1af4 manual
// OFFSET: 0x00855740
void* luaM_initPool() {
    MemPool** pools = new (__FILE__, __LINE__) MemPool*[sizeof(s_blocksizes) / sizeof(s_blocksizes[0])];

    for (uint32_t i = 0; i < sizeof(s_blocksizes) / sizeof(s_blocksizes[0]); i++) {
        pools[i] = new (__FILE__, __LINE__) MemPool();
        pools[i]->m_blockSize = s_blocksizes[i];
        pools[i]->m_numBlocks = (s_blocksizes[i] * 1024) / s_blocksizes[i];
        pools[i]->m_chunkSize = s_blocksizes[i] * pools[i]->m_numBlocks;
    }

    return pools;
}

// VERIFY: 2d194715 manual
// OFFSET: 0x008557D0
void luaM_freePool(void* ud) {
    MemPool** pools = static_cast<MemPool**>(ud);

    if (!pools) {
        return;
    }

    for (uint32_t i = 0; i < sizeof(s_blocksizes) / sizeof(s_blocksizes[0]); i++) {
        delete pools[i];
    }

    SMemFree(pools, __FILE__, __LINE__, 0);
}

// VERIFY: 48e99a2f manual
// OFFSET: 0x00855820
void* MemPool::MemAlloc() {
    // CUSTOM (Badgermilk0 port); the first chunk of the free-block list instead of a scan over all chunks
    MemChunk* chunk = this->m_available;

    if (!chunk) {
        uint32_t count = this->m_chunks.Count();
        this->m_chunks.SetCount(count + 1);
        this->m_chunks[count] = new (__FILE__, __LINE__) MemChunk(this->m_blockSize, this->m_chunkSize);

        chunk = this->m_chunks[count];

        auto it = std::upper_bound(this->m_byAddress.begin(), this->m_byAddress.end(), chunk, [](const MemChunk* a, const MemChunk* b) {
            return static_cast<char*>(a->m_memblocks) < static_cast<char*>(b->m_memblocks);
        });

        this->m_byAddress.insert(it, chunk);

        if (!chunk->m_free) {
            return nullptr;
        }

        chunk->m_available = 1;
        chunk->m_nextAvailable = nullptr;
        this->m_available = chunk;
    }

    void* block = chunk->m_free;
    chunk->m_numBlocks--;
    chunk->m_free = *static_cast<void**>(block);

    if (!chunk->m_free) {
        this->m_available = chunk->m_nextAvailable;
        chunk->m_nextAvailable = nullptr;
        chunk->m_available = 0;
    }

    return block;
}

// VERIFY: f201d154 manual
// OFFSET: 0x008558E0
void* luaM_reallocPool(void* ud, void* ptr, size_t osize, size_t nsize) {
    MemPool** pools = static_cast<MemPool**>(ud);
    int32_t oldpool = -1;
    int32_t newpool = -1;

    if (ptr) {
        for (int32_t i = 0; i < static_cast<int32_t>(sizeof(s_blocksizes) / sizeof(s_blocksizes[0])); i++) {
            if (osize <= s_blocksizes[i]) {
                oldpool = i;
                break;
            }
        }
    }

    if (!nsize) {
        if (ptr) {
            if (oldpool >= 0) {
                pools[oldpool]->MemFree(ptr);
                return nullptr;
            }

            SMemFree(ptr, __FILE__, __LINE__, 0);
        }

        return nullptr;
    }

    for (int32_t i = 0; i < static_cast<int32_t>(sizeof(s_blocksizes) / sizeof(s_blocksizes[0])); i++) {
        if (nsize <= s_blocksizes[i]) {
            newpool = i;
            break;
        }
    }

    if (oldpool == newpool) {
        if (oldpool < 0) {
            return SMemReAlloc(ptr, nsize, __FILE__, __LINE__, 0);
        }

        return ptr;
    }

    void* newptr = newpool < 0 ? SMemAlloc(nsize, __FILE__, __LINE__, 0) : pools[newpool]->MemAlloc();

    if (ptr) {
        memcpy(newptr, ptr, osize < nsize ? osize : nsize);

        if (oldpool >= 0) {
            pools[oldpool]->MemFree(ptr);
        } else {
            SMemFree(ptr, __FILE__, __LINE__, 0);
        }
    }

    return newptr;
}
