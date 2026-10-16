// Occlusion queries over one VkQueryPool.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Queries.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Warn;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    /// Queries the pool holds: the sun glare's one, plus up to 2048 for occlusion culling.
    // OCCLUSION: raised from 64 for WoW/Source/WorldClient/OcclusionCull.cpp.
    constexpr uint32_t kMaxQueries = 4096;

    struct Query
    {
        bool     used   = false;
        bool     issued = false;   ///< begun at least once: a result exists or is coming
        bool     active = false;   ///< begun, not ended
        uint64_t serial = 0;       ///< the frame holding the end
    };

    struct State
    {
        bool                  ready   = false;
        VkDevice              device  = nullptr;
        const loader::Device* dapi    = nullptr;
        VkQueryPool           pool    = VK_NULL_HANDLE;
        bool                  precise = false;
        Query                 queries[kMaxQueries];
        bool                  warnedFull = false;
        gxvk::vk::queries::Stats stats;
    };
    State s;

    Query* Of(gxvk::vk::queries::Handle q)
    {
        if (!s.ready || q == 0 || q > kMaxQueries || !s.queries[q - 1].used) return nullptr;
        return &s.queries[q - 1];
    }
}

namespace gxvk::vk::queries
{
    bool Init()
    {
        if (s.ready) return true;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi)
        {
            Write(Warn, "vulkan: queries: Init() without a device; declining");
            return false;
        }
        s.device  = h->device;
        s.dapi    = h->dapi;
        s.precise = h->occlusionQueryPrecise;

        VkQueryPoolCreateInfo info = {};
        info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType  = VK_QUERY_TYPE_OCCLUSION;
        info.queryCount = kMaxQueries;
        const VkResult r = s.dapi->CreateQueryPool(s.device, &info, nullptr, &s.pool);
        if (r != VK_SUCCESS)
        {
            s.pool = VK_NULL_HANDLE;
            Write(Error, "vulkan: queries: vkCreateQueryPool failed: %s", ResultName(r));
            return false;
        }
        s.ready = true;
        Write(Debug, "vulkan: queries: %u occlusion queries, %s", kMaxQueries, s.precise ? "precise sample counts" : "any-sample counts");
        return true;
    }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        if (s.pool != VK_NULL_HANDLE) s.dapi->DestroyQueryPool(s.device, s.pool, nullptr);
        s = State();
    }

    Handle Alloc()
    {
        if (!s.ready) return 0;
        for (uint32_t i = 0; i < kMaxQueries; ++i)
        {
            if (s.queries[i].used) continue;
            s.queries[i]      = Query();
            s.queries[i].used = true;
            ++s.stats.live;
            return i + 1;
        }
        if (!s.warnedFull)
        {
            s.warnedFull = true;
            Write(Warn, "vulkan: queries: all %u queries are in use; further ones are refused (logged once)", kMaxQueries);
        }
        return 0;
    }

    void Free(Handle q)
    {
        Query* e = Of(q);
        if (!e) return;
        if (e->active) frame::EndQuery();
        *e = Query();
        --s.stats.live;
    }

    bool Begin(Handle q)
    {
        Query* e = Of(q);
        if (!e || e->active || frame::QueryActive()) return false;
        // The reset ahead of the frame, outside any pass; the begin on the frame's pass.
        VkCommandBuffer pre = frame::PrePass();
        if (!pre) return false;
        s.dapi->CmdResetQueryPool(pre, s.pool, q - 1, 1);
        if (!frame::BeginQuery(s.pool, q - 1, s.precise)) return false;
        e->active = true;
        e->issued = true;
        e->serial = 0;
        ++s.stats.begun;
        return true;
    }

    bool End(Handle q)
    {
        Query* e = Of(q);
        if (!e || !e->active) return false;
        e->serial = frame::EndQuery();
        e->active = false;
        ++s.stats.ended;
        return e->serial != 0;
    }

    bool Available(Handle q)
    {
        Query* e = Of(q);
        if (!e) return false;
        if (!e->issued) return true;
        if (e->active || e->serial == 0 || e->serial > frame::Submitted()) return false;
        uint32_t value = 0;
        const VkResult r = s.dapi->GetQueryPoolResults(s.device, s.pool, q - 1, 1, sizeof value, &value, sizeof value, 0);
        return r == VK_SUCCESS;
    }

    bool Result(Handle q, uint32_t& samples)
    {
        Query* e = Of(q);
        if (!e) return false;
        ++s.stats.results;
        if (!e->issued)
        {
            samples = 0;
            return true;
        }
        if (e->active)
        {
            // An end the engine has not issued: D3D's GetData would answer S_FALSE forever; the
            // query is ended here so the read can complete.
            End(q);
        }
        if (e->serial == 0) return false;
        if (e->serial > frame::Submitted())
        {
            frame::Flush();
            ++s.stats.flushes;
        }
        if (e->serial > frame::Completed()) ++s.stats.waits;
        uint32_t value = 0;
        const VkResult r = s.dapi->GetQueryPoolResults(s.device, s.pool, q - 1, 1, sizeof value, &value, sizeof value, VK_QUERY_RESULT_WAIT_BIT);
        if (r != VK_SUCCESS) return false;
        samples = value;
        return true;
    }

    void GetStats(Stats& out) { out = s.stats; }
}
