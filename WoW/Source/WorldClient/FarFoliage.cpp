// Not in 12340 (farTrees, farGrass): far trees and bushes as cards of their own models, and the ground-effect
// grass culled by its size on screen. docs/FARFIELD_FOG.md "Far foliage".
//
// Placements: each ADT the far-ground bake reads (MDDF, wraithhost's tree / bush name rules), with the ground's
// MCCV tint and MCSH shadow under each. Pictures: the model's skin-00 mesh rasterised on the CPU from 8 sides
// (45 deg apart), 4 x 4 supersampled, into two 4096 atlases (albedo + coverage DXT5, normals DXT1), 64 px a view
// (128 for models 40 yd+ as placed), coverage-preserving mips; baked a view at a time under 3 ms a frame.
// Cards: one static quad a placement per block; the FarFoliage vertex shader turns it to the camera and mixes
// the two nearest views per pixel, so nothing is rebuilt as the camera moves (the old per-rebuild snap of
// every card's view and turn was the "flicker when moving"). Lit from the baked normals with the terrain's
// sun, ambient and fog (FarField::TerrainConstants), dissolved by a card-anchored hash at the hand-over.
#include "MapLowDetail.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Os/TimeManager.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/MapRenderChunk.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxMatrixStack.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/CDetailDoodadData.h"
#include "WoW/Source/WorldClient/DetailDoodad.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/WorldView.h"

namespace {

constexpr float kOrigin = 17066.666f;
constexpr float kTileYd = 533.33331f;
constexpr uint32_t kViews = 8;           // every 45 deg; the two nearest are mixed per pixel (wxl-farfield)
constexpr uint32_t kCellPx = 64;         // a view of a small model's picture; a big model's is 128 (2 x 2 cells)
constexpr uint32_t kSuper = 4;           // raster samples a picture pixel, each way (antialiased edges, filtered texels)
constexpr uint32_t kViewLevels = 5;      // per view: 64..4 / 128..8 px; the atlas levels past that from kSmallFrom
constexpr uint32_t kAtlasPx = 4096;      // two square atlases: albedo + coverage (DXT5), normals (DXT1)
constexpr uint32_t kCols = kAtlasPx / (kCellPx * kViews);  // 8 cells a row (a cell = one small model's 8 views)
constexpr uint32_t kRows = kAtlasPx / kCellPx;             // 64 rows: 512 small models, or 128 big ones
constexpr uint32_t kAtlasLevels = 11;    // 4096 .. 4: the DXT chain ITexWHDStartEnd asks for
constexpr uint32_t kSmallFrom = 4;       // the atlas level kept as floats; levels 5..10 are box-filtered from it
constexpr float kBigYards = 40.0f;       // header-box frame x the largest placed scale: 128 px views from here
constexpr uint32_t kBakeMs = 3;          // picture bake budget a frame (main thread, a view at a time)
constexpr uint32_t kChunkCards = 16384;  // 65536 vertices: 16-bit indices
constexpr float kTreeMinHeight = 6.0f;   // wraithhost FarLod.hpp: yards, as placed
constexpr float kBushMinHeight = 1.5f;
constexpr float kBushCover = 0.95f;      // a bush needs a texture cut out somewhere (leaves), not a solid prop
constexpr float kTintStrength = 0.5f;    // how much of the ground's vertex colour (MCCV) a card takes
constexpr float kShadowLevel = 0.7f;     // brightness in the ground's baked shadow (the terrain's 0.7 + 0.3 x lit)

uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

uint16_t Rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}

float RdF(const uint8_t* p) {
    float f;
    memcpy(&f, p, 4);
    return f;
}

bool ReadAll(const char* name, std::vector<uint8_t>& out, uint32_t limit = 0) {
    SFileImpl* file = nullptr;

    if (!SFileOpenFile(name, &file) || !file) {
        return false;
    }

    uint32_t size = SFileGetFileSize(file, nullptr);
    size = limit && size > limit ? limit : size;
    out.resize(size);
    uint32_t got = 0;
    const bool ok = size && SFileReadFile(file, out.data(), size, &got, nullptr, 0) && got == size;
    SFileCloseFile(file);
    return ok;
}

CVar* FarTreesCVar() {
    static CVar* s_cvar = CVar::Register("farTrees", "Engine option: draw trees and bushes past their models' view distance as cards of their own pictures, out to the far terrain, fogged like it (0: stock)", 0x1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* FarGrassCVar() {
    static CVar* s_cvar = CVar::Register("farGrass", "Engine option: fade out ground-effect grass where a tuft would be smaller on screen than farGrassMinPixels (0: stock, grass to groundEffectDist)", 0x1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* FarGrassMinPixelsCVar() {
    static CVar* s_cvar = CVar::Register("farGrassMinPixels", "Engine option: the on-screen height (pixels) under which a ground-effect tuft is faded out (farGrass)", 0x1, "4", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* FarFoliageStatsCVar() {
    static CVar* s_cvar = CVar::Register("farTreesStats", "Engine option: print the far tree cards' counts, draws, atlas use, memory and bake time to the console every 2 seconds", 0x1, "0", nullptr, CATEGORY_DEBUG, false, nullptr, false);
    return s_cvar;
}

// --- Which placements (wraithhost FarLod.cpp Builder::IsBushName / ModelInfo) ---

bool Has(const std::string& key, const char* w) {
    return key.find(w) != std::string::npos;
}

bool IsBushName(const std::string& path) {
    const size_t slash = path.find_last_of('\\');
    const std::string key = path.substr(slash == std::string::npos ? 0 : slash + 1);
    static const char* const kYes[] = { "BUSH", "SHRUB", "FERN", "HEDGE", "PLANT", "FOLIAGE", "THORN", "BRAMBLE",
        "REED", "CATTAIL", "IVY", "VINE", "SAPLING", "CACTUS", "MUSHROOM", "FROND", "BAMBOO", "GRASS", "FLOWER" };
    static const char* const kNo[] = { "UNDERWATER", "SEAWEED", "KELP", "CORAL", "FX", "SMOKE", "FIRE", "SPELL",
        "PARTICLE", "DETAIL", "ROCK", "CRYSTAL", "BANNER", "CRATE", "POT", "VASE", "BARREL", "BOX", "SACK", "BASKET",
        "CART", "WAGON", "TABLE", "SIGN", "FENCE", "ROOF", "HOUSE", "BUILDING", "HUT", "TENT", "BRIDGE",
        "LEAVES", "STUMP", "LOG", "TREE", "PALM" };
    bool yes = false;

    for (const char* w : kYes) {
        yes = yes || Has(key, w);
    }

    if (!yes) {
        return false;
    }

    for (const char* w : kNo) {
        if (Has(key, w)) {
            return false;
        }
    }

    return true;
}

enum : uint8_t { Pic_None, Pic_Queued, Pic_Baked, Pic_Failed };

struct Model {
    std::string file;   // .m2
    bool tree = false;
    bool bush = false;
    bool big = false;   // 128 px views (2 x 2 cells)
    float lo[3] = {};   // header box (model units)
    float hi[3] = {};
    float maxScale = 0.0f;  // the largest placed scale seen
    uint8_t pic = Pic_None;
    uint16_t cellCol = 0;
    uint16_t cellRow = 0;
    float frame = 0.0f;   // the picture's side (model units)
    float bottom = 0.0f;  // model z at its bottom edge
};

struct Placed {
    float x, y, z;
    float yaw;     // radians, the model's +x in the world
    float scale;
    uint32_t model;
    uint8_t cls;   // stock size class (MapDoodadDef.cpp: largest side of the world box against s_fadeSize)
    uint8_t tint[4] = { 127, 127, 127, 255 };  // the ground under it: MCCV b, g, r (127 = none), a = 255 lit / 0 in MCSH shadow
};

struct Chunk {
    CGxPool* pool = nullptr;
    CGxBuf* buf = nullptr;
    uint32_t capacity = 0;   // cards
    uint32_t cards = 0;
    uint32_t start[5] = {};  // per class: first card / count in this chunk
    uint32_t count[5] = {};
};

struct Block {
    std::vector<Placed> placed;
    std::vector<Chunk> chunks;
    float lo[3] = { 1e30f, 1e30f, 1e30f };    // placements' box, card tops included (frustum test)
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    uint32_t builtGen = 0;   // s_fol.bakeGen at the build
    bool built = false;
    bool missing = false;    // some placement's picture was not baked yet at the build
};

struct Texture {
    std::vector<uint8_t> rgba;
    uint32_t w = 0;
    uint32_t h = 0;
};

struct Layer {
    uint32_t start, count, blend;
    const Texture* tex;
    float colour[4];
};

// One model's picture in progress, a view a step (PumpPictures).
struct BakeJob {
    uint32_t model = ~0u;
    uint32_t view = 0;
    std::vector<uint8_t> m;
    std::vector<uint8_t> skin;
    std::unordered_map<uint32_t, Texture> textures;
    std::vector<Layer> layers;
    const uint8_t* verts = nullptr;
    const uint8_t* skinVerts = nullptr;
    const uint8_t* indices = nullptr;
    uint32_t nVert = 0, nSkinVert = 0, nIdx = 0;
    float zLo = 0.0f, zHi = 0.0f, frame = 0.0f;
    bool anyDrawn = false;
    uint32_t ms = 0;
};

struct FolState {
    std::unordered_map<std::string, uint32_t> byName;
    std::vector<Model> models;
    std::vector<uint32_t> queue;   // models waiting for their picture, nearest tile's first
    std::unordered_set<uint32_t> uids;
    Block blocks[64];
    bool cells[kCols * kRows] = {};
    uint32_t bakeGen = 1;
    uint8_t* chain[2] = {};        // 0: albedo + coverage DXT5, 1: normals DXT1, every level
    uint32_t levelOffset[2][kAtlasLevels] = {};
    std::vector<float> small[2];   // the atlases at level kSmallFrom (256 x 256) as rgba floats, for levels 5..10
    CTexture* atlas[2] = {};
    CGxPool* indexPool = nullptr;
    CGxBuf* indexBuf = nullptr;
    CGxShader* vs = nullptr;
    CGxShader* ps = nullptr;
    bool shadersTried = false;
    BakeJob job;
    uint32_t baked = 0;            // stats: pictures done, ms spent on them, draws last frame
    uint32_t bakeMs = 0;
    uint32_t draws = 0;
};

FolState s_fol;

// --- Pictures (wraithhost Impostor.cpp, at wxl-farfield's quality: 8 views, supersampled, normals) ---

void From565(uint16_t v, int* c) {
    c[0] = (v >> 11) * 255 / 31;
    c[1] = ((v >> 5) & 63) * 255 / 63;
    c[2] = (v & 31) * 255 / 31;
}

// BLP2 level `level` (or the nearest one it has) to RGBA8 (Impostor.cpp DecodeBlp).
bool DecodeBlp(const std::vector<uint8_t>& b, Texture& out, uint32_t level) {
    if (b.size() < 148 || memcmp(b.data(), "BLP2", 4)) {
        return false;
    }

    const uint8_t compression = b[8], alphaDepth = b[9], alphaType = b[10], hasMips = b[11];
    const uint32_t w0 = Rd32(&b[12]), h0 = Rd32(&b[16]);

    if (!w0 || !h0 || w0 > 4096 || h0 > 4096) {
        return false;
    }

    if (!hasMips) {
        level = 0;
    }

    while (level > 0) {
        const uint32_t off = Rd32(&b[20 + level * 4]), len = Rd32(&b[84 + level * 4]);

        if (level < 16 && off && len && uint64_t(off) + len <= b.size() && (w0 >> level) >= 4 && (h0 >> level) >= 4) {
            break;
        }

        level--;
    }

    const uint32_t w = std::max(w0 >> level, 1u), h = std::max(h0 >> level, 1u);
    const uint32_t off = Rd32(&b[20 + level * 4]), len = Rd32(&b[84 + level * 4]);

    if (!off || uint64_t(off) + len > b.size()) {
        return false;
    }

    const uint8_t* s = &b[off];
    out.rgba.assign(size_t(w) * h * 4, 255);
    auto put = [&](uint32_t x, uint32_t y, int r, int g, int bl, int a) {
        if (x >= w || y >= h) {
            return;
        }

        uint8_t* o = &out.rgba[(size_t(y) * w + x) * 4];
        o[0] = uint8_t(r);
        o[1] = uint8_t(g);
        o[2] = uint8_t(bl);
        o[3] = uint8_t(a);
    };

    if (compression == 2) {
        const bool dxt1 = alphaDepth <= 1 || alphaType == 0;
        const uint32_t bytes = dxt1 ? 8 : 16, bw = std::max(w / 4, 1u), bh = std::max(h / 4, 1u);

        if (len < bw * bh * bytes) {
            return false;
        }

        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t* ab = s + (size_t(by) * bw + bx) * bytes;
                const uint8_t* blk = ab + (bytes - 8);
                const uint16_t v0 = Rd16(blk), v1 = Rd16(blk + 2);
                int c[4][3];
                From565(v0, c[0]);
                From565(v1, c[1]);

                for (int j = 0; j < 3; j++) {
                    if (v0 > v1 || !dxt1) {
                        c[2][j] = (2 * c[0][j] + c[1][j]) / 3;
                        c[3][j] = (c[0][j] + 2 * c[1][j]) / 3;
                    } else {
                        c[2][j] = (c[0][j] + c[1][j]) / 2;
                        c[3][j] = 0;
                    }
                }

                int a5[8] = {};

                if (!dxt1 && alphaType == 7) {
                    a5[0] = ab[0];
                    a5[1] = ab[1];

                    for (int q = 2; q < 8; q++) {
                        a5[q] = a5[0] > a5[1] ? ((8 - q) * a5[0] + (q - 1) * a5[1]) / 7
                                              : q < 6 ? ((6 - q) * a5[0] + (q - 1) * a5[1]) / 5 : (q == 6 ? 0 : 255);
                    }
                }

                uint64_t a5bits = 0;

                for (int q = 0; q < 6; q++) {
                    a5bits |= uint64_t(ab[2 + q]) << (8 * q);
                }

                const uint32_t bits = Rd32(blk + 4);

                for (uint32_t p = 0; p < 16; p++) {
                    const uint32_t idx = (bits >> (2 * p)) & 3;
                    int a = 255;

                    if (dxt1) {
                        a = (alphaDepth >= 1 && v0 <= v1 && idx == 3) ? 0 : 255;
                    } else if (alphaType == 7) {
                        a = a5[(a5bits >> (3 * p)) & 7];
                    } else {
                        a = ((ab[p / 2] >> (4 * (p % 2))) & 15) * 17;
                    }

                    put(bx * 4 + p % 4, by * 4 + p / 4, c[idx][0], c[idx][1], c[idx][2], a);
                }
            }
        }
    } else if (compression == 1 && len >= w * h) {
        const uint8_t* am = s + size_t(w) * h;
        const bool alphaFits = uint64_t(w) * h * alphaDepth / 8 + uint64_t(w) * h <= len;

        for (uint32_t k = 0; k < w * h; k++) {
            const uint8_t* e = &b[148 + s[k] * 4];
            int a = 255;

            if (alphaFits && alphaDepth == 8) {
                a = am[k];
            } else if (alphaFits && alphaDepth == 4) {
                a = ((am[k / 2] >> (4 * (k % 2))) & 15) * 17;
            } else if (alphaFits && alphaDepth == 1) {
                a = (am[k / 8] >> (k % 8)) & 1 ? 255 : 0;
            }

            put(k % w, k / w, e[2], e[1], e[0], a);
        }
    } else if (compression == 3 && len >= w * h * 4) {
        for (uint32_t k = 0; k < w * h; k++) {
            put(k % w, k / w, s[k * 4 + 2], s[k * 4 + 1], s[k * 4], s[k * 4 + 3]);
        }
    } else {
        return false;
    }

    out.w = w;
    out.h = h;
    return true;
}

// An M2 batch's colour at frame 0 (Impostor.cpp BatchColour): colour track rgb, alpha x transparency.
void BatchColour(const std::vector<uint8_t>& m, const uint8_t* batch, float rgba[4]) {
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1.0f;
    auto frame0 = [&](uint32_t at, uint32_t comps, float* v) {
        if (uint64_t(at) + 20 > m.size() || !Rd32(&m[at + 12])) {
            return;
        }

        const uint32_t o = Rd32(&m[at + 16]);

        if (uint64_t(o) + 8 > m.size() || !Rd32(&m[o])) {
            return;
        }

        const uint32_t o2 = Rd32(&m[o + 4]);

        if (uint64_t(o2) + comps * 4 > m.size()) {
            return;
        }

        if (comps == 3) {
            for (uint32_t k = 0; k < 3; k++) {
                v[k] = std::clamp(RdF(&m[o2 + k * 4]), 0.0f, 1.0f);
            }
        } else {
            v[0] = std::clamp(float(int16_t(Rd16(&m[o2]))) / 32767.0f, 0.0f, 1.0f);
        }
    };
    const int16_t colour = int16_t(Rd16(batch + 8));

    if (colour >= 0 && uint32_t(colour) < Rd32(&m[0x48])) {
        const uint32_t at = Rd32(&m[0x4C]) + uint32_t(colour) * 0x28;
        frame0(at, 3, rgba);
        frame0(at + 20, 1, rgba + 3);
    }

    const uint32_t weight = Rd16(batch + 20);

    if (weight < Rd32(&m[0x90]) && uint64_t(Rd32(&m[0x94])) + (weight + 1) * 2 <= m.size()) {
        const uint32_t t = Rd16(&m[Rd32(&m[0x94]) + weight * 2]);
        float tr = 1.0f;

        if (t < Rd32(&m[0x58])) {
            frame0(Rd32(&m[0x5C]) + t * 20, 1, &tr);
        }

        rgba[3] *= tr;
    }
}

template <class Fn>
void RasterTriangle(const float* sx, const float* sy, int w, int h, Fn&& fn) {
    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);

    if (fabsf(area) < 1e-6f) {
        return;
    }

    const int x0 = std::max(0, int(floorf(std::min({ sx[0], sx[1], sx[2] }))));
    const int x1 = std::min(w - 1, int(ceilf(std::max({ sx[0], sx[1], sx[2] }))));
    const int y0 = std::max(0, int(floorf(std::min({ sy[0], sy[1], sy[2] }))));
    const int y1 = std::min(h - 1, int(ceilf(std::max({ sy[0], sy[1], sy[2] }))));

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const float qx = x + 0.5f, qy = y + 0.5f;
            const float w0 = ((sx[1] - qx) * (sy[2] - qy) - (sx[2] - qx) * (sy[1] - qy)) / area;
            const float w1 = ((sx[2] - qx) * (sy[0] - qy) - (sx[0] - qx) * (sy[2] - qy)) / area;
            const float w2 = 1.0f - w0 - w1;

            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }

            fn(x, y, w0, w1, w2);
        }
    }
}

void EncodeDxt5Alpha(const uint8_t* a, uint8_t* out) {
    uint8_t lo = 255, hi = 0;

    for (int k = 0; k < 16; k++) {
        if (a[k]) {
            lo = std::min(lo, a[k]);
        }

        hi = std::max(hi, a[k]);
    }

    // Six-value mode (a0 <= a1) keeps exact 0 for the cut-out; the interpolated six span the noise.
    if (lo > hi) {
        lo = hi;
    }

    int v[8] = { lo, hi };

    for (int q = 2; q < 6; q++) {
        v[q] = ((6 - q) * lo + (q - 1) * hi) / 5;
    }

    v[6] = 0;
    v[7] = 255;
    uint64_t bits = 0;

    for (int k = 0; k < 16; k++) {
        // A cut-out texel stays exactly 0 (index 6); a covered one takes the nearest of the six, never 0.
        int best = 6;

        if (a[k]) {
            int bestD = 1 << 30;

            for (int q = 0; q < 6; q++) {
                const int d = (a[k] - v[q]) * (a[k] - v[q]);

                if (d < bestD) {
                    bestD = d;
                    best = q;
                }
            }
        }

        bits |= uint64_t(best) << (3 * k);
    }

    out[0] = lo;
    out[1] = hi;

    for (int q = 0; q < 6; q++) {
        out[2 + q] = static_cast<uint8_t>(bits >> (8 * q));
    }
}

uint8_t* BlockAt(uint32_t kind, uint32_t l, uint32_t x0, uint32_t y0) {
    const uint32_t bytes = kind ? 8 : 16;
    return s_fol.chain[kind] + s_fol.levelOffset[kind][l] + (size_t(y0 / 4) * ((kAtlasPx >> l) / 4) + x0 / 4) * bytes;
}

// One 4 x 4 block (4 floats a pixel) at (x0, y0) of level l: kind 0 rgb + coverage as DXT5, kind 1 a normal
// (-1..1) as DXT1.
void EncodeBlock(uint32_t kind, const float* px, uint32_t stride, uint32_t l, uint32_t x0, uint32_t y0) {
    float col[16][3];
    uint8_t a[16];

    for (uint32_t k = 0; k < 16; k++) {
        const float* q = px + (size_t(k / 4) * stride + k % 4) * 4;

        for (int j = 0; j < 3; j++) {
            col[k][j] = std::clamp(kind ? q[j] * 0.5f + 0.5f : q[j], 0.0f, 1.0f) * 255.0f;
        }

        a[k] = static_cast<uint8_t>(std::clamp(q[3], 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    uint8_t* out = BlockAt(kind, l, x0, y0);

    if (kind) {
        FarField::EncodeDxt1Block(col, out);
    } else {
        EncodeDxt5Alpha(a, out);
        FarField::EncodeDxt1Block(col, out + 8);
    }
}

void AtlasUserFunc(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    const uint32_t kind = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(userArg));

    if (command == GxTex_Latch && kind < 2 && s_fol.chain[kind] && mipLevel < kAtlasLevels) {
        texelStrideInBytes = (kAtlasPx >> mipLevel) / 4 * (kind ? 8 : 16);
        texels = s_fol.chain[kind] + s_fol.levelOffset[kind][mipLevel];
    }
}

// Bilinear, wrapped (the M2's own sampling, near enough).
void Sample(const Texture& t, float u, float v, float out[4]) {
    u = u * t.w - 0.5f;
    v = v * t.h - 0.5f;
    const float fu = floorf(u), fv = floorf(v);
    const float au = u - fu, av = v - fv;
    const int32_t w = static_cast<int32_t>(t.w), h = static_cast<int32_t>(t.h);
    const int32_t x0 = ((static_cast<int32_t>(fu) % w) + w) % w, y0 = ((static_cast<int32_t>(fv) % h) + h) % h;
    const int32_t x1 = (x0 + 1) % w, y1 = (y0 + 1) % h;
    const uint8_t* p00 = &t.rgba[(size_t(y0) * w + x0) * 4];
    const uint8_t* p10 = &t.rgba[(size_t(y0) * w + x1) * 4];
    const uint8_t* p01 = &t.rgba[(size_t(y1) * w + x0) * 4];
    const uint8_t* p11 = &t.rgba[(size_t(y1) * w + x1) * 4];

    for (int j = 0; j < 4; j++) {
        const float top = p00[j] + (p10[j] - p00[j]) * au;
        const float bot = p01[j] + (p11[j] - p01[j]) * au;
        out[j] = (top + (bot - top) * av) / 255.0f;
    }
}

// Model's M2, skin 00, textures and batches into job; false when it cannot be drawn or, for a bush, when no
// texture is cut out (a solid prop).
bool PrepareJob(const Model& model, BakeJob& job) {
    std::vector<uint8_t>& m = job.m;

    if (!ReadAll(model.file.c_str(), m) || m.size() < 0xBC || memcmp(m.data(), "MD20", 4)) {
        return false;
    }

    auto arr = [&](const std::vector<uint8_t>& f, uint32_t at, uint32_t stride, uint32_t& n) -> const uint8_t* {
        n = Rd32(&f[at]);
        const uint32_t ofs = Rd32(&f[at + 4]);

        if (!n || uint64_t(ofs) + uint64_t(n) * stride > f.size()) {
            n = 0;
            return nullptr;
        }

        return &f[ofs];
    };
    uint32_t nTex = 0, nMat = 0, nLookup = 0;
    job.verts = arr(m, 0x3C, 48, job.nVert);
    const uint8_t* texs = arr(m, 0x50, 16, nTex);
    const uint8_t* mats = arr(m, 0x70, 4, nMat);
    const uint8_t* lookup = arr(m, 0x80, 2, nLookup);

    if (!job.verts) {
        return false;
    }

    const std::string skinName = model.file.substr(0, model.file.size() - 3) + "00.skin";

    if (!ReadAll(skinName.c_str(), job.skin) || job.skin.size() < 0x30 || memcmp(job.skin.data(), "SKIN", 4)) {
        return false;
    }

    uint32_t nSub = 0, nBatch = 0;
    job.skinVerts = arr(job.skin, 0x04, 2, job.nSkinVert);
    job.indices = arr(job.skin, 0x0C, 2, job.nIdx);
    const uint8_t* subs = arr(job.skin, 0x1C, 48, nSub);
    const uint8_t* batches = arr(job.skin, 0x24, 24, nBatch);

    if (!job.skinVerts || !job.indices || !subs || !batches) {
        return false;
    }

    // The frame: tall enough for the model, wide enough for it turned any way about its axis.
    float zLo = 1e30f, zHi = -1e30f, reach = 0.0f;

    for (uint32_t v = 0; v < job.nVert; v++) {
        const uint8_t* p = job.verts + v * 48;
        const float x = RdF(p), y = RdF(p + 4), z = RdF(p + 8);
        zLo = std::min(zLo, z);
        zHi = std::max(zHi, z);
        reach = std::max(reach, sqrtf(x * x + y * y));
    }

    job.zLo = zLo;
    job.zHi = zHi;
    job.frame = std::max(zHi - zLo, 2.0f * reach);

    if (!(job.frame > 0.1f)) {
        return false;
    }

    // Textures by lookup slot, decoded once at about the raster's density; the least alpha coverage seen (bushes).
    const uint32_t maxTex = model.big ? 512 : 256;
    float leastCover = 1.0f;
    auto texture = [&](uint32_t combo) -> const Texture* {
        if (!lookup || combo >= nLookup) {
            return nullptr;
        }

        const uint32_t t = Rd16(lookup + combo * 2);
        auto it = job.textures.find(t);

        if (it != job.textures.end()) {
            return it->second.rgba.empty() ? nullptr : &it->second;
        }

        Texture& tex = job.textures[t];

        if (!texs || t >= nTex) {
            return nullptr;
        }

        const uint8_t* e = texs + t * 16;
        const uint32_t type = Rd32(e), len = Rd32(e + 8), at = Rd32(e + 12);

        if (type != 0 || len < 2 || uint64_t(at) + len > m.size()) {
            return nullptr;
        }

        std::vector<uint8_t> blp;
        const std::string name(reinterpret_cast<const char*>(&m[at]), strnlen(reinterpret_cast<const char*>(&m[at]), len));

        if (!ReadAll(name.c_str(), blp) || blp.size() < 20) {
            return nullptr;
        }

        const uint32_t w0 = Rd32(&blp[12]);
        uint32_t level = 0;

        while ((w0 >> level) > maxTex) {
            level++;
        }

        if (!DecodeBlp(blp, tex, level) || !tex.w || !tex.h) {
            tex.rgba.clear();
            return nullptr;
        }

        uint32_t covered = 0;

        for (size_t k = 3; k < tex.rgba.size(); k += 4) {
            covered += tex.rgba[k] >= 128;
        }

        leastCover = std::min(leastCover, float(covered) / float(tex.w * tex.h));
        return &tex;
    };

    // Blend 0-2 into the picture; 3, 4 (additive glow) and 5, 6 (modulated shimmer) left out.
    for (uint32_t bi = 0; bi < nBatch; bi++) {
        const uint8_t* b = batches + bi * 24;
        const uint32_t section = Rd16(b + 4), material = Rd16(b + 10), combo = Rd16(b + 16);

        if (section >= nSub) {
            continue;
        }

        const uint32_t blend = (mats && material < nMat) ? Rd16(mats + material * 4 + 2) : 0;

        if (blend >= 3) {
            continue;
        }

        const uint8_t* sub = subs + section * 48;
        Layer l;
        l.start = Rd16(sub + 8) + (uint32_t(Rd16(sub + 2)) << 16);
        l.count = Rd16(sub + 10);
        l.blend = blend;
        l.tex = texture(combo);
        BatchColour(m, b, l.colour);
        job.layers.push_back(l);
    }

    return !(model.bush && !model.tree && leastCover >= kBushCover);
}

// View `view` of job's model into its atlas cells: the M2 rasterised orthographically from the model's +x
// turned view x 45 deg, kSuper x kSuper samples a pixel (textures bilinear, keyed at the game's own 224/255 for
// blend 1, half for blend 2), normals in the view's frame (right, up, toward the camera) half the mesh's and
// half a canopy dome's; then levels 0..4 with coverage-preserving alpha (each keeps level 0's cover at the 0.5
// test), empty pixels filled from drawn neighbours so filtering never pulls in a dark rim.
void BakeView(BakeJob& job, const Model& model, uint32_t view) {
    struct Pixel {
        float depth;
        float rgb[3];
        float n[3];
    };
    const uint32_t px = model.big ? kCellPx * 2 : kCellPx;
    const uint32_t raster = px * kSuper;
    static std::vector<Pixel> ras;
    ras.assign(size_t(raster) * raster, Pixel { -1e30f, { 0, 0, 0 }, { 0, 0, 0 } });
    const float frame = job.frame, zLo = job.zLo, zHi = job.zHi;
    const float ang = float(view) * (6.2831853f / kViews);
    const float cx = cosf(ang), cy = sinf(ang);

    for (const Layer& L : job.layers) {
        for (uint32_t i = 0; i + 2 < L.count && L.start + i + 2 < job.nIdx; i += 3) {
            float sx[3], sy[3], sd[3], uv[3][2], nv[3][3];
            bool ok = true;

            for (int k = 0; k < 3 && ok; k++) {
                const uint32_t si = Rd16(job.indices + (L.start + i + k) * 2);
                const uint32_t vi = si < job.nSkinVert ? Rd16(job.skinVerts + si * 2) : job.nVert;

                if (vi >= job.nVert) {
                    ok = false;
                    break;
                }

                const uint8_t* v = job.verts + vi * 48;
                const float x = RdF(v), y = RdF(v + 4), z = RdF(v + 8);
                const float nx = RdF(v + 20), ny = RdF(v + 24), nz = RdF(v + 28);
                sx[k] = ((-cy * x + cx * y) / frame + 0.5f) * raster;
                sy[k] = (zHi - z) / frame * raster + (frame - (zHi - zLo)) / frame * raster;  // bottom-aligned
                sd[k] = cx * x + cy * y;
                uv[k][0] = RdF(v + 32);
                uv[k][1] = RdF(v + 36);
                nv[k][0] = -cy * nx + cx * ny;
                nv[k][1] = nz;
                nv[k][2] = cx * nx + cy * ny;
            }

            if (!ok) {
                continue;
            }

            RasterTriangle(sx, sy, int(raster), int(raster), [&](int x, int y, float w0, float w1, float w2) {
                const float depth = w0 * sd[0] + w1 * sd[1] + w2 * sd[2];
                Pixel& p = ras[size_t(y) * raster + x];

                if (depth <= p.depth) {
                    return;
                }

                float c[4] = { 0.35f, 0.3f, 0.2f, 1.0f };

                if (L.tex) {
                    Sample(*L.tex, w0 * uv[0][0] + w1 * uv[1][0] + w2 * uv[2][0], w0 * uv[0][1] + w1 * uv[1][1] + w2 * uv[2][1], c);

                    if (L.blend == 0) {
                        c[3] = 1.0f;
                    }
                }

                // The game's own key: 224/255 for blend 1 (alpha-ref table 0x00AD8B7C), half for blend 2.
                if (c[3] < (L.blend == 1 ? 224.0f / 255.0f : 0.5f)) {
                    return;
                }

                float n[3];

                for (int j = 0; j < 3; j++) {
                    p.rgb[j] = c[j] * L.colour[j];
                    n[j] = w0 * nv[0][j] + w1 * nv[1][j] + w2 * nv[2][j];
                }

                // Two-sided: a leaf seen from behind is lit from its side facing the camera.
                const float s = n[2] < 0.0f ? -1.0f : 1.0f;

                for (int j = 0; j < 3; j++) {
                    p.n[j] = n[j] * s;
                }

                p.depth = depth;
                job.anyDrawn = true;
            });
        }
    }

    // Down to px: coverage in alpha, colour and normal averaged over what drew.
    const float domeY = 1.0f - 0.5f * (zHi - zLo) / frame;  // picture y (0 top) of the model's middle
    const float domeR = std::max(0.5f * (zHi - zLo) / frame, 0.05f);
    std::vector<float> lv(size_t(px) * px * 4, 0.0f), ln(size_t(px) * px * 4, 0.0f);

    for (uint32_t y = 0; y < px; y++) {
        for (uint32_t x = 0; x < px; x++) {
            float acc[3] = {}, nacc[3] = {};
            uint32_t n = 0;

            for (uint32_t k = 0; k < kSuper * kSuper; k++) {
                const Pixel& p = ras[size_t(y * kSuper + k / kSuper) * raster + x * kSuper + k % kSuper];

                if (p.depth > -1e29f) {
                    for (int j = 0; j < 3; j++) {
                        acc[j] += p.rgb[j];
                        nacc[j] += p.n[j];
                    }

                    n++;
                }
            }

            float* o = &lv[(size_t(y) * px + x) * 4];
            float* on = &ln[(size_t(y) * px + x) * 4];
            o[3] = float(n) / float(kSuper * kSuper);

            if (n) {
                // Half the mesh's normal, half a dome over the model (a canopy reads round, not as flat leaves).
                const float dx = ((x + 0.5f) / px - 0.5f) / domeR, dy = (domeY - (y + 0.5f) / px) / domeR;
                const float dl = sqrtf(dx * dx + dy * dy + 0.36f);
                const float ml = sqrtf(nacc[0] * nacc[0] + nacc[1] * nacc[1] + nacc[2] * nacc[2]);
                const float mesh[3] = { ml > 0 ? nacc[0] / ml : 0.0f, ml > 0 ? nacc[1] / ml : 0.0f, ml > 0 ? nacc[2] / ml : 1.0f };
                const float dome[3] = { dx / dl, dy / dl, 0.6f / dl };
                float nn[3], len = 0.0f;

                for (int j = 0; j < 3; j++) {
                    o[j] = acc[j] / n;
                    nn[j] = 0.5f * mesh[j] + 0.5f * dome[j];
                    len += nn[j] * nn[j];
                }

                len = len > 0 ? 1.0f / sqrtf(len) : 0.0f;

                for (int j = 0; j < 3; j++) {
                    on[j] = nn[j] * len;
                }
            }
        }
    }

    std::vector<uint8_t> has(size_t(px) * px);

    for (size_t k = 0; k < has.size(); k++) {
        has[k] = lv[k * 4 + 3] > 0.0f;
    }

    for (int pass = 0; pass < 8; pass++) {
        std::vector<float> next = lv, nextN = ln;
        std::vector<uint8_t> nextHas = has;

        for (uint32_t y = 0; y < px; y++) {
            for (uint32_t x = 0; x < px; x++) {
                if (has[y * px + x]) {
                    continue;
                }

                float acc[3] = {}, nacc[3] = {};
                int n = 0;

                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        const int xx = int(x) + dx, yy = int(y) + dy;

                        if (xx < 0 || yy < 0 || xx >= int(px) || yy >= int(px) || !has[yy * px + xx]) {
                            continue;
                        }

                        for (int j = 0; j < 3; j++) {
                            acc[j] += lv[(size_t(yy) * px + xx) * 4 + j];
                            nacc[j] += ln[(size_t(yy) * px + xx) * 4 + j];
                        }

                        n++;
                    }
                }

                if (n) {
                    for (int j = 0; j < 3; j++) {
                        next[(size_t(y) * px + x) * 4 + j] = acc[j] / n;
                        nextN[(size_t(y) * px + x) * 4 + j] = nacc[j] / n;
                    }

                    nextHas[y * px + x] = 1;
                }
            }
        }

        lv.swap(next);
        ln.swap(nextN);
        has.swap(nextHas);
    }

    float coverage0 = 0.0f;

    for (size_t k = 0; k < size_t(px) * px; k++) {
        coverage0 += lv[k * 4 + 3] >= 0.5f;
    }

    coverage0 /= float(px * px);
    const uint32_t slotX = model.cellCol * kCellPx * kViews, slotY = model.cellRow * kCellPx;
    uint32_t size = px;

    for (uint32_t l = 0; l < kViewLevels; l++) {
        if (l) {
            const uint32_t half = size / 2;

            for (std::vector<float>* img : { &lv, &ln }) {
                std::vector<float> down(size_t(half) * half * 4, 0.0f);

                for (uint32_t y = 0; y < half; y++) {
                    for (uint32_t x = 0; x < half; x++) {
                        for (int k = 0; k < 4; k++) {
                            const float* q = &(*img)[(size_t(y * 2 + k / 2) * size + x * 2 + k % 2) * 4];
                            float* o = &down[(size_t(y) * half + x) * 4];

                            for (int j = 0; j < 4; j++) {
                                o[j] += q[j] * 0.25f;
                            }
                        }
                    }
                }

                img->swap(down);
            }

            size = half;
            // Alpha scaled so the level keeps level 0's coverage at the 0.5 test (a far tree does not thin out).
            float lo = 0.5f, hi = 4.0f;

            for (int it = 0; it < 12; it++) {
                const float s = 0.5f * (lo + hi);
                float c = 0.0f;

                for (size_t k = 0; k < size_t(size) * size; k++) {
                    c += lv[k * 4 + 3] * s >= 0.5f;
                }

                if (c / float(size * size) < coverage0) {
                    lo = s;
                } else {
                    hi = s;
                }
            }

            for (size_t k = 0; k < size_t(size) * size; k++) {
                lv[k * 4 + 3] = std::min(1.0f, lv[k * 4 + 3] * hi);
            }
        }

        const uint32_t ox = (slotX + view * px) >> l;
        const uint32_t oy = slotY >> l;

        for (uint32_t by = 0; by < size / 4; by++) {
            for (uint32_t bx = 0; bx < size / 4; bx++) {
                EncodeBlock(0, &lv[(size_t(by * 4) * size + bx * 4) * 4], size, l, ox + bx * 4, oy + by * 4);
                EncodeBlock(1, &ln[(size_t(by * 4) * size + bx * 4) * 4], size, l, ox + bx * 4, oy + by * 4);
            }
        }

        if (l == kSmallFrom) {
            for (uint32_t y = 0; y < size; y++) {
                memcpy(&s_fol.small[0][(size_t(oy + y) * (kAtlasPx >> kSmallFrom) + ox) * 4], &lv[size_t(y) * size * 4], size * 4 * sizeof(float));
                memcpy(&s_fol.small[1][(size_t(oy + y) * (kAtlasPx >> kSmallFrom) + ox) * 4], &ln[size_t(y) * size * 4], size * 4 * sizeof(float));
            }
        }
    }
}

// Levels 5..10, where a texel spans views or models: box-filtered from level kSmallFrom, over the rows a model
// at cell row `row` (`rows` tall) reaches.
void StoreSmallLevels(uint32_t row, uint32_t rows) {
    for (uint32_t kind = 0; kind < 2; kind++) {
        std::vector<float> cur = s_fol.small[kind];
        uint32_t w = kAtlasPx >> kSmallFrom;

        for (uint32_t l = kSmallFrom + 1; l < kAtlasLevels; l++) {
            const uint32_t half = w / 2;
            std::vector<float> down(size_t(half) * half * 4, 0.0f);

            for (uint32_t y = 0; y < half; y++) {
                for (uint32_t x = 0; x < half; x++) {
                    for (int k = 0; k < 4; k++) {
                        const float* q = &cur[(size_t(y * 2 + k / 2) * w + x * 2 + k % 2) * 4];

                        for (int j = 0; j < 4; j++) {
                            down[(size_t(y) * half + x) * 4 + j] += q[j] * 0.25f;
                        }
                    }
                }
            }

            w = half;
            cur.swap(down);
            // The rows of 4 x 4 blocks this model's cells touch at this level.
            const uint32_t y0 = ((row * kCellPx) >> l) & ~3u, y1 = std::min(w, ((((row + rows) * kCellPx) >> l) + 3) & ~3u);
            std::vector<float> enc = cur;

            if (!kind) {
                // ponytail: a box-filtered alpha thins a far tree out; doubled here instead of a per-level cover fit
                // (a model is a pixel or two at these levels).
                for (size_t k = 3; k < enc.size(); k += 4) {
                    enc[k] = std::min(1.0f, enc[k] * 2.0f);
                }
            }

            for (uint32_t by = y0; by < std::max(y1, y0 + 4) && by < w; by += 4) {
                for (uint32_t bx = 0; bx < w; bx += 4) {
                    EncodeBlock(kind, &enc[(size_t(by) * w + bx) * 4], w, l, bx, by);
                }
            }
        }
    }
}

bool AllocCells(Model& model) {
    const uint32_t n = model.big ? 2 : 1;

    // Big models from the bottom up, small ones from the top down, so neither breaks the other's runs.
    for (uint32_t i = 0; i + n <= kRows; i += n) {
        const uint32_t r = model.big ? kRows - n - i : i;

        for (uint32_t c = 0; c + n <= kCols; c += n) {
            bool free = true;

            for (uint32_t k = 0; k < n * n && free; k++) {
                free = !s_fol.cells[(r + k / n) * kCols + c + k % n];
            }

            if (free) {
                for (uint32_t k = 0; k < n * n; k++) {
                    s_fol.cells[(r + k / n) * kCols + c + k % n] = true;
                }

                model.cellCol = static_cast<uint16_t>(c);
                model.cellRow = static_cast<uint16_t>(r);
                return true;
            }
        }
    }

    return false;
}

void FreeCells(const Model& model) {
    const uint32_t n = model.big ? 2 : 1;

    for (uint32_t k = 0; k < n * n; k++) {
        s_fol.cells[(model.cellRow + k / n) * kCols + model.cellCol + k % n] = false;
    }
}

void EnsureAtlases() {
    if (s_fol.chain[0]) {
        return;
    }

    for (uint32_t kind = 0; kind < 2; kind++) {
        uint32_t size = 0;

        for (uint32_t l = 0; l < kAtlasLevels; l++) {
            s_fol.levelOffset[kind][l] = size;
            size += (kAtlasPx >> l) * (kAtlasPx >> l) / (kind ? 2 : 1);  // DXT5 a byte a pixel, DXT1 half
        }

        s_fol.chain[kind] = new uint8_t[size]();
        s_fol.small[kind].assign(size_t(kAtlasPx >> kSmallFrom) * (kAtlasPx >> kSmallFrom) * 4, 0.0f);
        const EGxTexFormat format = kind ? GxTex_Dxt1 : GxTex_Dxt5;
        s_fol.atlas[kind] = TextureCreate(kAtlasPx, kAtlasPx, format, format, CGxTexFlags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1), reinterpret_cast<void*>(uintptr_t(kind)), AtlasUserFunc, kind ? "FarFoliageNormals" : "FarFoliageAtlas", 0);
    }
}

// Queued pictures, a view at a time, under kBakeMs a frame (a big tree's view at 512 px raster is a few ms).
void PumpPictures() {
    const uint32_t start = OsGetAsyncTimeMs();

    while (OsGetAsyncTimeMs() - start < kBakeMs) {
        BakeJob& job = s_fol.job;

        if (job.model == ~0u) {
            if (s_fol.queue.empty()) {
                return;
            }

            const uint32_t id = s_fol.queue.front();
            s_fol.queue.erase(s_fol.queue.begin());
            Model& model = s_fol.models[id];
            const float frameEst = std::max(model.hi[2] - model.lo[2], 2.0f * std::max({ fabsf(model.lo[0]), fabsf(model.hi[0]), fabsf(model.lo[1]), fabsf(model.hi[1]) }));
            model.big = frameEst * model.maxScale >= kBigYards;

            // ponytail: the atlas full, the model goes without, as wraithhost's kImpMax (512 small / 128 big).
            if (!AllocCells(model)) {
                model.pic = Pic_Failed;
                continue;
            }

            EnsureAtlases();
            const uint32_t t0 = OsGetAsyncTimeMs();

            if (!PrepareJob(model, job)) {
                FreeCells(model);
                model.pic = Pic_Failed;
                job = BakeJob {};
                continue;
            }

            job.model = id;
            job.ms = OsGetAsyncTimeMs() - t0;
            continue;
        }

        Model& model = s_fol.models[job.model];
        const uint32_t t0 = OsGetAsyncTimeMs();
        BakeView(job, model, job.view++);
        job.ms += OsGetAsyncTimeMs() - t0;

        if (job.view < kViews) {
            continue;
        }

        if (!job.anyDrawn) {
            FreeCells(model);
            model.pic = Pic_Failed;
        } else {
            const uint32_t n = model.big ? 2 : 1;
            const uint32_t t1 = OsGetAsyncTimeMs();
            StoreSmallLevels(model.cellRow, n);
            model.frame = job.frame;
            model.bottom = job.zLo;
            model.pic = Pic_Baked;
            s_fol.bakeGen++;
            const uint32_t x = model.cellCol * kCellPx * kViews, y = model.cellRow * kCellPx;
            const bool fresh = s_fol.baked == 0;

            for (uint32_t kind = 0; kind < 2; kind++) {
                if (CGxTex* gxTex = TextureGetGxTex(s_fol.atlas[kind], 0, nullptr)) {
                    // ponytail: levels 5..10 change across the atlas width on this model's rows only; the rect
                    // update reaches them through the model's own rectangle.
                    GxTexUpdate(gxTex, fresh ? 0 : x, fresh ? 0 : y, fresh ? kAtlasPx : x + n * kCellPx * kViews, fresh ? kAtlasPx : y + n * kCellPx, 1);
                }
            }

            job.ms += OsGetAsyncTimeMs() - t1;
            s_fol.baked++;
            s_fol.bakeMs += job.ms;
        }

        job = BakeJob {};
    }
}

bool EnsureIndices() {
    if (!s_fol.indexBuf) {
        s_fol.indexPool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Index, GxPoolUsage_Static, kChunkCards * 6 * sizeof(uint16_t), GxPoolHintBit_Unk0, "FarFoliageIB");
        s_fol.indexBuf = g_theGxDevicePtr->BufCreate(s_fol.indexPool, sizeof(uint16_t), kChunkCards * 6, 0);
    }

    if (!s_fol.indexBuf) {
        return false;
    }

    if (s_fol.indexBuf->m_hasData && s_fol.indexBuf->m_valid) {
        return true;
    }

    uint16_t* index = reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(s_fol.indexBuf));

    for (uint32_t q = 0; q < kChunkCards; q++) {
        const uint16_t v = static_cast<uint16_t>(q * 4);
        const uint16_t quad[6] = { v, static_cast<uint16_t>(v + 1), static_cast<uint16_t>(v + 2), v, static_cast<uint16_t>(v + 2), static_cast<uint16_t>(v + 3) };
        memcpy(index + q * 6, quad, sizeof(quad));
    }

    g_theGxDevicePtr->BufUnlock(s_fol.indexBuf, 0);
    s_fol.indexBuf->m_hasData = 1;
    return true;
}

C3Vector BlockOrigin(uint32_t b) {
    return C3Vector(kOrigin - ((b / 8) * 8 + 4) * kTileYd, kOrigin - ((b % 8) * 8 + 4) * kTileYd, 0.0f);
}

void FreeChunk(Chunk& c) {
    if (c.buf) {
        g_theGxDevicePtr->BufDestroy(c.buf);
    }

    if (c.pool) {
        g_theGxDevicePtr->PoolDestroy(c.pool);
    }

    c = Chunk {};
}

// Block b's cards, sorted by size class (one draw per class and chunk). The buffer holds where each card stands
// and which pictures it has; the shader turns it to the camera and picks the views, so it is built once (and
// again only when a missing picture is baked or the device drops it), never because the camera moved.
void BuildBlock(uint32_t b) {
    Block& block = s_fol.blocks[b];
    std::vector<const Placed*> cards;
    cards.reserve(block.placed.size());
    block.missing = false;

    for (const Placed& p : block.placed) {
        const uint8_t pic = s_fol.models[p.model].pic;

        if (pic == Pic_Baked) {
            cards.push_back(&p);
        } else if (pic != Pic_Failed) {
            block.missing = true;
        }
    }

    std::stable_sort(cards.begin(), cards.end(), [](const Placed* a, const Placed* c) { return a->cls < c->cls; });
    const uint32_t chunks = static_cast<uint32_t>((cards.size() + kChunkCards - 1) / kChunkCards);

    while (block.chunks.size() > chunks) {
        FreeChunk(block.chunks.back());
        block.chunks.pop_back();
    }

    block.chunks.resize(chunks);
    const C3Vector origin = BlockOrigin(b);

    for (uint32_t ci = 0; ci < chunks; ci++) {
        Chunk& c = block.chunks[ci];
        const uint32_t first = ci * kChunkCards;
        const uint32_t need = std::min(kChunkCards, static_cast<uint32_t>(cards.size()) - first);

        // A fresh static buffer each build (rare: a picture baked), never a relock of one the GPU may be reading.
        {
            FreeChunk(c);
            c.capacity = std::min(kChunkCards, (need + 1023) & ~1023u);
            c.pool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Vertex, GxPoolUsage_Static, c.capacity * 4 * sizeof(CGxVertexPNCT), GxPoolHintBit_Unk0, "FarFoliageVB");
            c.buf = g_theGxDevicePtr->BufCreate(c.pool, sizeof(CGxVertexPNCT), c.capacity * 4, 0);
        }

        c.cards = need;

        if (!c.buf) {
            c.cards = 0;
            continue;
        }

        memset(c.start, 0, sizeof(c.start));
        memset(c.count, 0, sizeof(c.count));
        CGxVertexPNCT* v = reinterpret_cast<CGxVertexPNCT*>(g_theGxDevicePtr->BufLock(c.buf));

        for (uint32_t k = 0; k < c.cards; k++) {
            const Placed& p = *cards[first + k];
            const Model& model = s_fol.models[p.model];

            if (!c.count[p.cls]) {
                c.start[p.cls] = k;
            }

            c.count[p.cls]++;
            // FarFoliageVs.hlsl's layout: normal x = corner side x (1 + turn), y = corner height + 2 (big), z = side.
            float turn = p.yaw / 6.2831853f;
            turn -= floorf(turn);
            const float side = model.frame * p.scale;
            const C3Vector base(p.x - origin.x, p.y - origin.y, p.z + model.bottom * p.scale);
            const C2Vector uv(float(model.cellCol * kCellPx * kViews) / kAtlasPx, float(model.cellRow * kCellPx) / kAtlasPx);
            const float big = model.big ? 2.0f : 0.0f;
            const float sx[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
            const float sy[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

            for (uint32_t i = 0; i < 4; i++) {
                v[k * 4 + i].m_position = base;
                v[k * 4 + i].m_normal = C3Vector(sx[i] * (1.0f + turn), sy[i] + big, side);
                v[k * 4 + i].m_color = CImVector(p.tint[0], p.tint[1], p.tint[2], p.tint[3]);
                v[k * 4 + i].m_texture = uv;
            }
        }

        g_theGxDevicePtr->BufUnlock(c.buf, 0);
        c.buf->m_hasData = 1;
    }

    block.builtGen = s_fol.bakeGen;
    block.built = true;
}

// Whether the box (camera-relative) can be on screen under viewProj: not wholly past one side plane, or behind.
bool BoxOnScreen(const C44Matrix& m, const C3Vector& lo, const C3Vector& hi) {
    uint32_t all = 0x1F;

    for (uint32_t i = 0; i < 8; i++) {
        const float x = i & 1 ? hi.x : lo.x, y = i & 2 ? hi.y : lo.y, z = i & 4 ? hi.z : lo.z;
        const float cx = x * m.a0 + y * m.b0 + z * m.c0 + m.d0;
        const float cy = x * m.a1 + y * m.b1 + z * m.c1 + m.d1;
        const float cw = x * m.a3 + y * m.b3 + z * m.c3 + m.d3;
        const uint32_t out = (cx < -cw ? 1u : 0u) | (cx > cw ? 2u : 0u) | (cy < -cw ? 4u : 0u) | (cy > cw ? 8u : 0u) | (cw <= 0.0f ? 16u : 0u);
        all &= out;
    }

    return all == 0;
}

std::string FarFoliageStatsLine(size_t cards, uint32_t cellsUsed) {
    size_t bufs = 0;

    for (const Block& block : s_fol.blocks) {
        for (const Chunk& c : block.chunks) {
            bufs += size_t(c.capacity) * 4 * sizeof(CGxVertexPNCT);
        }
    }

    size_t chain = 0;

    for (uint32_t l = 0; l < kAtlasLevels; l++) {
        chain += size_t(kAtlasPx >> l) * (kAtlasPx >> l) * 3 / 2;  // DXT5 + DXT1
    }

    char line[320];
    SStrPrintf(line, sizeof(line), "farTrees: %u models, %u pictures baked (%u ms, %.1f ms each), %u queued; %u of %u atlas cells; %u cards; %u draws; atlases %.1f MB (x2: CPU copy + GPU), cards %.1f MB",
        static_cast<uint32_t>(s_fol.models.size()), s_fol.baked, s_fol.bakeMs, s_fol.baked ? float(s_fol.bakeMs) / s_fol.baked : 0.0f,
        static_cast<uint32_t>(s_fol.queue.size()), cellsUsed, kCols * kRows, static_cast<uint32_t>(cards), s_fol.draws,
        (s_fol.chain[0] ? chain : 0) / 1048576.0f, bufs / 1048576.0f);
    return line;
}

}  // namespace

void FarField::FoliageScanTile(uint32_t tile, const uint8_t* b, uint32_t n) {
    const uint32_t kMMDX = 0x4D4D4458;
    const uint32_t kMMID = 0x4D4D4944;
    const uint32_t kMDDF = 0x4D444446;
    const uint32_t kMCNK = 0x4D434E4B;
    const uint8_t *mmdx = nullptr, *mmid = nullptr, *mddf = nullptr;
    uint32_t mmdxLen = 0, mmidLen = 0, mddfLen = 0;
    const uint8_t* mcnk[256] = {};
    uint32_t mcnkLen[256] = {};

    for (uint32_t o = 0; o + 8 <= n;) {
        const uint32_t magic = Rd32(b + o);
        const uint32_t size = Rd32(b + o + 4);

        if (o + 8ull + size > n) {
            break;
        }

        if (magic == kMMDX) {
            mmdx = b + o + 8;
            mmdxLen = size;
        } else if (magic == kMMID) {
            mmid = b + o + 8;
            mmidLen = size;
        } else if (magic == kMDDF) {
            mddf = b + o + 8;
            mddfLen = size;
        } else if (magic == kMCNK && size >= 0x80) {
            const uint32_t ix = Rd32(b + o + 8 + 4), iy = Rd32(b + o + 8 + 8);

            if (ix < 16 && iy < 16) {
                mcnk[iy * 16 + ix] = b + o;
                mcnkLen[iy * 16 + ix] = size + 8;
            }
        }

        o += 8 + size;
    }

    if (!mmdx || !mmid || !mddf) {
        return;
    }

    for (uint32_t k = 0; k + 36 <= mddfLen; k += 36) {
        const uint8_t* e = mddf + k;
        const uint32_t id = Rd32(e);

        if (uint64_t(id) * 4 + 4 > mmidLen || Rd32(mmid + id * 4) >= mmdxLen) {
            continue;
        }

        const uint32_t at = Rd32(mmid + id * 4);
        std::string key(reinterpret_cast<const char*>(mmdx + at), strnlen(reinterpret_cast<const char*>(mmdx + at), mmdxLen - at));

        for (char& c : key) {
            c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        }

        auto it = s_fol.byName.find(key);
        uint32_t mi;

        if (it == s_fol.byName.end()) {
            // Trees by name as the art is filed (wraithhost ModelInfo); bushes by their own name.
            Model model;
            const bool treeish = Has(key, "TREE") || Has(key, "PALM") || Has(key, "KALIDARBUSH") || Has(key, "FELWOODBUSH");
            model.bush = IsBushName(key) && !Has(key, "KALIDARBUSH") && !Has(key, "FELWOODBUSH");
            model.tree = treeish && !(Has(key, "STREET") || (Has(key, "LEAVES") && !Has(key, "NOLEAVES")) || Has(key, "STUMP") || Has(key, "LOG") || (Has(key, "ROOT") && !Has(key, "TREEROOT")));

            if (model.tree || model.bush) {
                model.file = key;
                const size_t dot = model.file.find_last_of('.');

                if (dot != std::string::npos) {
                    model.file.resize(dot);
                }

                model.file += ".M2";  // .mdx / .mdl in old placements
                std::vector<uint8_t> head;

                if (ReadAll(model.file.c_str(), head, 0xBC) && head.size() >= 0xBC && !memcmp(head.data(), "MD20", 4)) {
                    for (int j = 0; j < 3; j++) {
                        model.lo[j] = RdF(&head[0xA0 + j * 4]);
                        model.hi[j] = RdF(&head[0xAC + j * 4]);
                    }

                    const float height = model.hi[2] - model.lo[2];

                    if (!(height > 0.5f) || height > 500.0f) {
                        model.tree = model.bush = false;
                    }
                } else {
                    model.tree = model.bush = false;
                }
            }

            mi = static_cast<uint32_t>(s_fol.models.size());
            s_fol.models.push_back(model);
            s_fol.byName.emplace(key, mi);
        } else {
            mi = it->second;
        }

        Model& model = s_fol.models[mi];

        if (!model.tree && !model.bush) {
            continue;
        }

        model.maxScale = std::max(model.maxScale, float(Rd16(e + 32)) / 1024.0f);

        const float scale = float(Rd16(e + 32)) / 1024.0f;
        const float placedH = (model.hi[2] - model.lo[2]) * scale;
        const bool asTree = model.tree && placedH >= kTreeMinHeight;
        const bool asBush = model.bush && placedH >= kBushMinHeight && !asTree;

        // A placement is listed in every tile its model reaches into: one card.
        if ((!asTree && !asBush) || !s_fol.uids.insert(Rd32(e + 4)).second) {
            continue;
        }

        Placed p;
        p.x = kOrigin - RdF(e + 16);
        p.y = kOrigin - RdF(e + 8);
        p.z = RdF(e + 12);
        p.yaw = (RdF(e + 24) + 180.0f) * 0.017453292f;
        p.scale = scale;
        p.model = mi;

        // Its stock size class: the yaw-turned box's largest side (MapDoodadDef.cpp:85).
        const float c = fabsf(cosf(p.yaw)), s = fabsf(sinf(p.yaw));
        const float ex = (model.hi[0] - model.lo[0]) * scale, ey = (model.hi[1] - model.lo[1]) * scale;
        const float size = std::max({ ex * c + ey * s, ex * s + ey * c, placedH });
        p.cls = 0;

        while (p.cls < 4 && size > CWorldView::s_fadeSize[p.cls]) {
            p.cls++;
        }

        const int32_t row = std::clamp(static_cast<int32_t>((kOrigin - p.x) / kTileYd), 0, 63);
        const int32_t col = std::clamp(static_cast<int32_t>((kOrigin - p.y) / kTileYd), 0, 63);
        // The ground under it, from this ADT's chunk (a placement first listed by a neighbour keeps no tint):
        // MCCV's nearest outer vertex, and MCSH's bit.
        if (static_cast<uint32_t>(row * 64 + col) == tile) {
            const float kChunkYd = kTileYd / 16.0f;
            const float lx = ((kOrigin - p.y) - col * kTileYd) / kChunkYd, ly = ((kOrigin - p.x) - row * kTileYd) / kChunkYd;
            const int32_t ix = std::clamp(static_cast<int32_t>(lx), 0, 15), iy = std::clamp(static_cast<int32_t>(ly), 0, 15);
            const float fx = std::clamp(lx - ix, 0.0f, 1.0f), fy = std::clamp(ly - iy, 0.0f, 1.0f);

            if (const uint8_t* c = mcnk[iy * 16 + ix]) {
                const uint32_t len = mcnkLen[iy * 16 + ix];
                const uint32_t flags = Rd32(c + 8), ofsCv = Rd32(c + 8 + 0x74), ofsSh = Rd32(c + 8 + 0x2C);

                if (ofsCv && uint64_t(ofsCv) + 8 + 145 * 4 <= len && Rd32(c + ofsCv) == 0x4D434356) {
                    const uint8_t* cv = c + ofsCv + 8 + (lroundf(fy * 8.0f) * 17 + lroundf(fx * 8.0f)) * 4;
                    p.tint[0] = cv[0];
                    p.tint[1] = cv[1];
                    p.tint[2] = cv[2];
                }

                if ((flags & 1) && ofsSh && uint64_t(ofsSh) + 8 + 512 <= len && Rd32(c + ofsSh) == 0x4D435348) {
                    const uint32_t sx = std::min(63u, static_cast<uint32_t>(fx * 64.0f)), sy = std::min(63u, static_cast<uint32_t>(fy * 64.0f));

                    if ((c[ofsSh + 8 + sy * 8 + sx / 8] >> (sx % 8)) & 1) {
                        p.tint[3] = 0;
                    }
                }
            }
        }

        Block& block = s_fol.blocks[(row / 8) * 8 + col / 8];
        block.placed.push_back(p);
        const float reachEst = std::max({ fabsf(model.lo[0]), fabsf(model.hi[0]), fabsf(model.lo[1]), fabsf(model.hi[1]), 0.5f * (model.hi[2] - model.lo[2]) }) * scale;
        block.lo[0] = std::min(block.lo[0], p.x - reachEst);
        block.lo[1] = std::min(block.lo[1], p.y - reachEst);
        block.lo[2] = std::min(block.lo[2], p.z + model.lo[2] * scale);
        block.hi[0] = std::max(block.hi[0], p.x + reachEst);
        block.hi[1] = std::max(block.hi[1], p.y + reachEst);
        block.hi[2] = std::max(block.hi[2], p.z + std::max(model.hi[2] * scale, 2.0f * reachEst));

        if (model.pic == Pic_None) {
            model.pic = Pic_Queued;
            s_fol.queue.push_back(mi);
        }
    }

}

void FarField::FoliageReset() {
    if (!g_theGxDevicePtr) {
        return;
    }

    for (Block& block : s_fol.blocks) {
        for (Chunk& c : block.chunks) {
            FreeChunk(c);
        }

        block = Block {};
    }

    for (uint32_t kind = 0; kind < 2; kind++) {
        if (s_fol.atlas[kind]) {
            HandleClose(reinterpret_cast<HOBJECT>(s_fol.atlas[kind]));
        }

        delete[] s_fol.chain[kind];
        s_fol.chain[kind] = nullptr;
        s_fol.atlas[kind] = nullptr;
        s_fol.small[kind].clear();
    }

    memset(s_fol.cells, 0, sizeof(s_fol.cells));
    s_fol.job = BakeJob {};
    s_fol.byName.clear();
    s_fol.models.clear();
    s_fol.queue.clear();
    s_fol.uids.clear();
    s_fol.baked = 0;
    s_fol.bakeMs = 0;
    s_fol.bakeGen++;
}

void FarField::RenderFoliage(bool horizon) {
    if (!FarTreesCVar()->m_intValue || FarField::FogStretch() == 1.0f) {
        return;
    }

    if (!s_fol.shadersTried) {
        s_fol.shadersTried = true;
        g_theGxDevicePtr->ShaderCreate(&s_fol.vs, GxSh_Vertex, "Shaders\\Vertex", "FarFoliage", 1);
        g_theGxDevicePtr->ShaderCreate(&s_fol.ps, GxSh_Pixel, "Shaders\\Pixel", "FarFoliagePs", 1);
    }

    if (!s_fol.vs || !s_fol.vs->Valid() || !s_fol.ps || !s_fol.ps->Valid()) {
        return;
    }

    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float farZ = CGWorldFrame::GetActiveCamera()->m_farZ;

    if (!horizon) {
        PumpPictures();
        s_fol.draws = 0;
    }

    if (!s_fol.atlas[0] || !s_fol.atlas[1] || !EnsureIndices()) {
        return;
    }

    CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_View];
    const C44Matrix view = stack.m_mtx[stack.m_level];
    const C44Matrix viewProj = view * g_theGxDevicePtr->m_projNative;

    // Blocks in this pass's depth range and on screen, front to back. Each block is drawn in exactly the pass
    // its distance range calls for; a block straddling farZ is drawn in both, the main pass's copy winning the
    // depth test where they overlap (the same card at the same place, never a fight).
    uint32_t order[64];
    float nearD[64];
    float farD[64];
    uint32_t count = 0;
    uint32_t rebuilds = 0;

    for (uint32_t b = 0; b < 64; b++) {
        Block& block = s_fol.blocks[b];

        if (block.placed.empty()) {
            continue;
        }

        const float dx = std::max({ block.lo[0] - cam.x, cam.x - block.hi[0], 0.0f });
        const float dy = std::max({ block.lo[1] - cam.y, cam.y - block.hi[1], 0.0f });
        const float nd = sqrtf(dx * dx + dy * dy);
        const float fx = std::max(fabsf(cam.x - block.lo[0]), fabsf(cam.x - block.hi[0]));
        const float fy = std::max(fabsf(cam.y - block.lo[1]), fabsf(cam.y - block.hi[1]));
        const float fd = sqrtf(fx * fx + fy * fy);

        if (horizon ? fd < farZ - 50.0f : nd > farZ) {
            continue;
        }

        if (!BoxOnScreen(viewProj, C3Vector(block.lo[0], block.lo[1], block.lo[2]) - cam, C3Vector(block.hi[0], block.hi[1], block.hi[2]) - cam)) {
            continue;
        }

        uint32_t i = count++;

        for (; i > 0 && nearD[i - 1] > nd; i--) {
            order[i] = order[i - 1];
            nearD[i] = nearD[i - 1];
            farD[i] = farD[i - 1];
        }

        order[i] = b;
        nearD[i] = nd;
        farD[i] = fd;
    }

    for (uint32_t i = 0; i < count; i++) {
        Block& block = s_fol.blocks[order[i]];
        bool lost = false;

        for (Chunk& c : block.chunks) {
            lost = lost || !c.buf || !c.buf->m_hasData || !c.buf->m_valid;
        }

        // A picture baked since the build: at most two such rebuilds a frame, nearest first (main pass only, so
        // a block never changes between the two passes of one frame).
        const bool stale = block.missing && block.builtGen != s_fol.bakeGen;

        if (!block.built || lost || (stale && !horizon && rebuilds < 2)) {
            BuildBlock(order[i]);
            rebuilds++;
        }
    }

    // Where each size class's model starts to fade (or farclip, past which no doodad is drawn), less the chunk
    // slack (the stock cull goes by the chunk's distance) and measured as view depth at the screen's edge (the
    // shader fades by view depth, the game by distance): the card is whole there, and comes in over one band.
    // The card is whole before its tree starts the stock alpha fade, and opaque, so the fading tree blends over its
    // own picture: tree x a + card x (1 - a), an exact crossfade where the card covers, with no gap or darkening.
    const C44Matrix& proj = g_theGxDevicePtr->m_projNative;
    const float tanX = proj.a0 != 0.0f ? 1.0f / fabsf(proj.a0) : 1.0f;
    const float cosEdge = 1.0f / sqrtf(1.0f + tanX * tanX);
    struct { float x, y, z, w; } fade[5];
    float fadeFrom[5];

    for (uint32_t c = 0; c < 5; c++) {
        const float end = std::min(CWorldScene::s_fadeDistMin[c] - 24.0f, farZ - 35.0f);
        const float whole = std::max(end * cosEdge, 10.0f);
        const float band = std::max(20.0f, whole * 0.1f);
        fadeFrom[c] = whole - band;
        fade[c].x = 1.0f / band;
        fade[c].y = -fadeFrom[c] / band;
        fade[c].z = 1.0f / 300.0f;   // tint ramp
        fade[c].w = std::min(CWorldScene::s_fadeDistMin[c] + CWorldScene::s_fadeRangeDefault[c], farZ);
    }

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);   // crossfade opacity with near trees
    GxRsSet(GxRs_DepthWrite, 1);
    GxRsSet(GxRs_DepthTest, 3);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Fog, 0);                         // fogged in the shader, the terrain's formula
    GxRsSet(GxRs_VertexShader, s_fol.vs);
    GxRsSet(GxRs_PixelShader, s_fol.ps);
    GxRsSet(GxRs_Texture0, static_cast<void*>(TextureGetGxTex(s_fol.atlas[0], 0, nullptr)));
    GxRsSet(GxRs_Texture1, static_cast<void*>(TextureGetGxTex(s_fol.atlas[1], 0, nullptr)));

    // The terrain's own constants for this frame (projection bound now, DNInfo fog in c12, sun c24-c26): the
    // cards fog and light exactly as the far ground does.
    CMapRenderChunk::SVertexShaderConstants consts;
    FarField::TerrainConstants(consts);
    const DayNight::DNInfo* info = DayNight::GetInfo();
    const C4Vector fogColour(info->m_fog.m_color);
    const float pixel[16] = {
        consts.m_sunAmbient.x, consts.m_sunAmbient.y, consts.m_sunAmbient.z, 1.0f,
        consts.m_sunDiffuse.x, consts.m_sunDiffuse.y, consts.m_sunDiffuse.z, 1.0f,
        fogColour.x, fogColour.y, fogColour.z, 1.0f,
        kTintStrength, kShadowLevel, 0.0f, 0.0f,
    };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 3, pixel, 4);
    const float card[4] = { float(kCellPx) / kAtlasPx, float(kCellPx * 2) / kAtlasPx, 0.0f, 0.0f };
    bool uploaded = false;
    int32_t boundFade = -1;

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t b = order[i];
        Block& block = s_fol.blocks[b];
        const C3Vector origin = BlockOrigin(b);

        C44Matrix world;
        world.Translate(origin - cam);
        consts.m_worldView = world * view;
        const float camLocal[4] = { cam.x - origin.x, cam.y - origin.y, cam.z - origin.z, 0.0f };

        if (!uploaded) {
            // c0..c26 (to m_sunDiffuse), then ours over c8..c10.
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&consts), offsetof(CMapRenderChunk::SVertexShaderConstants, m_sunSpecular) / sizeof(C4Vector));
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 10, card, 1);
            uploaded = true;
        } else {
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&consts.m_worldView), 4);
        }

        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 8, camLocal, 1);

        for (Chunk& c : block.chunks) {
            if (!c.cards || !c.buf) {
                continue;
            }

            bool bound = false;

            for (uint32_t cls = 0; cls < 5; cls++) {
                // Wholly nearer than where this class's cards begin: every pixel would be faded out.
                if (!c.count[cls] || farD[i] < fadeFrom[cls]) {
                    continue;
                }

                if (!bound) {
                    GxPrimVertexPtr(c.buf, GxVBF_PNCT);
                    g_theGxDevicePtr->PrimIndexPtr(s_fol.indexBuf);
                    bound = true;
                }

                if (boundFade != static_cast<int32_t>(cls)) {
                    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 9, &fade[cls].x, 1);
                    boundFade = static_cast<int32_t>(cls);
                }

                CGxBatch batch;
                batch.m_primType = GxPrim_Triangles;
                batch.m_start = c.start[cls] * 6;
                batch.m_count = c.count[cls] * 6;
                batch.m_minIndex = c.start[cls] * 4;
                batch.m_maxIndex = (c.start[cls] + c.count[cls]) * 4 - 1;
                g_theGxDevicePtr->Draw(&batch, 1);
                s_fol.draws++;
            }
        }
    }

    g_theGxDevicePtr->RsPop();

    if (horizon && FarFoliageStatsCVar()->m_intValue) {
        static uint32_t s_last;
        const uint32_t now = OsGetAsyncTimeMs();

        if (now - s_last > 2000) {
            s_last = now;
            size_t cards = 0;

            for (const Block& block : s_fol.blocks) {
                cards += block.placed.size();
            }

            uint32_t used = 0;

            for (bool cell : s_fol.cells) {
                used += cell;
            }

            ConsoleWrite(FarFoliageStatsLine(cards, used).c_str(), DEFAULT_COLOR);
        }
    }
}

bool FarField::GrassSizeCull() {
    return FarGrassCVar()->m_intValue != 0;
}

float FarField::GrassCutDistance(float height) {
    // A tuft `height` yd tall covers height x proj.b1 x (viewport height / 2) / depth pixels; it is cut where
    // that falls under farGrassMinPixels. Distance, FOV and resolution all come in through this one line.
    const float minPx = std::max(FarGrassMinPixelsCVar()->m_floatValue, 0.25f);
    const float vpH = (g_theGxDevicePtr->m_viewport.m_y.m_h - g_theGxDevicePtr->m_viewport.m_y.m_l) * static_cast<float>(g_theGxDevicePtr->m_format.m_size.y);
    return height * fabsf(g_theGxDevicePtr->m_projNative.b1) * vpH * 0.5f / minPx;
}
