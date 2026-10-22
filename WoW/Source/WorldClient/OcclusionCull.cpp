// OCCLUSION: see OcclusionCull.h.
#include "WoW/Source/WorldClient/OcclusionCull.h"
#include <cstring>
#include <unordered_map>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/CAaSphere.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/World.h"

CVar* OcclusionCull::s_cvar;

namespace {
    /// Queries in flight at once. The Vulkan pool holds kMaxQueries (Queries.cpp) and the sun glare
    /// takes one of them. 8 verts a box keeps the indices in uint16.
    constexpr uint32_t kMaxQueries = 2048;
    /// Query boxes are grown by this much (yards) so a box never sits behind its own surface.
    constexpr float kPad = 1.0f;
    /// A camera inside the box grown by this much is never culled and never queried (near plane).
    constexpr float kInsidePad = kPad + 2.0f;
    /// Camera moves past this reset every answer (teleport, loading screen). No reset on a slow frame:
    /// a time gate made culling flip on and off every other frame near the threshold (flicker with a
    /// low background FPS cap); the per-answer position and direction gates already cover stale answers.
    constexpr float kJumpDistSq = 10.0f * 10.0f;
    /// Doodads smaller than this (sphere radius, yards) cost less to draw than to query.
    constexpr float kMinDoodadRadius = 1.0f;
    /// An "occluded" answer holds only while the camera is within this of where its query was drawn.
    /// Occlusion from a point does not depend on the view direction, but it does on the position: a
    /// near occluder's edge sweeps far objects by (move x object distance / occluder distance), which no
    /// box padding covers, and the answer is 1-3 frames old. Honouring stale answers while moving hid
    /// visible WMOs and doodads for several frames at every silhouette (the "flicker when moving").
    /// ponytail: culling is in effect only while (nearly) standing still; a reprojected or same-frame
    /// test is the upgrade if moving-camera savings matter.
    constexpr float kAnswerMoveSq = 0.05f * 0.05f;
    /// ...and while the view direction is within about 0.8 degrees of it. The box is only rasterised
    /// where it is on screen and tested against on-screen depth, so after a turn an "occluded" answer
    /// can belong to the part of the box that was in view, not the part that is now.
    constexpr float kAnswerDirDot = 0.9999f;

    struct Entry {
        CAaBox box;
        uint32_t lastSeen = 0;      ///< frame it was last a culling candidate
        CGxQuery* query = nullptr;  ///< in flight
        bool occluded = false;      ///< the last answer
        C3Vector queryAt;           ///< camera position the query in flight was drawn from
        C3Vector answerAt;          ///< camera position the last answer belongs to
        C3Vector queryDir;          ///< view direction the query in flight was drawn with
        C3Vector answerDir;         ///< view direction the last answer belongs to
        bool queued = false;        ///< in s_toQuery this frame
    };

    bool s_active;
    CGxDevice* s_device;
    uint32_t s_frame = 1;
    C3Vector s_lastCam;
    std::unordered_map<const void*, Entry> s_entries;
    std::vector<Entry*> s_toQuery;
    std::vector<CGxQuery*> s_free;
    std::vector<CGxQuery*> s_draining;   ///< released while still in flight: free once their result is in
    uint32_t s_created;

    /// `idle`: the query has no result pending (answered, or never begun), so it can be reused at once.
    void Release(Entry& e, bool idle = false) {
        if (e.query) {
            (idle ? s_free : s_draining).push_back(e.query);
            e.query = nullptr;
        }
    }

    void Drain() {
        for (size_t i = 0; i < s_draining.size();) {
            uint32_t available = 0;

            if (GxQueryGetParam(s_draining[i], GxQueryParam_Unk1, &available) && available) {
                s_free.push_back(s_draining[i]);
                s_draining[i] = s_draining.back();
                s_draining.pop_back();
            } else {
                i++;
            }
        }
    }

    void ResetAll() {
        for (auto& kv : s_entries) {
            Release(kv.second);
        }

        s_entries.clear();
        s_toQuery.clear();
    }

    bool Enabled() {
        return OcclusionCull::s_cvar && OcclusionCull::s_cvar->m_intValue && g_theGxDevicePtr
            && g_theGxDevicePtr->m_api == GxApi_Vulkan && g_theGxDevicePtr->Caps().m_occlusionQuery;
    }

    bool CameraInside(const CAaBox& box, float pad) {
        const C3Vector& c = CWorldScene::s_activeWorldView;
        return c.x >= box.b.x - pad && c.x <= box.t.x + pad
            && c.y >= box.b.y - pad && c.y <= box.t.y + pad
            && c.z >= box.b.z - pad && c.z <= box.t.z + pad;
    }

    CGxQuery* TakeQuery() {
        if (!s_free.empty()) {
            CGxQuery* q = s_free.back();
            s_free.pop_back();
            return q;
        }

        if (s_created >= kMaxQueries) {
            return nullptr;
        }

        CGxQuery* q = nullptr;
        GxQueryCreate(q, GxQueryType_Occlusion);

        if (q) {
            s_created++;
        }

        return q;
    }
}

void OcclusionCull::BeginFrame() {
    s_active = Enabled();

    if (!s_active) {
        if (!s_entries.empty()) {
            ResetAll();
        }

        return;
    }

    if (s_device != g_theGxDevicePtr) {
        // ponytail: queries of a replaced device are abandoned (leaked), not destroyed; a device
        // swap is rare. Destroy them on the old device if that ever matters.
        ResetAll();
        s_free.clear();
        s_draining.clear();
        s_created = 0;
        s_device = g_theGxDevicePtr;
    }

    const C3Vector cam = CWorldScene::s_activeWorldView;
    const C3Vector d = cam - s_lastCam;

    if (d.x * d.x + d.y * d.y + d.z * d.z > kJumpDistSq) {
        ResetAll();
    }

    s_lastCam = cam;
    s_frame++;
    s_toQuery.clear();
    Drain();

    for (auto it = s_entries.begin(); it != s_entries.end();) {
        Entry& e = it->second;
        e.queued = false;

        if (e.query) {
            uint32_t available = 0;

            if (GxQueryGetParam(e.query, GxQueryParam_Unk1, &available) && available) {
                uint32_t samples = 1;
                e.occluded = GxQueryGetData(e.query, &samples) && samples == 0;
                e.answerAt = e.queryAt;
                e.answerDir = e.queryDir;
                Release(e, true);
            }
        }

        // Not a candidate last frame: off screen, CPU-culled or unloaded. Its answer is stale.
        if (e.lastSeen + 1 < s_frame) {
            Release(e);
            it = s_entries.erase(it);
        } else {
            ++it;
        }
    }
}

bool OcclusionCull::IsOccluded(const void* key, const CAaBox& box) {
    if (!s_active) {
        return false;
    }

    if (CameraInside(box, kInsidePad)) {
        return false;
    }

    Entry& e = s_entries[key];

    // New, or moved / a different object at a reused address: no valid answer.
    if (e.lastSeen == 0 || memcmp(&e.box, &box, sizeof(CAaBox)) != 0) {
        e.box = box;
        e.occluded = false;
    }

    e.lastSeen = s_frame;

    if (!e.query && !e.queued) {
        e.queued = true;
        s_toQuery.push_back(&e);
    }

    const C3Vector d = CWorldScene::s_activeWorldView - e.answerAt;
    const C3Vector& v = CWorldScene::camVec;
    return e.occluded && d.x * d.x + d.y * d.y + d.z * d.z <= kAnswerMoveSq
        && v.x * e.answerDir.x + v.y * e.answerDir.y + v.z * e.answerDir.z >= kAnswerDirDot;
}

bool OcclusionCull::IsOccluded(const void* key, const CAaSphere& sphere) {
    if (!s_active || sphere.r < kMinDoodadRadius) {
        return false;
    }

    CAaBox box;
    box.b = C3Vector(sphere.c.x - sphere.r, sphere.c.y - sphere.r, sphere.c.z - sphere.r);
    box.t = C3Vector(sphere.c.x + sphere.r, sphere.c.y + sphere.r, sphere.c.z + sphere.r);
    return OcclusionCull::IsOccluded(key, box);
}

void OcclusionCull::IssueQueries() {
    if (!s_active || s_toQuery.empty()) {
        return;
    }

    // Each queued entry gets a query or is left without one (counts as visible next frame).
    static std::vector<Entry*> batch;   // static: reused every frame
    batch.clear();

    for (Entry* e : s_toQuery) {
        if (CameraInside(e->box, kInsidePad)) {
            e->occluded = false;
            continue;
        }

        e->query = TakeQuery();

        if (!e->query) {
            e->occluded = false;
            continue;
        }

        e->queryAt = CWorldScene::s_activeWorldView;
        e->queryDir = CWorldScene::camVec;
        batch.push_back(e);
    }

    s_toQuery.clear();

    if (batch.empty()) {
        return;
    }

    const uint32_t count = static_cast<uint32_t>(batch.size());
    const C3Vector& cam = CWorldScene::s_activeWorldView;

    C44Matrix world;
    world.Translate(C3Vector(-cam.x, -cam.y, -cam.z));
    g_theGxDevicePtr->XformPushAndSet(GxXform_World, world);

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_ColorWrite, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthFunc, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_PixelShader, static_cast<void*>(nullptr));
    GxRsSet(GxRs_VertexShader, static_cast<void*>(nullptr));
    GxRsSet(GxRs_Texture0, static_cast<void*>(nullptr));

    CGxBuf* vertexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, sizeof(CGxVertexPN), 8 * count);
    CGxVertexPN* vertex = reinterpret_cast<CGxVertexPN*>(g_theGxDevicePtr->BufLock(vertexBuf));

    for (Entry* e : batch) {
        const C3Vector lo(e->box.b.x - kPad, e->box.b.y - kPad, e->box.b.z - kPad);
        const C3Vector hi(e->box.t.x + kPad, e->box.t.y + kPad, e->box.t.z + kPad);

        for (uint32_t i = 0; i < 8; i++) {
            vertex[i].m_position = C3Vector(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
            vertex[i].m_normal = C3Vector(0.0f, 0.0f, 1.0f);
        }

        vertex += 8;
    }

    g_theGxDevicePtr->BufUnlock(vertexBuf, 0);
    vertexBuf->m_hasData = 1;
    GxPrimVertexPtr(vertexBuf, GxVBF_PN);

    // 12 triangles over the corners (bit 0 = x, bit 1 = y, bit 2 = z); culling is off so winding is free.
    static const uint16_t kFaces[36] = {
        0, 1, 3, 0, 3, 2,   4, 6, 7, 4, 7, 5,
        0, 4, 5, 0, 5, 1,   2, 3, 7, 2, 7, 6,
        0, 2, 6, 0, 6, 4,   1, 5, 7, 1, 7, 3,
    };

    CGxBuf* indexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, sizeof(uint16_t), 36 * count);
    uint16_t* indices = reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(indexBuf));

    for (uint32_t b = 0; b < count; b++) {
        for (uint32_t i = 0; i < 36; i++) {
            *indices++ = static_cast<uint16_t>(8 * b + kFaces[i]);
        }
    }

    g_theGxDevicePtr->BufUnlock(indexBuf, 0);
    indexBuf->m_hasData = 1;
    g_theGxDevicePtr->PrimIndexPtr(indexBuf);

    for (uint32_t b = 0; b < count; b++) {
        Entry* e = batch[b];

        if (!GxQueryBegin(e->query)) {
            Release(*e, true);
            e->occluded = false;
            continue;
        }

        CGxBatch draw;
        draw.m_primType = GxPrim_Triangles;
        draw.m_start = 36 * b;
        draw.m_count = 36;
        draw.m_minIndex = static_cast<uint16_t>(8 * b);
        draw.m_maxIndex = static_cast<uint16_t>(8 * b + 7);
        g_theGxDevicePtr->Draw(&draw, 1);

        if (!GxQueryEnd(e->query)) {
            Release(*e);
            e->occluded = false;
        }
    }

    g_theGxDevicePtr->RsPop();
    GxXformPop(GxXform_World);
}
