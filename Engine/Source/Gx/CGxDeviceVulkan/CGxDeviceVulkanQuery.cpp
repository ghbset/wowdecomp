// The Vulkan backend's queries: the engine's occlusion queries (the sun glare's) on the backend's
// query pool, with D3D9's answers -- a query never issued reads as complete.
#include <cstdint>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Queries.hpp"

namespace queries = gxvk::vk::queries;

namespace {
    /// The pool handle behind a CGxQuery, taken on first use as D3D creates its query lazily.
    queries::Handle HandleOf(CGxQuery* query) {
        if (!query) {
            return 0;
        }

        if (!query->m_apiSpecific && query->m_type == GxQueryType_Occlusion) {
            const queries::Handle h = queries::Alloc();

            if (h) {
                query->m_apiSpecific = reinterpret_cast<void*>(static_cast<uintptr_t>(h));
            }
        }

        return static_cast<queries::Handle>(reinterpret_cast<uintptr_t>(query->m_apiSpecific));
    }
}

// Mirrors CGxDeviceD3d::QueryCreate (0x0068E9C0): the base's list entry, then the pool's query.
void CGxDeviceVulkan::QueryCreate(CGxQuery*& query, EGxQueryType type) {
    CGxDevice::QueryCreate(query, type);

    if (this->m_context && this->m_drawPath) {
        HandleOf(query);
    }
}

// Mirrors CGxDeviceD3d::QueryDestroy (0x006A0190).
void CGxDeviceVulkan::QueryDestroy(CGxQuery*& query) {
    if (query && query->m_apiSpecific) {
        queries::Free(static_cast<queries::Handle>(reinterpret_cast<uintptr_t>(query->m_apiSpecific)));
        query->m_apiSpecific = nullptr;
    }

    CGxDevice::QueryDestroy(query);
}

// Mirrors CGxDeviceD3d::QueryBegin (0x0068EA10).
bool CGxDeviceVulkan::QueryBegin(CGxQuery* query) {
    if (!this->m_context || !this->m_drawPath) {
        return false;
    }

    const queries::Handle h = HandleOf(query);

    if (!h) {
        return false;
    }

    return query->m_type != GxQueryType_Occlusion || queries::Begin(h);
}

// Mirrors CGxDeviceD3d::QueryEnd (0x006A0240).
bool CGxDeviceVulkan::QueryEnd(CGxQuery* query) {
    if (!this->m_context || !query || !query->m_apiSpecific) {
        return false;
    }

    const queries::Handle h = static_cast<queries::Handle>(reinterpret_cast<uintptr_t>(query->m_apiSpecific));
    return query->m_type != GxQueryType_Occlusion || queries::End(h);
}

// Mirrors CGxDeviceD3d::QueryGetParam (0x0068EA90): Unk0 is the result's size, Unk1 whether the
// result can be read without waiting (D3D's GetData(NULL) == D3D_OK, which the sun glare reads
// before it asks for the data).
bool CGxDeviceVulkan::QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) {
    if (!this->m_context || !this->m_drawPath) {
        return false;
    }

    const queries::Handle h = HandleOf(query);

    if (!h) {
        return false;
    }

    switch (param) {
        case GxQueryParam_Unk0:
            data = sizeof(uint32_t);
            return true;

        case GxQueryParam_Unk1:
            data = queries::Available(h) ? 1u : 0u;
            return true;

        default:
            return false;
    }
}

// Mirrors CGxDeviceD3d::QueryGetData (0x006A0310): the result, waited for.
bool CGxDeviceVulkan::QueryGetData(CGxQuery* query, uint32_t& data) {
    if (!this->m_context || !query || !query->m_apiSpecific) {
        return false;
    }

    const queries::Handle h = static_cast<queries::Handle>(reinterpret_cast<uintptr_t>(query->m_apiSpecific));
    return queries::Result(h, data);
}
