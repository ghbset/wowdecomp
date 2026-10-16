// Occlusion queries: D3D9's D3DQUERYTYPE_OCCLUSION over one VkQueryPool, with D3D's state machine --
// a query never issued answers as complete with zero samples, an issued one answers once the frame
// that recorded its end has run.
#pragma once

#include <cstdint>

namespace gxvk::vk::queries
{
    /// A query handle: 1-based; 0 is none.
    using Handle = uint32_t;

    /// The pool, after the context exists. Idempotent.
    bool Init();

    /// Everything, after the device is idle.
    void Shutdown();

    /// A query from the pool, or 0 when the pool is full (logged once).
    Handle Alloc();

    void Free(Handle q);

    /**
     * D3D's Issue(BEGIN): the query reset ahead of the frame and begun on the frame's pass. One
     * query may be active at a time.
     *
     * @return bool begun
     */
    bool Begin(Handle q);

    /// D3D's Issue(END): the query ended on the frame's pass; its result belongs to this frame.
    bool End(Handle q);

    /**
     * D3D's GetData(NULL) == D3D_OK: whether the result can be read without waiting. True for a
     * query never issued (D3D's initial signalled state).
     */
    bool Available(Handle q);

    /**
     * D3D's GetData with FLUSH, looped until ready: the frame holding the query's end is submitted
     * if it is still open, then the result is waited for.
     *
     * @param uint32& samples : the samples that passed; 0 for a query never issued
     * @return bool ok : false for an unknown query or a failed read
     */
    bool Result(Handle q, uint32_t& samples);

    struct Stats
    {
        uint64_t begun     = 0;
        uint64_t ended     = 0;
        uint64_t results   = 0;   ///< Result() calls
        uint64_t waits     = 0;   ///< Result() calls that blocked on the GPU
        uint64_t flushes   = 0;   ///< Result() calls that submitted the open frame
        uint32_t live      = 0;
    };
    void GetStats(Stats& out);
}
