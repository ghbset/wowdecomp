// TREESWAY: not in 12340. Engine glue for M2TreeSway.h (wxl-treewind ApplySway).
#include "Engine/Source/Model2/M2TreeSway.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Tempest/C44Matrix.h"

namespace M2TreeSway {

int32_t s_enabled = 1;
float s_strength = 1.0f;

namespace {

// m_flags 0x20: set only for map-placed static doodads (ADT/WMO), never units, weapons or spell FX.
constexpr uint32_t kDoodadFlag = 0x20;

// The name scans are the hot cost and never change for a model, so each thread (AnimateMT runs on the
// animate workers) memoizes them per CM2Shared; the name hash guards against a reused address.
struct Memo {
    const CM2Shared* shared;
    uint32_t hash;
    Kind kind;
};
thread_local Memo t_memo[256];

uint32_t NameHash(const char* s) {
    uint32_t h = 2166136261u; // FNV-1a
    for (int i = 0; s[i] && i < 128; ++i) {
        h ^= static_cast<uint8_t>(s[i]);
        h *= 16777619u;
    }
    return h;
}

} // namespace

Kind KindOf(const CM2Model* model) {
    if (!s_enabled || !(model->m_flags & kDoodadFlag)) return None;

    const CM2Shared* shared = model->m_shared;
    if (!shared || !shared->m_data) return None; // still loading

    Memo& memo = t_memo[(reinterpret_cast<uintptr_t>(shared) >> 4) & 255];
    uint32_t hash = NameHash(shared->m_fileName);
    if (memo.shared != shared || memo.hash != hash) {
        memo.shared = shared;
        memo.hash = hash;
        memo.kind = Classify(shared->m_fileName, shared->m_data->m_lights.m_count);
    }
    if (memo.kind != Tree) return memo.kind;

    // A giant world tree keeps its stock pose instead of leaning its canopy across the zone.
    const CAaBox& box = shared->m_data->m_bounds.m_extent;
    float ez = box.t.z - box.b.z;
    const float* m = reinterpret_cast<const float*>(&model->m_worldTransform);
    float scaleSq = 0.0f;
    for (int r = 0; r < 3; ++r) {
        float len = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (len > scaleSq) scaleSq = len;
    }
    return ez * ez * scaleSq > kMaxTreeHeight * kMaxTreeHeight ? None : Tree;
}

C44Matrix World(const CM2Model* model) {
    C44Matrix world = model->m_worldTransform;
    Kind kind = KindOf(model);
    if (kind == None) return world;

    float* m = reinterpret_cast<float*>(&world);
    float t = static_cast<float>(model->m_scene->m_time % 3600000u) * 0.001f;
    State s = Evaluate(kind == Bush ? kBushWind : kTreeWind, m[12], m[13], t, s_strength);
    float rot[9];
    TiltMatrix(s.tx, s.ty, rot);

    // world' = world * rot on the upper 3x3: tilt about the model origin, translation kept.
    for (int r = 0; r < 3; ++r) {
        float b0 = m[r * 4], b1 = m[r * 4 + 1], b2 = m[r * 4 + 2];
        m[r * 4 + 0] = b0 * rot[0] + b1 * rot[3] + b2 * rot[6];
        m[r * 4 + 1] = b0 * rot[1] + b1 * rot[4] + b2 * rot[7];
        m[r * 4 + 2] = b0 * rot[2] + b1 * rot[5] + b2 * rot[8];
    }
    return world;
}

} // namespace M2TreeSway
