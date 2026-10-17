// OCCLUSION: GPU occlusion culling for exterior WMO groups and doodads (not in the 3.3.5a binary).
//
// Classic one-frame-late scheme: every object that survives the stock CPU culling is tested with a
// depth-only bounding-box draw inside an occlusion query, after terrain and WMO groups have filled
// depth. The answer, read whenever the backend has it (non-blocking), decides whether the object is
// drawn on later frames. Anything without an answer, newly in view, moved, or after a camera jump
// counts as visible; occluded objects keep being queried so they come back.
// An "occluded" answer only counts while the camera is where its query was drawn (see
// kAnswerMoveSq): stale answers from a moving camera hid visible objects for frames at a time.
//
// Active only on the Vulkan backend with CVar occlusionCulling = 1. Otherwise every call is a no-op
// and culling is exactly stock.
#pragma once

class CAaBox;
class CAaSphere;
struct CVar;

namespace OcclusionCull {
    extern CVar* s_cvar;

    /// Start of CWorldScene::Render: poll answers, drop objects not seen last frame, detect jumps.
    void BeginFrame();

    /// During culling: whether the object (identified by its address) was occluded last time it was
    /// tested. Also queues it for a query this frame.
    bool IsOccluded(const void* key, const CAaBox& box);
    bool IsOccluded(const void* key, const CAaSphere& sphere);

    /// After the opaque terrain + WMO passes: issue the queued bounding-box queries.
    void IssueQueries();
}
