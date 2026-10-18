// TREELEAVES: not in 12340. See TreeLeaves.h; port of wxl-treewind Leaves.cpp (branch leaves). The CPU uploads
// four constants per tree and issues one draw from a static buffer; TreeLeavesVS does the rest.
#include "WoW/Source/WorldClient/TreeLeaves.h"

#include <cmath>
#include <cstring>
#include <mutex>

#include "Engine/Source/BLPFile/blp.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/World.h"

namespace TreeLeaves {

int32_t s_enabled = 1;
float s_perTree = 18.0f;

namespace {

// wxl-treewind leaves::Settings defaults (only PerTree is a CVar).
constexpr float kRange = 70.0f;      // yards from the camera; leaves fade out toward it
constexpr float kSize = 0.16f;       // leaf half-size, yards
constexpr float kFallSpeed = 1.3f;   // yards per second
constexpr float kRate = 0.55f;       // share of leaf slots in use at full gust
constexpr float kCalm = 0.15f;       // share of kRate that still falls with no gust
constexpr float kDrift = 0.45f;      // how much of the tree wind speed carries a leaf sideways

constexpr float kTwoPi = 6.28318530717959f;
constexpr uint32_t kMaxTrees = 32, kMaxPerTree = 48, kVerts = kMaxTrees * kMaxPerTree * 6;
// A tree once in the table keeps shedding while out of view; one not noted for kHoldMs is gone and fades over kFadeMs.
constexpr uint32_t kHoldMs = 60000, kFadeMs = 1500;
constexpr uint32_t kMaxTex = 4, kNameLen = 128;

struct Tree {
    float x, y, z;              // trunk base
    float crownZ, crownR, top;  // crown centre above the base, radius, full height (world yards)
    float seed, gust, wx, wy, wspeed;
    float r, g, b;              // leaf colour
    float shed;                 // 0 = never (palms), 1 = normal, < 1 = sparse (conifers)
    char tex[kMaxTex][kNameLen];  // the model's file textures, read for the colour on the main thread
    uint32_t texCount;
    bool tinted;
    uint32_t stamp, born;
    bool used;
};

Tree s_trees[kMaxTrees];
std::mutex s_lock;

bool Has(const char* hay, const char* needle) {
    for (int i = 0; hay[i] && i < 256; ++i) {
        int n = 0;
        while (needle[n] && hay[i + n] && (hay[i + n] | 0x20) == needle[n]) ++n;
        if (!needle[n]) return true;
    }
    return false;
}

// Whether a tree sheds, and how much, from the model's name; the colour stands until LeafColour has read its texture.
void Classify(const char* path, Tree& t) {
    t.shed = 1.0f; t.r = 0.38f; t.g = 0.50f; t.b = 0.17f;   // summer green
    if (Has(path, "palm") || Has(path, "mushroom") || Has(path, "cactus")) { t.shed = 0.0f; return; }
    if (Has(path, "pine") || Has(path, "spruce") || Has(path, "cedar") || Has(path, "conifer") || Has(path, "fir0") ||
        Has(path, "larch") || Has(path, "juniper") || Has(path, "cypress") || Has(path, "redwood"))
    { t.shed = 0.25f; t.r = 0.24f; t.g = 0.33f; t.b = 0.14f; return; }   // a few dark needles
    if (Has(path, "autumn") || Has(path, "fall") || Has(path, "aspen") || Has(path, "maple") || Has(path, "birch") ||
        Has(path, "orange") || Has(path, "azshara") || Has(path, "grizzly") || Has(path, "eversong") || Has(path, "silvermoon"))
    { t.r = 0.80f; t.g = 0.48f; t.b = 0.12f; return; }   // autumn
    if (Has(path, "red") || Has(path, "blood")) { t.r = 0.66f; t.g = 0.20f; t.b = 0.12f; return; }
    if (Has(path, "yellow") || Has(path, "gold")) { t.r = 0.82f; t.g = 0.68f; t.b = 0.16f; return; }
    if (Has(path, "snow") || Has(path, "winter") || Has(path, "frost")) { t.shed = 0.2f; t.r = 0.80f; t.g = 0.84f; t.b = 0.88f; return; }
    if (Has(path, "teldrassil") || Has(path, "ashenvale") || Has(path, "kalidar") || Has(path, "purple"))
    { t.r = 0.45f; t.g = 0.30f; t.b = 0.55f; return; }   // night elf violet
}

// The average colour of a BLP's opaque texels, from a small mip (the plugin locked the D3D9 texture; the file is
// the same pixels on every backend). False when it cannot be read.
bool TextureColour(const char* name, float rgb[3]) {
    CBLPFile blp;
    if (!blp.Open(name, 1)) return false;
    const uint32_t levels = blp.m_numLevels;
    const uint32_t level = levels > 3 ? levels - 3 : 0;   // 4 x 4 on a full chain
    uint8_t* bits = nullptr;
    uint32_t stride = 0;
    bool ok = false;
    if (blp.Lock(PIXEL_ARGB8888, level, bits, stride) && bits) {
        const uint32_t w = blp.Width(level), h = blp.Height(level), pitch = 4 * w;
        float sum[3] = {}, n = 0.0f;
        for (uint32_t y = 0; y < h && y < 8; ++y)
            for (uint32_t x = 0; x < w && x < 8; ++x) {
                const uint8_t* px = bits + y * pitch + x * 4;   // B, G, R, A
                if (px[3] < 128) continue;
                sum[0] += px[2] / 255.0f; sum[1] += px[1] / 255.0f; sum[2] += px[0] / 255.0f; n += 1.0f;
            }
        if (n >= 1.0f) {
            for (int i = 0; i < 3; ++i) rgb[i] = sum[i] / n;
            ok = true;
        }
    }
    blp.Unlock(level);
    blp.Close();
    return ok;
}

// Of the tree's textures, the one named for leaves, or failing a name the most colourful (bark is grey-brown).
bool LeafColour(const Tree& t, float rgb[3]) {
    float best = -1e9f;
    for (uint32_t i = 0; i < t.texCount; ++i) {
        float c[3];
        const char* name = t.tex[i];
        if (!TextureColour(name, c)) continue;
        const float hi = std::fmax(c[0], std::fmax(c[1], c[2])), lo = std::fmin(c[0], std::fmin(c[1], c[2]));
        float score = hi > 0.01f ? (hi - lo) / hi : 0.0f;
        if (Has(name, "leaf") || Has(name, "leaves") || Has(name, "lvs") || Has(name, "branch") || Has(name, "needle") ||
            Has(name, "canopy") || Has(name, "frond") || Has(name, "foliage")) score += 10.0f;
        if (Has(name, "bark") || Has(name, "trunk")) score -= 10.0f;
        if (score <= best) continue;
        best = score;
        const float lift = hi < 0.25f && hi > 0.01f ? 0.25f / hi : 1.0f;   // a near-black texture still gives a leaf that shows
        for (int k = 0; k < 3; ++k) rgb[k] = c[k] * lift;
    }
    return best > -1e8f;
}

uint32_t Mix(uint32_t h) { h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16; return h; }
float U01(uint32_t h) { return float(Mix(h) & 0xFFFFFFu) / 16777216.0f; }

// GxVBF_PNCT2. The plugin's (corner xy, tree, 1 | four slot randoms | four more) as P = (x, y, tree),
// N + T0.x = the first four, T0.y + T1 + colour = the rest (TreeLeavesVS repacks them).
struct Vertex {
    float p[3], n[3];
    uint8_t c[4];
    float t0[2], t1[2];
};
static_assert(sizeof(Vertex) == 44, "GxVBF_PNCT2");

CGxShader* s_vs = nullptr;
CGxShader* s_ps = nullptr;
CGxTex* s_tex = nullptr;
CGxPool* s_pool = nullptr;
CGxBuf* s_buf = nullptr;
bool s_failed = false;
uint32_t s_leafTexels[32 * 32];

// A leaf silhouette (pointed ellipse with a midrib), white, alpha = coverage.
void LeafTexCallback(EGxTexCommand command, uint32_t, uint32_t, uint32_t, uint32_t, void*, uint32_t& stride, const void*& texels) {
    if (command == GxTex_Latch) {
        texels = s_leafTexels;
        stride = 32 * 4;
    }
}

void BuildLeafTexels() {
    constexpr int N = 32;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float u = (x + 0.5f) / N * 2 - 1, v = (y + 0.5f) / N * 2 - 1;   // -1..1
            const float half = 0.52f * (1.0f - v * v) * (1.0f - 0.25f * v);      // fatter at one end
            float a = half > 0.0f ? (half - std::fabs(u)) * 9.0f : 0.0f;
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            const float rib = 1.0f - 0.22f * (std::fabs(u) < 0.05f ? 1.0f : 0.0f);
            const float shade = (0.82f + 0.18f * (u > 0 ? 1.0f : 0.0f)) * rib;  // one half a touch darker
            const uint32_t c = uint32_t(shade * 255.0f);
            s_leafTexels[y * N + x] = (uint32_t(a * 255.0f) << 24) | (c << 16) | (c << 8) | c;
        }
}

// Slot-major: the first perTree * kMaxTrees quads are every tree's first perTree slots, so one draw covers any density.
void FillBuffer() {
    auto* v = reinterpret_cast<Vertex*>(g_theGxDevicePtr->BufLock(s_buf));
    if (!v) return;
    static const float cx[6] = { -1, 1, 1, -1, 1, -1 }, cy[6] = { -1, -1, 1, -1, 1, 1 };
    for (uint32_t slot = 0; slot < kMaxPerTree; ++slot)
        for (uint32_t tree = 0; tree < kMaxTrees; ++tree) {
            const uint32_t ls = slot * 0x9E3779B9u + 12345u;   // per slot, not per row: see the shader
            float r[8];
            for (int k = 0; k < 4; ++k) { r[k] = U01(ls + 1 + k); r[4 + k] = U01(ls + 5 + k); }
            const uint8_t tone = static_cast<uint8_t>(r[7] * 255.0f + 0.5f);
            for (int k = 0; k < 6; ++k, ++v) {
                v->p[0] = cx[k]; v->p[1] = cy[k]; v->p[2] = float(tree);
                v->n[0] = r[0]; v->n[1] = r[1]; v->n[2] = r[2];
                v->c[0] = v->c[1] = v->c[2] = v->c[3] = tone;
                v->t0[0] = r[3]; v->t0[1] = r[4];
                v->t1[0] = r[5]; v->t1[1] = r[6];
            }
        }
    g_theGxDevicePtr->BufUnlock(s_buf, 0);
    s_buf->m_hasData = 1;
}

bool Gpu() {
    if (s_failed) return false;
    if (!s_vs) {
        g_theGxDevicePtr->ShaderCreate(&s_vs, GxSh_Vertex, "Shaders\\Vertex", "TreeLeavesVS", 1);
        g_theGxDevicePtr->ShaderCreate(&s_ps, GxSh_Pixel, "Shaders\\Pixel", "TreeLeavesPS", 1);
        BuildLeafTexels();
        CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 0, 1, 0, 0, 0);
        g_theGxDevicePtr->TexCreate(GxTex_2d, 32, 32, 0, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, LeafTexCallback, "TreeLeaves", s_tex);
        s_pool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Vertex, GxPoolUsage_Static, kVerts * sizeof(Vertex), GxPoolHintBit_Unk0, "TreeLeaves");
        s_buf = s_pool ? g_theGxDevicePtr->BufCreate(s_pool, sizeof(Vertex), kVerts, 0) : nullptr;
    }
    if (!s_vs || !s_vs->Valid() || !s_ps || !s_ps->Valid() || !s_tex || !s_buf) {
        s_failed = true;   // no TreeLeaves shaders in custom.MPQ: no falling leaves
        return false;
    }
    if (!(s_buf->m_hasData && s_buf->m_valid)) FillBuffer();
    return s_buf->m_hasData != 0;
}

void Rgb(const CImVector& c, float out[3]) {
    out[0] = c.r / 255.0f; out[1] = c.g / 255.0f; out[2] = c.b / 255.0f;
}

} // namespace

void NoteTree(const CM2Model* model, const float* p, float seed, float gust, float windX, float windY, float windSpeed, uint32_t now) {
    if (!s_enabled) return;
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float dx = p[12] - cam.x, dy = p[13] - cam.y;
    const float d2 = dx * dx + dy * dy;
    const float reach = kRange + 20.0f;
    if (d2 > reach * reach) return;

    const CM2Shared* shared = model->m_shared;
    const M2Data* data = shared->m_data;
    const CAaBox& box = data->m_bounds.m_extent;
    const float scale = std::sqrt(p[8] * p[8] + p[9] * p[9] + p[10] * p[10]);   // the placement's up row
    const float ez = (box.t.z - box.b.z) * scale;
    if (!(ez > 3.0f)) return;   // saplings shed nothing worth drawing
    const float ex = (box.t.x - box.b.x) * scale, ey = (box.t.y - box.b.y) * scale;

    std::lock_guard<std::mutex> lock(s_lock);
    Tree* slot = nullptr;
    Tree* worst = nullptr;
    float worstD = d2 * 1.7f;   // full table: only a tree a good 30 % farther away gives up its row (no flip-flop)
    for (Tree& t : s_trees) {
        if (t.used && t.x == p[12] && t.y == p[13]) { slot = &t; break; }
        if (!t.used || now - t.stamp > kHoldMs + kFadeMs) { if (!slot) slot = &t; continue; }
        const float tx = t.x - cam.x, ty = t.y - cam.y;
        const float td = tx * tx + ty * ty;
        if (td > worstD) { worstD = td; worst = &t; }
    }
    if (!slot) slot = worst;
    if (!slot) return;
    if (!slot->used || slot->x != p[12] || slot->y != p[13]) {
        slot->x = p[12]; slot->y = p[13]; slot->z = p[14];
        slot->top = ez;
        slot->crownZ = (box.b.z * scale > 0 ? box.b.z * scale : 0.0f) + ez * 0.68f;
        slot->crownR = 0.36f * (ex > ey ? ex : ey);
        Classify(shared->m_fileName, *slot);
        slot->texCount = 0;
        for (uint32_t i = 0; i < data->m_textures.m_count && slot->texCount < kMaxTex; ++i) {
            const M2Texture& tex = data->m_textures[i];
            if (tex.m_type != 0 || tex.m_filename.m_count < 2) continue;   // only textures named in the file
            std::strncpy(slot->tex[slot->texCount], tex.m_filename.Data(), kNameLen - 1);
            slot->tex[slot->texCount++][kNameLen - 1] = '\0';
        }
        slot->tinted = false;
        slot->used = true;
        slot->born = now;
    }
    slot->seed = seed;
    slot->gust = gust > 0.05f ? 1.0f : 0.5f;
    slot->wx = windX; slot->wy = windY; slot->wspeed = windSpeed;
    slot->stamp = now;
}

void Draw() {
    CM2Scene* scene = World::s_m2Scene;
    if (!s_enabled || !scene || !g_theGxDevicePtr) return;
    const uint32_t now = scene->m_time;

    // Per tree: (x,y,z,seed) (crownZ,crownR,top,shed) (windX,windY,speed,gust) (r,g,b,fade). Unused rows stay
    // zero: shed 0, so their slots never draw.
    static float tc[kMaxTrees * 16];
    std::memset(tc, 0, sizeof(tc));
    uint32_t nTrees = 0;
    {
        std::lock_guard<std::mutex> lock(s_lock);
        for (Tree& t : s_trees) {
            if (!t.used) continue;
            const uint32_t idle = now - t.stamp;
            if (idle > kHoldMs + kFadeMs && idle < 0x80000000u) { t.used = false; continue; }
            if (t.shed <= 0.0f) continue;
            if (!t.tinted) {   // once, on the main thread (the BLP reader's buffer is not shared with the workers)
                float c[3];
                if (LeafColour(t, c)) { t.r = c[0]; t.g = c[1]; t.b = c[2]; }
                t.tinted = true;
            }
            float fade = float(now - t.born) / float(kFadeMs);
            if (!(fade < 1.0f) || now - t.born > 0x80000000u) fade = 1.0f;
            if (idle > kHoldMs && idle < 0x80000000u) fade *= 1.0f - float(idle - kHoldMs) / float(kFadeMs);
            float* c = tc + nTrees++ * 16;
            c[0] = t.x; c[1] = t.y; c[2] = t.z; c[3] = t.seed * (1.0f / kTwoPi);
            c[4] = t.crownZ; c[5] = t.crownR; c[6] = t.top; c[7] = t.shed;
            c[8] = t.wx; c[9] = t.wy; c[10] = t.wspeed; c[11] = t.gust;
            c[12] = t.r; c[13] = t.g; c[14] = t.b; c[15] = fade;
        }
    }
    if (!nTrees || !Gpu()) return;

    // clip = (world - camera) * view * projection (row vectors): the Gx view is camera relative here
    // (CWorldScene::Update), and the device's native projection takes the world viewport's depth range itself.
    C44Matrix vp;
    GxXformViewProjNativeTranspose(vp);
    vp = vp.Transpose();
    const float* M = reinterpret_cast<const float*>(&vp);
    float k[10][4] = {};
    std::memcpy(k, M, sizeof(float) * 16);
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    k[4][0] = cam.x; k[4][1] = cam.y; k[4][2] = cam.z;
    k[4][3] = static_cast<float>(now % 3600000u) * 0.001f;   // M2TreeSway's clock: the same gust phase
    k[5][0] = kSize; k[5][1] = kFallSpeed; k[5][2] = kRate; k[5][3] = kCalm;
    k[6][0] = kDrift; k[6][1] = kRange;
    const C44Matrix& proj = g_theGxDevicePtr->m_projNative;
    const DayNight::DNInfo& dn = DayNight::g_dnInfo;
    const float fogStart = dn.m_fog.m_start, fogEnd = dn.m_fog.m_end;   // vfog pushes these out: its composite fogs the leaves
    k[7][0] = std::fabs(proj.a0); k[7][1] = std::fabs(proj.b1); k[7][2] = fogStart;
    k[7][3] = fogEnd > fogStart + 0.01f ? 1.0f / (fogEnd - fogStart) : 0.0f;
    float amb[3], dir[3];
    Rgb(dn.m_bands.m_ambient, amb);
    Rgb(dn.m_bands.m_diffuse, dir);
    for (int i = 0; i < 3; ++i) k[8][i] = std::fmin(amb[i] + 0.7f * dir[i], 1.0f);
    Rgb(dn.m_fog.m_color, k[9]);

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_VertexShader, s_vs);
    GxRsSet(GxRs_PixelShader, s_ps);
    GxRsSet(GxRs_Texture0, s_tex);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);   // the plugin's alpha test is a clip in TreeLeavesPS
    GxRsSet(GxRs_AlphaRef, 0);
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthFunc, 0);   // LESSEQUAL
    GxRsSet(GxRs_DepthWrite, 1);
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_StencilTest, 0);
    GxRsSet(GxRs_ScissorTest, 0);
    GxRsSet(GxRs_ColorWrite, 7);   // rgb: leave the target's alpha alone
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, &k[0][0], 10);
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 10, tc, kMaxTrees * 4);
    GxPrimVertexPtr(s_buf, GxVBF_PNCT2);
    const uint32_t perTree = static_cast<uint32_t>(std::fmin(std::fmax(s_perTree, 1.0f), float(kMaxPerTree)));
    CGxBatch batch;
    batch.m_primType = GxPrim_Triangles;
    batch.m_start = 0;
    batch.m_count = perTree * kMaxTrees * 6;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 0;
    g_theGxDevicePtr->Draw(&batch, 0);
    g_theGxDevicePtr->RsPop();
}

} // namespace TreeLeaves
