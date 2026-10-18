#ifndef ENGINE_SOURCE_MODEL2_M2TREESWAY_H
#define ENGINE_SOURCE_MODEL2_M2TREESWAY_H

// TREESWAY: not in 12340. Port of wxl-treewind TreeWind.cpp (branch leaves, the shipped one), sway only.
// Each map-placed living tree (and bush) gets a small rigid tilt about its origin (the trunk base),
// driven by a travelling two-wave gust field. The tilt goes into the model's world*view (m_matrixF4 /
// the shadow-map bone 0) in AnimateMT, AnimateMTSimple and AnimateSM, so the drawn palette and the sun
// shadow casters carry the same tilt; m_worldTransform itself is never touched (no pristine-base cache).
// Pure parts (name filter, wind field) are engine-free so Tools/TreeSwayCheck/check.cpp can test them,
// and so the falling-leaves follow-up can reuse the same field per tree.

#include <cmath>
#include <cstdint>
#include <cstring>

class C44Matrix;
class CM2Model;

namespace M2TreeSway {

enum Kind : uint8_t { None = 0, Tree = 1, Bush = 2 };

// wxl-treewind WindSettings (its .ini defaults). Tree and bush rows are independent.
struct Wind {
    float directionDeg;      // heading in the world XY plane
    float speed;             // wave travel, yards per second
    float amplitudeDeg;      // primary wave tilt
    float wavelength;        // primary wave length, yards
    float crossAmplitudeDeg; // secondary cross-swell tilt
    float crossWavelength;
    float crossAngleDeg;     // secondary heading offset
    float leanDeg;           // constant downwind lean
    float variance;          // per-tree amplitude spread 0..1
    float gust;              // slow gust modulation 0..1
};
constexpr Wind kTreeWind = { 35.0f, 3.0f, 0.60f, 26.0f, 0.22f, 9.0f, 40.0f, 0.25f, 0.50f, 0.35f };
constexpr Wind kBushWind = { 35.0f, 2.0f, 0.50f, 9.0f, 0.25f, 5.0f, 40.0f, 0.20f, 0.60f, 0.40f };
constexpr float kMaxTreeHeight = 200.0f; // world yards; taller trees (world trees) stay rigid

// CVars treeSway / treeSwayStrength (WorldParam_CVar.cpp).
extern int32_t s_enabled;
extern float s_strength;

inline bool ContainsNoCase(const char* hay, const char* needle) {
    for (int i = 0; hay[i] && i < 512; ++i) {
        int h = i, n = 0;
        while (hay[h] && needle[n]) {
            char a = hay[h], b = needle[n];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
            ++h; ++n;
        }
        if (!needle[n]) return true;
    }
    return false;
}

template <size_t N>
inline bool Any(const char* path, const char* const (&words)[N]) {
    for (const char* w : words)
        if (ContainsNoCase(path, w)) return true;
    return false;
}

// Dead, bare, burnt or felled wood, or a loose tree part; "blight" except as Dragonblight.
inline bool IsDead(const char* path) {
    static const char* const kDead[] = {
        "dead", "deadwood", "burnt", "burned", "charred", "scorch", "wither",
        "wilted", "corrupt", "cursed", "bare", "leafless", "defoliated",
        "skeletal", "petrified", "stump", "snag", "log", "driftwood", "uprooted",
        "felled", "fallen", "rotten", "hollow", "burning", "noleaves", "canopyless", "broken",
        "cuttree", "treebite", "treebreak", "blastedtree", "dying", "infected",
    };
    if (Any(path, kDead)) return true;
    auto prefix = [](const char* s, const char* p) {
        for (; *p; ++s, ++p)
            if (!*s || (*s | 0x20) != *p) return false;
        return true;
    };
    for (const char* at = path; *at; ++at)
        if (prefix(at, "blight") && (at - path < 6 || !prefix(at - 6, "dragon")))
            return true;
    return false;
}

// lightCount: the model's own lights (a lit model is a lamp or a building, whatever it is called).
// Bushes win when bushSway is on; a bush-named model otherwise falls through to the tree test.
inline Kind Classify(const char* path, uint32_t lightCount, bool bushSway = true) {
    static const char* const kTreeWords[] = {
        "tree", "pines", "pine", "spruce", "cedar", "oak", "willow", "birch", "aspen",
        "palm", "canopy", "conifer", "maple", "eucalyptus", "redwood", "cypress",
        "larch", "juniper", "mangrove", "olive", "baobab", "treant",
    };
    static const char* const kNotTree[] = {
        "street", "lamp", "lantern", "torch", "brazier", "candle", "signpost", "treehouse",
        "spine", "cloak", "pineapple",
        "grass", "reed", "cattail", "flower", "clover", "cactus", "crop", "vine", "fruit", "seedpod",
        "pinecone\\", "felpinecone", "grizzlyhills_mushroom", "shelfmushroom", "thinmushroom", "plaguelandmushroom",
        "root", "trunk", "branch", "treeleaf", "leaves",
        "wallarch", "rock", "bedcanopy", "boardwalk", "dock", "gate", "bridge", "treehut", "facade",
        "barrier", "banner", "portal", "teleport", "smoke", "lightbeam", "waterfall", "simon",
        "karazahn", "xmas", "coral", "floatingdebris", "nest", "crater",
    };
    static const char* const kBushWords[] = {
        "bush", "shrub", "fern", "hedge", "thicket", "bramble", "undergrowth",
        "topiary", "bracken", "sapling",
    };
    static const char* const kNotBush[] = {
        "infernal", "staff", "pumpkin", "cactus", "azjol_mushroom", "shelfmushroom", "thinmushroom",
        "spore", "glowthread",
    };
    if (IsDead(path)) return None;
    if (bushSway && !Any(path, kNotBush) && Any(path, kBushWords)) return Bush;
    if (lightCount == 0 && Any(path, kTreeWords) && !Any(path, kNotTree)) return Tree;
    return None;
}

// Deterministic 0..1 hash of a world position: independent phase and amplitude per tree.
inline float Hash01(float x, float y) {
    uint32_t ix = static_cast<uint32_t>(static_cast<int32_t>(x * 8.0f));
    uint32_t iy = static_cast<uint32_t>(static_cast<int32_t>(y * 8.0f));
    uint32_t h = ix * 73856093u ^ iy * 19349663u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

// The per-tree state the leaves follow-up needs too: tilt (tx, ty) in radians toward a world-plane
// direction, plus the seed, gust and primary wind direction it was built from.
struct State {
    float tx, ty;
    float seed;       // 0..2pi
    float d1x, d1y;   // primary wind direction
};

// wxl-treewind ApplySway's field at tree origin (px, py), time t seconds, amplitudes scaled by strength.
inline State Evaluate(const Wind& w, float px, float py, float t, float strength) {
    constexpr float kTwoPi = 6.28318530717959f;
    constexpr float kDeg2Rad = 3.14159265358979f / 180.0f;
    float a1 = w.directionDeg * kDeg2Rad;
    float a2 = (w.directionDeg + w.crossAngleDeg) * kDeg2Rad;
    float k1 = kTwoPi / (w.wavelength > 0.1f ? w.wavelength : 0.1f);
    float k2 = kTwoPi / (w.crossWavelength > 0.1f ? w.crossWavelength : 0.1f);
    State s;
    s.d1x = std::cos(a1); s.d1y = std::sin(a1);
    float d2x = std::cos(a2), d2y = std::sin(a2);
    s.seed = Hash01(px, py) * kTwoPi;
    float ampScale = (1.0f + (Hash01(py, px) - 0.5f) * 2.0f * w.variance) * strength;
    float gust = 1.0f + w.gust * std::sin(t * 0.37f + s.seed * 0.5f);
    float s1 = std::sin(k1 * (px * s.d1x + py * s.d1y) - k1 * w.speed * t + s.seed);
    float s2 = std::sin(k2 * (px * d2x + py * d2y) - k2 * w.speed * 1.37f * t + s.seed * 0.6f);
    float deg1 = (w.amplitudeDeg * s1 * gust + w.leanDeg) * ampScale;
    float deg2 = (w.crossAmplitudeDeg * s2 * gust) * ampScale;
    s.tx = (deg1 * s.d1x + deg2 * d2x) * kDeg2Rad;
    s.ty = (deg1 * s.d1y + deg2 * d2y) * kDeg2Rad;
    return s;
}

// Row-vector 3x3 rotation tilting "up" toward (tx, ty); the origin stays fixed.
inline void TiltMatrix(float tx, float ty, float m[9]) {
    float t2 = tx * tx + ty * ty;
    std::memset(m, 0, sizeof(float) * 9);
    if (t2 < 1.0e-12f) {
        m[0] = m[4] = m[8] = 1.0f;
        return;
    }
    float theta = std::sqrt(t2);
    float ax = -ty / theta, ay = tx / theta;
    float s = std::sin(theta), c = std::cos(theta), ic = 1.0f - c;
    m[0] = c + ax * ax * ic; m[1] = ay * ax * ic;     m[2] = -ay * s;
    m[3] = ax * ay * ic;     m[4] = c + ay * ay * ic; m[5] = ax * s;
    m[6] = ay * s;           m[7] = -ax * s;          m[8] = c;
}

// Engine glue (M2TreeSway.cpp): the kind of a placed model (None when not a swaying doodad), and its
// world transform with the sway applied (m_worldTransform unchanged when it does not sway).
Kind KindOf(const CM2Model* model);
C44Matrix World(const CM2Model* model);

} // namespace M2TreeSway

#endif
