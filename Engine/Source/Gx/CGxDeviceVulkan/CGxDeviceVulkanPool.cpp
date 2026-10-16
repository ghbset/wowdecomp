// The Vulkan backend's pools and buffers: a CGxPool is one or more VkBuffer chunks owned by a
// record behind CGxPool::m_apiSpecific; a CGxBuf is a range of the pool's current chunk at
// CGxBuf::m_index, which is what the draw binds the stream at. The pool and buf objects, their
// lists, the stream cursor and the locked-buf table stay the engine's: every virtual runs its base
// entry first, as the D3D overrides do, and the lock reproduces D3D's IBufLock (0x0068FB10) step by
// step.
//
// The memory strategy, by CGxPool::m_usage:
//
//   Stream   a RENAMED ring of chunks, each a host-visible mapped VkBuffer of m_size bytes. The lock
//            places the buf as D3D does (NOOVERWRITE while the cursor fits; at the wrap
//            CGxPool::Discard, offset 0) and writes in place; the wrap -- D3D's DISCARD, "new memory,
//            the old stays with the GPU" -- moves the pool to another chunk no unfinished frame named,
//            or to a new one. A draw binds the chunk current at its call, so a draw recorded before a
//            wrap keeps reading the bytes it was given.
//   Dynamic  a host-visible chunk the draws bind, behind a CPU SHADOW the lock hands out: the unlock
//            copies the written range into the chunk. A D3D GPU runs a frame's draws while the frame
//            is still being issued, and this frame runs them all after it, so a lock that overlaps a
//            range a draw of THIS frame bound would show that draw bytes D3D never showed it. The draw
//            records the ranges it bound (INoteBufDrawn); a lock that overlaps one moves the pool to
//            another chunk first, a copy of the shadow (copy on write), so the earlier draw keeps its
//            bytes. Ranges drawn by an earlier frame still in flight are D3D's own NOOVERWRITE case.
//   Static   a host-visible chunk written in place: D3D creates these WRITEONLY and the engine fills
//            the whole buf it locks, once, before it is drawn. A lock while an earlier frame still
//            reads the chunk waits for that frame first (a synchronised lock, as D3D's MANAGED lock);
//            a lock while the OPEN frame reads it cannot wait and writes in place.
//
// All three classes ask memory::Where::Stream: device-local host-visible memory (the BAR window,
// all of VRAM with resizable BAR) while the allocator holds less than half of that heap, else
// plain host-visible memory.
//
// A pool gets its record at its first lock (or at PoolSizeSet): the two stream pools exist before
// the device does, and every other pool arrives sized by PoolCreate. With no context, no draw path
// or no memory, the lock takes D3D's emergency path: the engine's own EmergencyMem block, and the
// unlock poisons the frame (m_intF5C), so the draws that would read the missing bytes are dropped
// until the next present.
#include <cstdint>
#include <cstring>
#include <new>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"

namespace {
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace memory  = gxvk::vk::memory;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Warn;
    using gxvk::log::Error;

    /// Chunks one pool may rename through. Two frames in flight need three at a steady one wrap per
    /// frame; a frame that wraps W times needs about 2 (W + 1). In-world frames wrap the 2.75 MiB
    /// vertex ring several times, so 32 -- and when even those are all still read, the rename waits
    /// for the oldest earlier frame instead of writing in place (FreeChunk).
    constexpr uint32_t kMaxChunks = 32;

    /// Byte ranges of a dynamic pool one frame's draws bound, before the list gives up and calls
    /// the whole pool drawn (every overlapping lock then renames).
    constexpr uint32_t kMaxDrawnRanges = 64;

    /// Outstanding locks per target. The engine's locked-buf table holds one per target, but BufData
    /// runs the lock / unlock pair itself, so a BufData inside an open lock of the same target would
    /// overwrite a single record; the records are a few deep and an unlock finds its own by buf.
    constexpr uint32_t kLockDepth = 4;

    struct Range {
        uint32_t begin;
        uint32_t end;
    };

    struct PoolRec {
        CGxPool*       pool       = nullptr;
        EGxPoolUsage   usage      = GxPoolUsage_Static;
        uint32_t       size       = 0;
        uint32_t       chunkCount = 0;
        uint32_t       current    = 0;
        uint64_t       chunkUsed[kMaxChunks] = {};   ///< serial of the last frame a draw bound the chunk in
        memory::Buffer chunks[kMaxChunks];

        // Dynamic pools: the shadow the lock hands out, and what this frame's draws bound.
        uint8_t*       shadow      = nullptr;   ///< `size` bytes; null when it could not be had (then written in place)
        uint64_t       drawnSerial = 0;
        uint32_t       drawnCount  = 0;
        bool           drawnAll    = false;     ///< the range list overflowed
        Range          drawn[kMaxDrawnRanges];

        ~PoolRec() { delete[] shadow; }
    };

    struct Lock {
        const CGxBuf* buf       = nullptr;
        PoolRec*      rec       = nullptr;
        bool          emergency = false;
        bool          shadowed  = false;   ///< dynamic: the engine writes the shadow; the unlock copies
    };

    struct Stats {
        uint64_t locks[GxPoolUsages_Last];
        uint64_t discards;          ///< stream wraps
        uint64_t renames;           ///< of which moved to another chunk
        uint64_t renameHazards;     ///< wraps with every chunk still read by the open frame: written in place
        uint64_t renameWaits;       ///< wraps that waited for an earlier frame to free a chunk
        uint64_t chunksCreated;
        uint64_t staticWaits;       ///< static locks that waited for an earlier frame
        uint64_t staticHazards;     ///< static locks of a chunk the open frame reads: written in place
        uint64_t dynamicOverlap;    ///< dynamic locks overlapping a range a draw of this frame bound
        uint64_t dynamicRenames;    ///< of which renamed by copy on write
        uint64_t dynamicInPlace;    ///< of which written in place anyway (no chunk could be had)
        uint64_t emergency;         ///< locks answered with EmergencyMem
        uint64_t records;           ///< pool records created
        uint64_t unresolved;        ///< draws whose pool had no buffer
        uint32_t mostChunks;        ///< the most chunks one pool has held
        uint64_t lockOverflow;      ///< locks with every record of their target in use
    };
    Stats g_stats;
    Lock  g_locks[GxPoolTargets_Last][kLockDepth];
    bool  g_loggedFail = false;

    PoolRec*& RecOf(CGxPool* pool) {
        return reinterpret_cast<PoolRec*&>(pool->m_apiSpecific);
    }

    uint32_t TargetOf(const CGxBuf* buf) {
        const uint32_t target = buf->m_pool ? static_cast<uint32_t>(buf->m_pool->m_target) : 0u;
        return target < GxPoolTargets_Last ? target : 0u;
    }

    /// The record for a new lock of `buf`: its own if it is already locked, else a free one.
    Lock& LockFor(uint32_t target, const CGxBuf* buf) {
        Lock* row = g_locks[target];

        for (uint32_t i = 0; i < kLockDepth; i++) {
            if (row[i].buf == buf) {
                return row[i];
            }
        }

        for (uint32_t i = 0; i < kLockDepth; i++) {
            if (!row[i].buf) {
                return row[i];
            }
        }

        g_stats.lockOverflow++;
        return row[0];
    }

    /// The record of an outstanding lock of `buf`, or null.
    Lock* LockOf(uint32_t target, const CGxBuf* buf) {
        Lock* row = g_locks[target];

        for (uint32_t i = 0; i < kLockDepth; i++) {
            if (row[i].buf == buf) {
                return &row[i];
            }
        }

        return nullptr;
    }

    bool CreateChunk(PoolRec& r) {
        if (r.chunkCount == kMaxChunks || r.size == 0) {
            return false;
        }

        memory::Buffer& b = r.chunks[r.chunkCount];

        if (!memory::CreateBuffer(r.size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, memory::Where::Stream, b) || !b.block.mapped) {
            memory::DestroyBuffer(b);

            if (!g_loggedFail) {
                g_loggedFail = true;
                Write(Error, "vulkan: pools: no host-visible buffer of %u bytes for pool \"%s\"; its locks take the emergency path (logged once)", r.size, r.pool->m_name ? r.pool->m_name : "?");
            }

            return false;
        }

        r.chunkUsed[r.chunkCount] = 0;
        r.chunkCount++;
        g_stats.chunksCreated++;

        if (r.chunkCount > g_stats.mostChunks) {
            g_stats.mostChunks = r.chunkCount;
        }

        return true;
    }

    /// Every chunk retired with the frame that last bound it.
    void RetireAll(PoolRec& r) {
        for (uint32_t i = 0; i < r.chunkCount; i++) {
            frame::RetireBuffer(r.chunks[i], r.chunkUsed[i]);
        }

        r.chunkCount = 0;
        r.current = 0;
        r.drawnSerial = 0;
    }

    /// Every chunk destroyed at once; the caller has made the device idle.
    void DestroyAll(PoolRec& r) {
        for (uint32_t i = 0; i < r.chunkCount; i++) {
            memory::DestroyBuffer(r.chunks[i]);
        }

        r.chunkCount = 0;
        r.current = 0;
        r.drawnSerial = 0;
    }

    /// The dynamic pool's shadow, sized to the pool; dropped and remade when the size changes.
    void EnsureShadow(PoolRec& r) {
        if (r.usage != GxPoolUsage_Dynamic || r.size == 0 || r.shadow) {
            return;
        }

        r.shadow = new (std::nothrow) uint8_t[r.size];

        if (r.shadow) {
            memset(r.shadow, 0, r.size);
        }
    }

    void DropShadow(PoolRec& r) {
        delete[] r.shadow;
        r.shadow = nullptr;
    }

    /// The pool's record, created and sized on first use. Null without a usable draw path.
    PoolRec* Ensure(CGxPool* pool, bool ready) {
        if (!pool) {
            return nullptr;
        }

        PoolRec* r = RecOf(pool);

        if (r && r->chunkCount) {
            return r;
        }

        if (!ready) {
            return nullptr;
        }

        if (!r) {
            r = new (std::nothrow) PoolRec();

            if (!r) {
                return nullptr;
            }

            r->pool = pool;
            r->usage = pool->m_usage;

            if (r->usage >= GxPoolUsages_Last) {
                r->usage = GxPoolUsage_Static;
            }

            RecOf(pool) = r;
            g_stats.records++;
        }

        const uint32_t size = static_cast<uint32_t>(pool->m_size);

        if (size != r->size) {
            DropShadow(*r);
        }

        r->size = size;

        if (!CreateChunk(*r)) {
            return nullptr;
        }

        EnsureShadow(*r);
        return r;
    }

    /// A chunk other than the current one that no unfinished frame named, or a new one; failing
    /// both, the chunk the oldest submitted frame named, once that frame is waited for (never the
    /// open frame's, which cannot be waited for). kMaxChunks when none can be had.
    uint32_t FreeChunk(PoolRec& r) {
        const uint64_t done = frame::Completed();

        for (uint32_t i = 0; i < r.chunkCount; i++) {
            if (i != r.current && r.chunkUsed[i] <= done) {
                return i;
            }
        }

        if (CreateChunk(r)) {
            return r.chunkCount - 1;
        }

        uint32_t oldest = kMaxChunks;

        for (uint32_t i = 0; i < r.chunkCount; i++) {
            if (i != r.current && !frame::UsedThisFrame(r.chunkUsed[i]) && (oldest == kMaxChunks || r.chunkUsed[i] < r.chunkUsed[oldest])) {
                oldest = i;
            }
        }

        if (oldest != kMaxChunks && frame::WaitSerial(r.chunkUsed[oldest])) {
            g_stats.renameWaits++;
            return oldest;
        }

        return kMaxChunks;
    }

    /// The stream wrap's rename: a chunk no unfinished frame named, or a new one (FreeChunk).
    void Rename(PoolRec& r) {
        if (r.chunkUsed[r.current] <= frame::Completed()) {
            return;   // nothing in flight reads it: keep writing here
        }

        const uint32_t next = FreeChunk(r);

        if (next == kMaxChunks) {
            g_stats.renameHazards++;   // every chunk is read by this very frame: written in place
            return;
        }

        r.current = next;
        g_stats.renames++;
    }

    /// Whether [begin, end) of a dynamic pool overlaps what a draw of this frame bound.
    bool DrawnThisFrame(const PoolRec& r, uint32_t begin, uint32_t end) {
        if (r.drawnSerial == 0 || !frame::UsedThisFrame(r.drawnSerial)) {
            return false;
        }

        if (r.drawnAll) {
            return true;
        }

        for (uint32_t i = 0; i < r.drawnCount; i++) {
            if (begin < r.drawn[i].end && r.drawn[i].begin < end) {
                return true;
            }
        }

        return false;
    }

    /// The copy on write of a dynamic pool whose lock overlaps bytes a draw of this frame bound:
    /// the pool moves to a chunk no unfinished frame reads, filled from the shadow (which holds
    /// exactly what the chunk holds), so the earlier draw keeps its chunk and every later draw binds
    /// the new one. The new chunk carries no drawn range yet.
    void CopyOnWrite(PoolRec& r) {
        g_stats.dynamicOverlap++;
        const uint32_t next = r.shadow ? FreeChunk(r) : kMaxChunks;

        if (next == kMaxChunks || !r.chunks[next].block.mapped) {
            g_stats.dynamicInPlace++;
            return;
        }

        memcpy(r.chunks[next].block.mapped, r.shadow, r.size);
        memory::FlushHost(r.chunks[next].block);
        r.current = next;
        r.drawnSerial = 0;
        r.drawnCount = 0;
        r.drawnAll = false;
        g_stats.dynamicRenames++;
    }
}

// D3D's IBufLock (0x0068FB10), with the VkBuffer chunks behind it. Returns where the engine writes
// buf->m_size bytes.
char* CGxDeviceVulkan::IBufLock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;
    const uint32_t target = TargetOf(buf);
    Lock& l = LockFor(target, buf);
    l = Lock();
    l.buf = buf;

    // 1. No context: the emergency block.
    if (!this->m_context || !pool) {
        g_stats.emergency++;
        l.emergency = true;
        return static_cast<char*>(this->m_emergencyMem[target].Lock(buf->m_size));
    }

    // 2. The placement, which D3D does before it even has a buffer.
    bool wrapped = false;

    if (pool->m_usage == GxPoolUsage_Stream) {
        const uint32_t stride = buf->m_itemSize ? buf->m_itemSize : 1;
        const uint32_t index = (stride + pool->m_offset - 1) - (stride + pool->m_offset - 1) % stride;

        if (index + buf->m_size > static_cast<uint32_t>(pool->m_size)) {
            pool->Discard();
            buf->m_index = 0;
            pool->m_offset = buf->m_size;
            wrapped = true;
            g_stats.discards++;
        } else {
            buf->m_index = index;
            pool->m_offset = index + buf->m_size;
        }
    }

    if (pool->m_usage < GxPoolUsages_Last) {
        g_stats.locks[pool->m_usage]++;
    }

    // 3. The chunk on first use (D3D's CreatePoolAPI).
    PoolRec* r = Ensure(pool, this->m_drawPath != 0);

    if (!r) {
        g_stats.emergency++;
        l.emergency = true;
        return static_cast<char*>(this->m_emergencyMem[target].Lock(buf->m_size));
    }

    l.rec = r;

    if (wrapped) {
        Rename(*r);
    }

    // 4. The pointer.
    const uint32_t offset = buf->m_index;

    if (static_cast<uint64_t>(offset) + buf->m_size > r->size) {
        g_stats.emergency++;
        l.emergency = true;
        return static_cast<char*>(this->m_emergencyMem[target].Lock(buf->m_size));
    }

    if (r->usage == GxPoolUsage_Static) {
        const uint64_t used = r->chunkUsed[r->current];

        if (used > frame::Completed()) {
            if (frame::UsedThisFrame(used)) {
                g_stats.staticHazards++;
            } else if (frame::WaitSerial(used)) {
                g_stats.staticWaits++;
            }
        }
    } else if (r->usage == GxPoolUsage_Dynamic) {
        if (DrawnThisFrame(*r, offset, offset + buf->m_size)) {
            CopyOnWrite(*r);
        }

        if (r->shadow) {
            l.shadowed = true;
            return reinterpret_cast<char*>(r->shadow) + offset;
        }
    }

    return static_cast<char*>(r->chunks[r->current].block.mapped) + offset;
}

// D3D's IBufUnlock (0x0068FA60): the emergency case poisons the frame; otherwise the written range
// is made visible to the device (copied out of the shadow for a dynamic pool, flushed on a
// non-coherent type).
void CGxDeviceVulkan::IBufUnlock(CGxBuf* buf, uint32_t begin, uint32_t length) {
    const uint32_t target = TargetOf(buf);
    Lock* found = LockOf(target, buf);

    if (!found || found->emergency || !found->rec) {
        EmergencyMem& emergencyMem = this->m_emergencyMem[target];

        if (emergencyMem.m_lock) {
            emergencyMem.Unlock();
        }

        buf->m_valid = 0;
        this->m_intF5C = 1;

        if (found) {
            *found = Lock();
        }

        return;
    }

    Lock& l = *found;
    PoolRec& r = *l.rec;

    if (l.shadowed && r.shadow && r.chunks[r.current].block.mapped) {
        const uint64_t at = static_cast<uint64_t>(buf->m_index) + begin;

        if (at + length <= r.size) {
            memcpy(static_cast<uint8_t*>(r.chunks[r.current].block.mapped) + at, r.shadow + at, length);
        }
    }

    memory::FlushHost(r.chunks[r.current].block);   // a no-op on the coherent type
    buf->m_valid = 1;
    l = Lock();
}

// Mirrors CGxDeviceD3d::PoolSizeSet (0x0069FF80): the bufs invalidated, the old chunks retired with
// the frames that read them, the size, a chunk at the new size.
void CGxDeviceVulkan::PoolSizeSet(CGxPool* pool, uint32_t size) {
    if (!pool) {
        return;
    }

    pool->Invalidate();

    if (PoolRec* r = RecOf(pool)) {
        RetireAll(*r);
        DropShadow(*r);
    }

    pool->m_offset = 0;
    pool->m_size = size;
    Ensure(pool, this->m_drawPath != 0);
}

// Mirrors CGxDeviceD3d::PoolDestroy (0x0068E720): the chunks retired by serial, since the frame that
// last bound one may still be on the GPU; then the base unlinks and frees the pool.
void CGxDeviceVulkan::PoolDestroy(CGxPool* pool) {
    if (pool) {
        if (PoolRec* r = RecOf(pool)) {
            RetireAll(*r);
            delete r;
            RecOf(pool) = nullptr;
        }
    }

    CGxDevice::PoolDestroy(pool);
}

// Mirrors CGxDeviceD3d::BufLock (0x0068FCE0): the base (the dirty bytes, the locked-buf table), then
// the lock.
char* CGxDeviceVulkan::BufLock(CGxBuf* buf) {
    CGxDevice::BufLock(buf);
    return this->IBufLock(buf);
}

// Mirrors CGxDeviceD3d::BufUnlock (0x0068FAE0). The size argument is not what was written: the whole
// buf is what the lock handed out.
int32_t CGxDeviceVulkan::BufUnlock(CGxBuf* buf, uint32_t size) {
    CGxDevice::BufUnlock(buf, size);
    this->IBufUnlock(buf, 0, buf->m_size);
    return 1;
}

// Mirrors CGxDeviceD3d::BufData (0x0068FD00): the base, the lock, the copy of [offset, offset + size),
// the unlock -- the non-virtual pair, so the locked-buf table is not touched.
void CGxDeviceVulkan::BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset) {
    CGxDevice::BufData(buf, data, size, offset);

    char* p = this->IBufLock(buf);

    if (p && data && size) {
        memcpy(p + offset, data, size);
    }

    this->IBufUnlock(buf, offset, size);
}

// The VkBuffer a draw binds for a pool: the current chunk, stamped with the frame that binds it (the
// stamp the renames and the retirements read).
int32_t CGxDeviceVulkan::IPoolBuffer(CGxPool* pool, VkBuffer& buffer) {
    PoolRec* r = pool ? RecOf(pool) : nullptr;

    if (!r || r->chunkCount == 0) {
        g_stats.unresolved++;
        return 0;
    }

    r->chunkUsed[r->current] = frame::Serial();
    buffer = r->chunks[r->current].handle;
    return buffer != VK_NULL_HANDLE;
}

// The byte range of a dynamic pool a draw just bound (the header's "Dynamic"): touching or
// overlapping ranges merge, so a run of neighbouring bufs stays one entry.
void CGxDeviceVulkan::INoteBufDrawn(CGxBuf* buf) {
    if (!buf || !buf->m_pool) {
        return;
    }

    PoolRec* r = RecOf(buf->m_pool);

    if (!r || r->usage != GxPoolUsage_Dynamic) {
        return;
    }

    const uint64_t serial = frame::Serial();

    if (r->drawnSerial != serial) {
        r->drawnSerial = serial;
        r->drawnCount = 0;
        r->drawnAll = false;
    }

    if (r->drawnAll) {
        return;
    }

    const uint32_t begin = buf->m_index;
    const uint32_t end = begin + buf->m_size;

    for (uint32_t i = 0; i < r->drawnCount; i++) {
        if (begin <= r->drawn[i].end && r->drawn[i].begin <= end) {
            if (begin < r->drawn[i].begin) {
                r->drawn[i].begin = begin;
            }

            if (end > r->drawn[i].end) {
                r->drawn[i].end = end;
            }

            return;
        }
    }

    if (r->drawnCount == kMaxDrawnRanges) {
        r->drawnAll = true;
        return;
    }

    r->drawn[r->drawnCount++] = Range { begin, end };
}

// Every pool's record and chunks, destroyed at once: the device is idle (the caller waited for it)
// or gone. The pools themselves stay the engine's.
void CGxDeviceVulkan::IReleasePools() {
    for (CGxPool* pool = this->m_poolList.Head(); pool; pool = this->m_poolList.Next(pool)) {
        PoolRec* r = RecOf(pool);

        if (!r) {
            continue;
        }

        if (context::Live()) {
            DestroyAll(*r);
        }

        delete r;
        RecOf(pool) = nullptr;
    }

    for (uint32_t target = 0; target < GxPoolTargets_Last; target++) {
        for (uint32_t i = 0; i < kLockDepth; i++) {
            g_locks[target][i] = Lock();
        }
    }

    Write(Debug, "vulkan: pools: %llu record(s); locks static %llu / dynamic %llu / stream %llu; stream wraps %llu (renamed %llu, waited %llu, in-place hazards %llu); %llu chunk(s) made, at most %u in one pool; static waits %llu, hazards %llu; dynamic overlaps %llu (%llu renamed, %llu in place); emergency locks %llu; unresolved draws %llu",
          static_cast<unsigned long long>(g_stats.records), static_cast<unsigned long long>(g_stats.locks[0]),
          static_cast<unsigned long long>(g_stats.locks[1]), static_cast<unsigned long long>(g_stats.locks[2]),
          static_cast<unsigned long long>(g_stats.discards), static_cast<unsigned long long>(g_stats.renames),
          static_cast<unsigned long long>(g_stats.renameWaits), static_cast<unsigned long long>(g_stats.renameHazards),
          static_cast<unsigned long long>(g_stats.chunksCreated), g_stats.mostChunks,
          static_cast<unsigned long long>(g_stats.staticWaits), static_cast<unsigned long long>(g_stats.staticHazards),
          static_cast<unsigned long long>(g_stats.dynamicOverlap), static_cast<unsigned long long>(g_stats.dynamicRenames),
          static_cast<unsigned long long>(g_stats.dynamicInPlace), static_cast<unsigned long long>(g_stats.emergency),
          static_cast<unsigned long long>(g_stats.unresolved));
}
