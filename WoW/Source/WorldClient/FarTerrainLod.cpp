#include "FarTerrainLod.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "Engine/Source/Tempest/CRect.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapLowDetail.h"
#include "WoW/Source/WorldClient/World.h"

// Not in 12340 (farTerrainLod): the far terrain's detailed levels. docs/FARFIELD_FOG.md, "Detailed levels".
//
// wxl-farfield (FarTerrain.cpp, "detail: the baked level") draws the land just past the view distance from
// the real terrain's heights, every other ADT corner (8.3 yd), as a 65 x 65 grid a tile with its four edges
// hung 25 yd down as skirts over any crack to the WDL, the ADT's holes left out, and leaves those tiles out
// of the WDL pass. wxl-wraithhost bakes those heights into <map>.farlod; neither loads in Wow32/Wow64, so
// here the heights come from the ADTs the WDL ground bake already reads (MapLowDetail.cpp PumpBake), and
// there are three levels instead of one, picked per tile by screen-space error:
//
//   level 0: every ADT corner, 129 x 129 (4.17 yd)     level 2: every 4th, 33 x 33 (16.7 yd)
//   level 1: every 2nd, 65 x 65 (8.3 yd, farfield's)   level 3: the WDL itself (33.3 yd, MapLowDetail.cpp)
//
// Each level's error is measured once, when the tile is read: the largest height difference between that
// level's surface and the real terrain's (all 145 MCVT vertices a chunk, the inner ones too). A frame takes
// the coarsest level whose error, projected to pixels at the tile's nearest point, stays under
// farTerrainLodPixels. Tiles the real terrain reaches into (nearer than farclip + a chunk) always take level
// 0: its vertices and MCNR normals are the real terrain's, and the real terrain's chunk edges run along its
// quad edges, so the two meet with no step. Vertices are PN in world space, as the WDL's, so the same
// shaders, constants, atlas and fog draw both; one draw a detailed tile.
namespace {

constexpr float kOrigin = 17066.666f;
constexpr float kTileYd = 533.33331f;
constexpr uint32_t kGrid = 129;
constexpr float kStep = kTileYd / 128.0f;
constexpr float kSkirt = 25.0f;          // farfield's: yards a tile's edges hang down
constexpr uint32_t kBuildsPerFrame = 2;  // ponytail: a count, not a timed budget (a level-0 build is ~0.5 ms)
constexpr uint32_t kIdleFrames = 120;    // a level not drawn for this long may be evicted for another

enum : uint8_t { Lod_None, Lod_Reading, Lod_Ready, Lod_Failed };

struct Level {
    CGxPool* vertexPool = nullptr;
    CGxPool* indexPool = nullptr;  // own, when the tile has holes; else the level's shared buffer
    CGxBuf* vertexBuf = nullptr;
    CGxBuf* indexBuf = nullptr;
    uint32_t verts = 0;
    uint32_t indices = 0;
    uint32_t bytes = 0;
    uint32_t used = 0;
};

struct Tile {
    float h[kGrid * kGrid];
    int8_t n[kGrid * kGrid * 3];  // MCNR, as the real terrain's vertices take it (x 1/127)
    uint16_t holes[256];
    bool anyHole;
    float err[4];                 // yards: levels 0-2 against the real terrain, 3 the WDL
    float lo, hi;
    Level lv[3];
};

struct Shared {
    CGxPool* pool = nullptr;
    CGxBuf* buf = nullptr;
    uint32_t indices = 0;
};

struct Drawn {
    float d;
    uint32_t t;
    uint32_t level;
};

struct Want {
    float d;
    uint32_t t;
    uint32_t level;
};

struct LodState {
    uint8_t state[4096] = {};
    Tile* tiles[4096] = {};
    std::vector<uint32_t> loaded;
    Shared shared[3];
    std::vector<Drawn> drawn[2];
    std::vector<Want> want;
    uint32_t frame = 0;
    size_t bytes = 0;
    // this frame, for farTerrainLodStats
    uint32_t draws = 0;
    uint32_t perLevel[4] = {};
};

LodState s_lod;

CVar* LodCVar() {
    static CVar* s_cvar = CVar::Register("farTerrainLod", "Engine option: draw the far terrain near the view distance from the real ADT heights (full, half or quarter resolution, by screen-space error) instead of the coarse WDL (0: WDL only)", 0x1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* PixelsCVar() {
    static CVar* s_cvar = CVar::Register("farTerrainLodPixels", "Engine option: the far terrain's largest allowed height error, in pixels on screen; a tile takes the coarsest level under it", 0x1, "1.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* MemoryCVar() {
    static CVar* s_cvar = CVar::Register("farTerrainLodMemory", "Engine option: megabytes the far terrain's detailed levels may hold (heights and buffers)", 0x1, "64", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* StatsCVar() {
    static CVar* s_cvar = CVar::Register("farTerrainLodStats", "Engine option: print the far terrain's level counts, draws and memory to the console every 10 seconds", 0x1, "0", nullptr, CATEGORY_DEBUG, false, nullptr, false);
    return s_cvar;
}

bool On() {
    return LodCVar()->m_intValue != 0;
}

size_t Cap() {
    return static_cast<size_t>(std::max(MemoryCVar()->m_intValue, 0)) << 20;
}

uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

float RdF(const uint8_t* p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}

// A sub-chunk of an MCNK by its header offset (relative to the MCNK's own 8-byte header).
const uint8_t* Sub(const uint8_t* mcnk, uint32_t len, uint32_t ofs, uint32_t tag, uint32_t need) {
    if (ofs < 8 || ofs + 8ull > len || Rd32(mcnk + ofs) != tag) {
        return nullptr;
    }

    const uint32_t size = Rd32(mcnk + ofs + 4);
    return size >= need && ofs + 8ull + need <= len ? mcnk + ofs + 8 : nullptr;
}

// Height of a triangle's plane at (px, py).
float Tri(float px, float py, float ax, float ay, float ha, float bx, float by, float hb, float cx, float cy, float hc) {
    const float d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    const float wa = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d;
    const float wb = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d;
    return wa * ha + wb * hb + (1.0f - wa - wb) * hc;
}

// Level L's surface at (fr, fc) in level-0 grid units (row along -x, column along -y): two triangles a quad,
// split a -> g as WriteIndices splits them.
float EvalGrid(const float* h, uint32_t level, float fr, float fc) {
    const uint32_t s = 1u << level;
    const int32_t q = static_cast<int32_t>(128 >> level);
    const int32_t r0 = std::min(static_cast<int32_t>(fr / s), q - 1);
    const int32_t c0 = std::min(static_cast<int32_t>(fc / s), q - 1);
    const float u = fr / s - r0;
    const float v = fc / s - c0;
    auto at = [&](int32_t r, int32_t c) { return h[(r * s) * kGrid + c * s]; };
    const float a = at(r0, c0), b = at(r0, c0 + 1), e = at(r0 + 1, c0), g = at(r0 + 1, c0 + 1);
    return v >= u ? a + v * (b - a) + u * (g - b) : a + u * (e - a) + v * (g - e);
}

// The WDL's surface at the same point: 17 x 17 + 16 x 16, four triangles a cell round its inner vertex
// (12340's pattern, MapLowDetail.cpp WriteGridIndices).
float EvalWdl(const int16_t* w, float fr, float fc) {
    const int32_t r = std::min(static_cast<int32_t>(fr / 8.0f), 15);
    const int32_t c = std::min(static_cast<int32_t>(fc / 8.0f), 15);
    const float u = fr / 8.0f - r;
    const float v = fc / 8.0f - c;
    const float a = w[17 * r + c], b = w[17 * r + c + 1], e = w[17 * (r + 1) + c], g = w[17 * (r + 1) + c + 1];
    const float m = w[289 + 16 * r + c];
    const float du = u - 0.5f, dv = v - 0.5f;

    if (fabsf(du) >= fabsf(dv)) {
        return du < 0.0f ? Tri(u, v, 0, 0, a, 0, 1, b, 0.5f, 0.5f, m) : Tri(u, v, 1, 0, e, 1, 1, g, 0.5f, 0.5f, m);
    }

    return dv < 0.0f ? Tri(u, v, 0, 0, a, 1, 0, e, 0.5f, 0.5f, m) : Tri(u, v, 0, 1, b, 1, 1, g, 0.5f, 0.5f, m);
}

// The ADT's heights, normals and holes; false when a chunk lacks them (the tile stays on the WDL).
bool Extract(const uint8_t* b, uint32_t n, Tile& tile, float* inner) {
    const uint32_t kMCNK = 0x4D434E4B;
    const uint32_t kMCVT = 0x4D435654;
    const uint32_t kMCNR = 0x4D434E52;
    bool got[256] = {};
    uint32_t seen = 0;

    for (uint32_t o = 0; o + 8 <= n;) {
        const uint32_t magic = Rd32(b + o);
        const uint32_t size = Rd32(b + o + 4);

        if (o + 8ull + size > n) {
            break;
        }

        if (magic == kMCNK && size >= 0x80) {
            const uint8_t* c = b + o;
            const uint8_t* h = c + 8;
            const uint32_t len = size + 8;
            uint32_t ix = Rd32(h + 4);
            uint32_t iy = Rd32(h + 8);

            if (ix > 15 || iy > 15) {
                ix = seen % 16;
                iy = seen / 16;
            }

            seen++;
            const uint8_t* mcvt = Sub(c, len, Rd32(h + 0x14), kMCVT, 145 * 4);
            const uint8_t* mcnr = Sub(c, len, Rd32(h + 0x18), kMCNR, 145 * 3);

            if (mcvt && mcnr && !got[iy * 16 + ix]) {
                got[iy * 16 + ix] = true;
                const float z = RdF(h + 0x70);

                for (uint32_t r = 0; r < 9; r++) {
                    for (uint32_t k = 0; k < 9; k++) {
                        const uint32_t g = (iy * 8 + r) * kGrid + ix * 8 + k;
                        tile.h[g] = z + RdF(mcvt + (r * 17 + k) * 4);
                        memcpy(tile.n + g * 3, mcnr + (r * 17 + k) * 3, 3);
                    }

                    for (uint32_t k = 0; r < 8 && k < 8; k++) {
                        inner[(iy * 8 + r) * 128 + ix * 8 + k] = z + RdF(mcvt + (r * 17 + 9 + k) * 4);
                    }
                }

                tile.holes[iy * 16 + ix] = static_cast<uint16_t>(Rd32(h + 0x3C) & 0xFFFF);
            }
        }

        o += 8 + size;
    }

    for (bool g : got) {
        if (!g) {
            return false;
        }
    }

    return true;
}

// Each level's largest height error against the real terrain (every MCVT vertex, outer and inner).
void MeasureErrors(Tile& tile, const float* inner, const int16_t* wdl) {
    float err[4] = {};

    for (uint32_t r = 0; r < kGrid; r++) {
        for (uint32_t c = 0; c < kGrid; c++) {
            const float h = tile.h[r * kGrid + c];

            for (uint32_t l = 1; l < 3; l++) {
                err[l] = std::max(err[l], fabsf(EvalGrid(tile.h, l, r, c) - h));
            }

            err[3] = std::max(err[3], wdl ? fabsf(EvalWdl(wdl, r, c) - h) : 1e9f);

            if (r < 128 && c < 128) {
                const float m = inner[r * 128 + c];
                const float fr = r + 0.5f, fc = c + 0.5f;

                for (uint32_t l = 0; l < 3; l++) {
                    err[l] = std::max(err[l], fabsf(EvalGrid(tile.h, l, fr, fc) - m));
                }

                err[3] = std::max(err[3], wdl ? fabsf(EvalWdl(wdl, fr, fc) - m) : 1e9f);
            }
        }
    }

    // Each level's surface contains the next finer one's error budget: never let a coarser level claim less.
    for (uint32_t l = 1; l < 3; l++) {
        err[l] = std::max(err[l], err[l - 1]);
    }

    memcpy(tile.err, err, sizeof(err));
}

uint16_t EdgeVertex(uint32_t side, uint32_t k, uint32_t n) {
    return static_cast<uint16_t>(side == 0 ? k : side == 1 ? (n - 1) * n + k : side == 2 ? k * n : k * n + n - 1);
}

// A level-L quad is left out only when every ADT quad under it is a hole (MCNK holes: a bit per 2 x 2 quads,
// CMapChunk::CreateIndices). Levels 0 and 1 keep the holes exactly; level 2 drops only whole 2 x 2 runs.
bool AllHoles(const uint16_t* holes, uint32_t r0, uint32_t c0, uint32_t s) {
    for (uint32_t r = r0; r < r0 + s; r++) {
        for (uint32_t c = c0; c < c0 + s; c++) {
            if (!((holes[(r / 8) * 16 + c / 8] >> (((r % 8) / 2) * 4 + (c % 8) / 2)) & 1)) {
                return false;
            }
        }
    }

    return true;
}

// Two triangles a quad (a b g, a g e), less the holes, then each edge's skirt (as farfield's MakeIndices).
void WriteIndices(std::vector<uint16_t>& ix, uint32_t level, const uint16_t* holes) {
    const uint32_t n = (128 >> level) + 1;
    const uint32_t s = 1u << level;
    ix.clear();

    for (uint32_t r = 0; r + 1 < n; r++) {
        for (uint32_t c = 0; c + 1 < n; c++) {
            if (holes && AllHoles(holes, r * s, c * s, s)) {
                continue;
            }

            const uint16_t a = static_cast<uint16_t>(r * n + c), b = a + 1, e = static_cast<uint16_t>(a + n), g = e + 1;
            const uint16_t t[6] = { a, b, g, a, g, e };
            ix.insert(ix.end(), t, t + 6);
        }
    }

    for (uint32_t side = 0; side < 4; side++) {
        for (uint32_t k = 0; k + 1 < n; k++) {
            const uint16_t top0 = EdgeVertex(side, k, n), top1 = EdgeVertex(side, k + 1, n);
            const uint16_t low0 = static_cast<uint16_t>(n * n + side * n + k), low1 = low0 + 1;
            const uint16_t t[6] = { top0, top1, low1, top0, low1, low0 };
            ix.insert(ix.end(), t, t + 6);
        }
    }
}

bool Valid(const CGxBuf* buf) {
    return buf && buf->m_hasData && buf->m_valid;
}

bool UploadIndices(CGxPool*& pool, CGxBuf*& buf, const std::vector<uint16_t>& ix) {
    if (!buf) {
        pool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Index, GxPoolUsage_Static, static_cast<int32_t>(ix.size() * 2), GxPoolHintBit_Unk0, "FarTerrainLodIB");
        buf = pool ? g_theGxDevicePtr->BufCreate(pool, sizeof(uint16_t), static_cast<uint32_t>(ix.size()), 0) : nullptr;
    }

    char* dst = buf ? g_theGxDevicePtr->BufLock(buf) : nullptr;

    if (!dst) {
        return false;
    }

    memcpy(dst, ix.data(), ix.size() * 2);
    g_theGxDevicePtr->BufUnlock(buf, 0);
    buf->m_hasData = 1;
    return true;
}

void FreeBuf(CGxPool*& pool, CGxBuf*& buf) {
    if (buf) {
        g_theGxDevicePtr->BufDestroy(buf);
    }

    if (pool) {
        g_theGxDevicePtr->PoolDestroy(pool);
    }

    pool = nullptr;
    buf = nullptr;
}

void FreeLevel(Level& lv) {
    FreeBuf(lv.vertexPool, lv.vertexBuf);

    if (lv.indexPool) {
        FreeBuf(lv.indexPool, lv.indexBuf);
    }

    s_lod.bytes -= lv.bytes;
    lv = Level {};
}

bool Built(const Tile& tile, uint32_t level) {
    const Level& lv = tile.lv[level];
    return Valid(lv.vertexBuf) && Valid(lv.indexBuf);
}

// Frees the least recently drawn levels that have been idle a while until `need` more bytes fit.
bool MakeRoom(size_t need) {
    while (s_lod.bytes + need > Cap()) {
        Level* oldest = nullptr;

        for (uint32_t t : s_lod.loaded) {
            for (Level& lv : s_lod.tiles[t]->lv) {
                if (lv.vertexBuf && lv.used + kIdleFrames < s_lod.frame && (!oldest || lv.used < oldest->used)) {
                    oldest = &lv;
                }
            }
        }

        if (!oldest) {
            return false;
        }

        FreeLevel(*oldest);
    }

    return true;
}

// Level L of tile t into its static buffers (or refilled, after the device dropped them).
bool Build(uint32_t t, uint32_t level) {
    Tile& tile = *s_lod.tiles[t];
    Level& lv = tile.lv[level];
    const uint32_t n = (128 >> level) + 1;
    const uint32_t s = 1u << level;
    const uint32_t verts = n * n + 4 * n;
    static std::vector<uint16_t> s_ix;

    if (!lv.vertexBuf) {
        WriteIndices(s_ix, level, tile.anyHole ? tile.holes : nullptr);
        const size_t bytes = verts * sizeof(CGxVertexPN) + (tile.anyHole ? s_ix.size() * 2 : 0);

        if (!MakeRoom(bytes)) {
            return false;
        }

        lv.verts = verts;
        lv.indices = static_cast<uint32_t>(s_ix.size());
        lv.bytes = static_cast<uint32_t>(bytes);
        s_lod.bytes += bytes;
        lv.vertexPool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Vertex, GxPoolUsage_Static, static_cast<int32_t>(verts * sizeof(CGxVertexPN)), GxPoolHintBit_Unk0, "FarTerrainLodVB");
        lv.vertexBuf = lv.vertexPool ? g_theGxDevicePtr->BufCreate(lv.vertexPool, sizeof(CGxVertexPN), verts, 0) : nullptr;

        if (!lv.vertexBuf) {
            FreeLevel(lv);
            return false;
        }
    }

    if (!Valid(lv.vertexBuf)) {
        CGxVertexPN* v = reinterpret_cast<CGxVertexPN*>(g_theGxDevicePtr->BufLock(lv.vertexBuf));

        if (!v) {
            return false;
        }

        const uint32_t row = t / 64, col = t % 64;
        const float x0 = kOrigin - row * kTileYd, y0 = kOrigin - col * kTileYd;
        // Skirts hang farfield's 25 yd, or twice the WDL's error where that is more: a neighbour drawn
        // coarser can stand that far off this tile's edge.
        const float skirt = std::max(kSkirt, std::min(2.0f * tile.err[3], 500.0f));

        for (uint32_t k = 0; k < verts; k++) {
            const uint32_t g = k < n * n ? k : EdgeVertex((k - n * n) / n, (k - n * n) % n, n);
            const uint32_t r = (g / n) * s, c = (g % n) * s;
            const int8_t* nn = tile.n + (r * kGrid + c) * 3;
            v[k].m_position = C3Vector(x0 - r * kStep, y0 - c * kStep, tile.h[r * kGrid + c] - (k < n * n ? 0.0f : skirt));
            v[k].m_normal = C3Vector(nn[0] * 0.0078740157f, nn[1] * 0.0078740157f, nn[2] * 0.0078740157f);
        }

        g_theGxDevicePtr->BufUnlock(lv.vertexBuf, 0);
        lv.vertexBuf->m_hasData = 1;
    }

    if (tile.anyHole) {
        if (!Valid(lv.indexBuf)) {
            WriteIndices(s_ix, level, tile.holes);
            return UploadIndices(lv.indexPool, lv.indexBuf, s_ix);
        }

        return true;
    }

    Shared& sh = s_lod.shared[level];

    if (!Valid(sh.buf)) {
        WriteIndices(s_ix, level, nullptr);
        const bool fresh = !sh.buf;

        if (!UploadIndices(sh.pool, sh.buf, s_ix)) {
            return false;
        }

        if (fresh) {
            sh.indices = static_cast<uint32_t>(s_ix.size());
            s_lod.bytes += s_ix.size() * 2;
        }
    }

    lv.indexBuf = sh.buf;
    lv.indices = sh.indices;
    return true;
}

void FreeTile(uint32_t t) {
    if (Tile* tile = s_lod.tiles[t]) {
        for (Level& lv : tile->lv) {
            FreeLevel(lv);
        }

        s_lod.bytes -= sizeof(Tile);
        delete tile;
        s_lod.tiles[t] = nullptr;
    }

    s_lod.state[t] = Lod_None;
}

// The camera's distance to tile t: nearest and farthest in the ground plane.
void TileDistance(uint32_t t, float& dmin, float& dmax) {
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float x1 = kOrigin - (t / 64) * kTileYd, x0 = x1 - kTileYd;
    const float y1 = kOrigin - (t % 64) * kTileYd, y0 = y1 - kTileYd;
    const float dx = std::max({ x0 - cam.x, cam.x - x1, 0.0f });
    const float dy = std::max({ y0 - cam.y, cam.y - y1, 0.0f });
    const float fx = std::max(fabsf(cam.x - x0), fabsf(cam.x - x1));
    const float fy = std::max(fabsf(cam.y - y0), fabsf(cam.y - y1));
    dmin = sqrtf(dx * dx + dy * dy);
    dmax = sqrtf(fx * fx + fy * fy);
}

float NearZ() {
    return CGWorldFrame::GetActiveCamera()->m_farZ - 50.0f;
}

}  // namespace

bool FarLod::WantsAdt(uint32_t t) {
    // Heights may take half the cap; the rest is for the levels built from them.
    if (!On() || s_lod.state[t] != Lod_None || (s_lod.loaded.size() + 1) * sizeof(Tile) > Cap() / 2) {
        return false;
    }

    float dmin, dmax;
    TileDistance(t, dmin, dmax);
    return dmax > NearZ();  // a tile wholly inside the horizon's near plane is never drawn by it
}

void FarLod::MarkReading(uint32_t t) {
    s_lod.state[t] = Lod_Reading;
}

void FarLod::AdtRead(uint32_t t, const uint8_t* buf, uint32_t size) {
    if (s_lod.state[t] != Lod_Reading) {
        return;  // reset since
    }

    static float s_inner[128 * 128];
    Tile* tile = buf ? new Tile : nullptr;
    const CMapAreaLow* area = CMap::s_lowDetail.m_areas[t];

    if (!tile || !Extract(buf, size, *tile, s_inner)) {
        delete tile;
        s_lod.state[t] = Lod_Failed;
        return;
    }

    MeasureErrors(*tile, s_inner, area ? area->m_heights : nullptr);
    tile->anyHole = false;
    tile->lo = 1e30f;
    tile->hi = -1e30f;

    for (uint16_t h : tile->holes) {
        tile->anyHole |= h != 0;
    }

    for (float h : tile->h) {
        tile->lo = std::min(tile->lo, h);
        tile->hi = std::max(tile->hi, h);
    }

    for (Level& lv : tile->lv) {
        lv = Level {};
    }

    s_lod.tiles[t] = tile;
    s_lod.state[t] = Lod_Ready;
    s_lod.loaded.push_back(t);
    s_lod.bytes += sizeof(Tile);
}

void FarLod::BeginFrame() {
    s_lod.frame++;
    s_lod.drawn[0].clear();
    s_lod.drawn[1].clear();
    s_lod.want.clear();
    s_lod.draws = 0;
    memset(s_lod.perLevel, 0, sizeof(s_lod.perLevel));

    // Heights the horizon has moved away from (or all of them, switched off) go; the pump reads them again
    // if they come back into reach.
    if (s_lod.frame % 60 == 0 || (!On() && !s_lod.loaded.empty())) {
        const float reach = World::s_horizonFarClipScale * World::s_farClip + 2.5f * kTileYd;

        for (size_t i = 0; i < s_lod.loaded.size();) {
            const uint32_t t = s_lod.loaded[i];
            float dmin, dmax;
            TileDistance(t, dmin, dmax);

            if (!On() || dmin > reach || dmax < NearZ() - kTileYd) {
                FreeTile(t);
                s_lod.loaded[i] = s_lod.loaded.back();
                s_lod.loaded.pop_back();
            } else {
                i++;
            }
        }
    }
}

bool FarLod::Claim(uint32_t t, bool textured) {
    if (!On() || s_lod.state[t] != Lod_Ready) {
        return false;
    }

    Tile& tile = *s_lod.tiles[t];
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float x1 = kOrigin - (t / 64) * kTileYd, x0 = x1 - kTileYd;
    const float y1 = kOrigin - (t % 64) * kTileYd, y0 = y1 - kTileYd;
    const float dx = std::max({ x0 - cam.x, cam.x - x1, 0.0f });
    const float dy = std::max({ y0 - cam.y, cam.y - y1, 0.0f });
    const float dz = std::max({ tile.lo - cam.z, cam.z - tile.hi, 0.0f });
    const float dmin = sqrtf(dx * dx + dy * dy + dz * dz);
    const float farZ = CGWorldFrame::GetActiveCamera()->m_farZ;

    // Pixels a yard of height error covers at the tile's nearest drawn point: half the viewport's height
    // times the projection's y scale (1 / tan(fov / 2)), over the distance.
    const CRect& rect = g_theGxDevicePtr->m_curWindowRect;
    const float pxPerYd = 0.5f * (rect.maxY - rect.minY) * fabsf(g_theGxDevicePtr->m_projNative.b1) / std::max(dmin, farZ - 50.0f);
    const float limit = std::max(PixelsCVar()->m_floatValue, 0.05f);
    uint32_t level = 0;

    // The real terrain reaches into this tile (it draws whole chunks out to farclip): level 0, so its edge
    // meets vertices and normals that are its own.
    if (dmin > farZ + 34.0f) {
        if (tile.err[3] * pxPerYd <= limit) {
            return false;  // the WDL is good enough here, and batched
        }

        for (level = 2; level > 0 && tile.err[level] * pxPerYd > limit; level--) {
        }
    }

    uint32_t draw = level;

    if (!Built(tile, level)) {
        s_lod.want.push_back({ dmin, t, level });
        // Meanwhile the nearest built level, finer first; the WDL when none is.
        draw = 3;

        for (uint32_t k = 1; k < 3 && draw == 3; k++) {
            if (level >= k && Built(tile, level - k)) {
                draw = level - k;
            } else if (level + k < 3 && Built(tile, level + k)) {
                draw = level + k;
            }
        }

        if (draw == 3) {
            return false;
        }
    }

    tile.lv[draw].used = s_lod.frame;
    s_lod.drawn[textured ? 0 : 1].push_back({ dmin, t, draw });
    return true;
}

void FarLod::Draw(uint32_t set) {
    std::vector<Drawn>& list = s_lod.drawn[set];

    if (list.empty()) {
        return;
    }

    std::sort(list.begin(), list.end(), [](const Drawn& a, const Drawn& b) { return a.d < b.d; });
    GxRsSet(GxRs_Culling, 0);  // skirts and the hole edges are seen from both sides

    for (const Drawn& d : list) {
        const Level& lv = s_lod.tiles[d.t]->lv[d.level];
        GxPrimVertexPtr(lv.vertexBuf, GxVBF_PN);
        g_theGxDevicePtr->PrimIndexPtr(lv.indexBuf);
        CGxBatch batch;
        batch.m_primType = GxPrim_Triangles;
        batch.m_start = 0;
        batch.m_count = lv.indices;
        batch.m_minIndex = 0;
        batch.m_maxIndex = lv.verts - 1;
        g_theGxDevicePtr->Draw(&batch, 1);
        s_lod.draws++;
        s_lod.perLevel[d.level]++;
    }
}

void FarLod::EndFrame() {
    std::sort(s_lod.want.begin(), s_lod.want.end(), [](const Want& a, const Want& b) { return a.d < b.d; });
    uint32_t built = 0;

    for (const Want& w : s_lod.want) {
        if (built >= kBuildsPerFrame) {
            break;
        }

        if (s_lod.tiles[w.t] && Build(w.t, w.level)) {
            built++;
        }
    }

    if (StatsCVar()->m_intValue && s_lod.frame % 600 == 0) {
        uint32_t levels[3] = {};

        for (uint32_t t : s_lod.loaded) {
            for (uint32_t l = 0; l < 3; l++) {
                levels[l] += s_lod.tiles[t]->lv[l].vertexBuf ? 1 : 0;
            }
        }

        char line[256];
        SStrPrintf(line, sizeof(line), "farTerrainLod: %u tiles with heights; built full/half/quarter %u/%u/%u; drawn %u/%u/%u in %u draws (+ the WDL's); %.1f of %d MB",
            static_cast<uint32_t>(s_lod.loaded.size()), levels[0], levels[1], levels[2], s_lod.perLevel[0], s_lod.perLevel[1], s_lod.perLevel[2], s_lod.draws,
            s_lod.bytes / 1048576.0, MemoryCVar()->m_intValue);
        ConsoleWrite(line, DEFAULT_COLOR);
    }
}

void FarLod::Reset() {
    for (uint32_t t = 0; t < 4096; t++) {
        FreeTile(t);
    }

    for (Shared& sh : s_lod.shared) {
        FreeBuf(sh.pool, sh.buf);
        sh = Shared {};
    }

    s_lod.loaded.clear();
    s_lod.bytes = 0;
}
