// Not in 12340 (farTrees, farGrass): far trees and bushes as cards of their own models, and the ground-effect
// grass culled by its size on screen. docs/FARFIELD_FOG.md "Far foliage".
//
// A port of wxl-farfield's far foliage (/root/wxl-farbush-wt/wxl-farfield/src/FarFoliage.cpp) and of the half
// of it wxl-wraithhost bakes (wraithhost-lite/src/host/terrain/FarLod.cpp: which placements are trees and
// bushes; Impostor.cpp: the pictures). wraithhost does not load in Wow32/Wow64, so both bakes run here:
//
// - Placements: every ADT the far-ground bake reads (MapLowDetail.cpp PumpBake) is handed to ScanTile, which
//   takes its MDDF placements whose model is a tree or a bush by wraithhost's name rules and size limits.
// - Pictures: each such model's skin-00 mesh and textures rasterised in software, orthographic, from 4 sides
//   (every 90 deg about +z), bottom-aligned in a square of side max(height, 2 x reach), as wraithhost does it;
//   one model a frame, into one 4096 x 2048 DXT5 atlas (64 px a view, 5 levels a view, 512 models).
// - Cards: per 8 x 8-tile block, one camera-facing quad per placement showing the view nearest the camera,
//   sized by the picture's frame x the placement's scale, in a vertex buffer rebuilt only when the camera has
//   moved far enough for the facing to be off by more than ~15 deg (far blocks almost never).
// - Drawing: the stock DetailDoodad shaders (the ground-effect grass's), which take world-space PNCT, light
//   by the sun, fog with the same formula and the same DNInfo fog as the terrain (vertex c8 here, c12 there)
//   and multiply the texture's alpha by a linear distance fade (vertex c9). Nothing new in custom.MPQ, and
//   alpha test (GxRs_AlphaRef) works on all three backends because the grass already relies on it.
// - Hand-over (feedback_sky_before_blended_far_passes): a card is drawn alpha-tested with depth write (a
//   screen door, never blended), and is whole one band BEFORE the place its model starts to fade: per stock
//   size class (CWorldView::s_fadeSize) where CWorldScene::s_fadeDistMin says, or farclip if nearer. The
//   picture's alpha holds a per-texel noise above the key, so the distance fade dissolves the card.
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
constexpr uint32_t kViewPx = 64;      // wraithhost impostor::kPx
constexpr uint32_t kViewLevels = 5;   // 64 .. 4 px
constexpr uint32_t kViews = 4;        // every 90 deg
constexpr uint32_t kRaster = kViewPx * 2;
constexpr uint32_t kAtlasW = 4096;
constexpr uint32_t kAtlasH = 2048;
constexpr uint32_t kCols = kAtlasW / (kViewPx * kViews);   // 16 models a row
constexpr uint32_t kMaxModels = kCols * (kAtlasH / kViewPx);  // 512 (wraithhost kImpMax)
constexpr uint32_t kAtlasLevels = 10;  // 4096 x 2048 .. 8 x 4: the DXT chain ITexWHDStartEnd asks for
constexpr uint32_t kChunkCards = 16384;  // 65536 vertices: 16-bit indices
constexpr float kTreeMinHeight = 6.0f;   // wraithhost FarLod.hpp: yards, as placed
constexpr float kBushMinHeight = 1.5f;
constexpr float kBushCover = 0.95f;      // a bush needs a texture cut out somewhere (leaves), not a solid prop
constexpr uint8_t kAlphaKey = 128;       // GxRs_AlphaRef; opaque texels hold 136..255 (noise), the rest 0

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
    float lo[3] = {};   // header box (model units)
    float hi[3] = {};
    uint8_t pic = Pic_None;
    uint16_t slot = 0;
    float frame = 0.0f;   // the picture's side (model units)
    float bottom = 0.0f;  // model z at its bottom edge
};

struct Placed {
    float x, y, z;
    float yaw;     // radians, the model's +x in the world
    float scale;
    uint32_t model;
    uint8_t cls;   // stock size class (MapDoodadDef.cpp: largest side of the world box against s_fadeSize)
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
    float lo[2] = { 1e30f, 1e30f };
    float hi[2] = { -1e30f, -1e30f };
    C3Vector builtAt;
    uint32_t builtGen = 0;   // s_fol.bakeGen at the build
    bool built = false;
    bool missing = false;    // some placement's picture was not baked yet at the build
};

struct FolState {
    std::unordered_map<std::string, uint32_t> byName;
    std::vector<Model> models;
    std::vector<uint32_t> queue;   // models waiting for their picture, nearest tile's first
    std::unordered_set<uint32_t> uids;
    Block blocks[64];
    uint32_t slots = 0;
    uint32_t bakeGen = 1;
    uint8_t* chain = nullptr;
    uint32_t levelOffset[kAtlasLevels] = {};
    std::vector<float> level4;     // the atlas at level 4 (256 x 128) as rgba, for levels 5..9
    CTexture* atlas = nullptr;
    CGxPool* indexPool = nullptr;
    CGxBuf* indexBuf = nullptr;
};

FolState s_fol;

// --- Pictures (wraithhost Impostor.cpp) ---

struct Texture {
    std::vector<uint8_t> rgba;
    uint32_t w = 0;
    uint32_t h = 0;
};

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

// A texel's dissolve noise: opaque texels sit at 136..255, so the distance fade (alpha x fade against the key
// 128) takes them away a few at a time instead of all at once.
uint8_t NoiseAlpha(uint32_t x, uint32_t y, uint32_t level) {
    uint32_t h = x * 73856093u ^ y * 19349663u ^ level * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<uint8_t>(136 + (h & 0xFFFF) * 119 / 0xFFFF);
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

// One DXT5 block of rgb + coverage (4 floats a pixel, 0..1) at (bx, by) of level l, alpha keyed with noise.
void EncodeBlock(const float* px, uint32_t stride, uint32_t l, uint32_t x0, uint32_t y0) {
    float col[16][3];
    uint8_t a[16];

    for (uint32_t k = 0; k < 16; k++) {
        const float* q = px + (size_t(k / 4) * stride + k % 4) * 4;

        for (int j = 0; j < 3; j++) {
            col[k][j] = std::clamp(q[j], 0.0f, 1.0f) * 255.0f;
        }

        a[k] = q[3] >= 0.5f ? NoiseAlpha(x0 + k % 4, y0 + k / 4, l) : 0;
    }

    uint8_t* out = s_fol.chain + s_fol.levelOffset[l] + (size_t(y0 / 4) * ((kAtlasW >> l) / 4) + x0 / 4) * 16;
    EncodeDxt5Alpha(a, out);
    FarField::EncodeDxt1Block(col, out + 8);
}

void AtlasUserFunc(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    if (command == GxTex_Latch && s_fol.chain && mipLevel < kAtlasLevels) {
        texelStrideInBytes = (kAtlasW >> mipLevel) / 4 * 16;
        texels = s_fol.chain + s_fol.levelOffset[mipLevel];
    }
}

// Model m's 4 views into its atlas slot (Impostor.cpp Render, without the glow picture, the second texture of
// two-texture batches and the normal map: the stock shader lights a card per vertex). False when nothing drew
// or, for a bush, when no texture is cut out (a solid prop).
bool RenderPicture(Model& model) {
    std::vector<uint8_t> m;

    if (!ReadAll(model.file.c_str(), m) || m.size() < 0xBC || memcmp(m.data(), "MD20", 4)) {
        return false;
    }

    auto arr = [&](uint32_t at, uint32_t stride, uint32_t& n) -> const uint8_t* {
        n = Rd32(&m[at]);
        const uint32_t ofs = Rd32(&m[at + 4]);

        if (!n || uint64_t(ofs) + uint64_t(n) * stride > m.size()) {
            n = 0;
            return nullptr;
        }

        return &m[ofs];
    };
    uint32_t nVert = 0, nTex = 0, nMat = 0, nLookup = 0;
    const uint8_t* verts = arr(0x3C, 48, nVert);
    const uint8_t* texs = arr(0x50, 16, nTex);
    const uint8_t* mats = arr(0x70, 4, nMat);
    const uint8_t* lookup = arr(0x80, 2, nLookup);

    if (!verts) {
        return false;
    }

    std::string skinName = model.file.substr(0, model.file.size() - 3) + "00.skin";
    std::vector<uint8_t> skin;

    if (!ReadAll(skinName.c_str(), skin) || skin.size() < 0x30 || memcmp(skin.data(), "SKIN", 4)) {
        return false;
    }

    auto sarr = [&](uint32_t at, uint32_t stride, uint32_t& n) -> const uint8_t* {
        n = Rd32(&skin[at]);
        const uint32_t ofs = Rd32(&skin[at + 4]);

        if (!n || uint64_t(ofs) + uint64_t(n) * stride > skin.size()) {
            n = 0;
            return nullptr;
        }

        return &skin[ofs];
    };
    uint32_t nSkinVert = 0, nIdx = 0, nSub = 0, nBatch = 0;
    const uint8_t* skinVerts = sarr(0x04, 2, nSkinVert);
    const uint8_t* indices = sarr(0x0C, 2, nIdx);
    const uint8_t* subs = sarr(0x1C, 48, nSub);
    const uint8_t* batches = sarr(0x24, 24, nBatch);

    if (!skinVerts || !indices || !subs || !batches) {
        return false;
    }

    // The frame: tall enough for the model, wide enough for it turned any way about its axis.
    float zLo = 1e30f, zHi = -1e30f, reach = 0.0f;

    for (uint32_t v = 0; v < nVert; v++) {
        const uint8_t* p = verts + v * 48;
        const float x = RdF(p), y = RdF(p + 4), z = RdF(p + 8);
        zLo = std::min(zLo, z);
        zHi = std::max(zHi, z);
        reach = std::max(reach, sqrtf(x * x + y * y));
    }

    const float frame = std::max(zHi - zLo, 2.0f * reach);

    if (!(frame > 0.1f)) {
        return false;
    }

    // Textures by lookup slot, decoded once, at most 256 px across; the least alpha coverage seen (bushes).
    std::unordered_map<uint32_t, Texture> textures;
    float leastCover = 1.0f;
    auto texture = [&](uint32_t combo) -> const Texture* {
        if (!lookup || combo >= nLookup) {
            return nullptr;
        }

        const uint32_t t = Rd16(lookup + combo * 2);
        auto it = textures.find(t);

        if (it != textures.end()) {
            return it->second.rgba.empty() ? nullptr : &it->second;
        }

        Texture& tex = textures[t];

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

        while ((w0 >> level) > 256) {
            level++;
        }

        if (!DecodeBlp(blp, tex, level)) {
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

    struct Layer {
        uint32_t start, count, blend;
        const Texture* tex;
        float colour[4];
    };
    std::vector<Layer> layers;

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
        layers.push_back(l);
    }

    if (model.bush && !model.tree && leastCover >= kBushCover) {
        return false;
    }

    struct Pixel {
        float depth;
        float rgb[3];
        bool set;
    };
    std::vector<Pixel> px(size_t(kRaster) * kRaster);
    bool anyDrawn = false;
    const uint32_t slotX = (model.slot % kCols) * kViews * kViewPx;
    const uint32_t slotY = (model.slot / kCols) * kViewPx;

    for (uint32_t view = 0; view < kViews; view++) {
        // Looking from direction c toward the model's axis; right = (-c.y, c.x), up = +z.
        const float ang = float(view) * 1.5707963f;
        const float cx = cosf(ang), cy = sinf(ang);

        for (Pixel& p : px) {
            p = Pixel { -1e30f, { 0, 0, 0 }, false };
        }

        for (const Layer& L : layers) {
            for (uint32_t i = 0; i + 2 < L.count && L.start + i + 2 < nIdx; i += 3) {
                float sx[3], sy[3], sd[3], uv[3][2];
                bool ok = true;

                for (int k = 0; k < 3 && ok; k++) {
                    const uint32_t si = Rd16(indices + (L.start + i + k) * 2);
                    const uint32_t vi = si < nSkinVert ? Rd16(skinVerts + si * 2) : nVert;

                    if (vi >= nVert) {
                        ok = false;
                        break;
                    }

                    const uint8_t* v = verts + vi * 48;
                    const float x = RdF(v), y = RdF(v + 4), z = RdF(v + 8);
                    sx[k] = ((-cy * x + cx * y) / frame + 0.5f) * kRaster;
                    sy[k] = (zHi - z) / frame * kRaster + (frame - (zHi - zLo)) / frame * kRaster;  // bottom-aligned
                    sd[k] = cx * x + cy * y;
                    uv[k][0] = RdF(v + 32);
                    uv[k][1] = RdF(v + 36);
                }

                if (!ok) {
                    continue;
                }

                RasterTriangle(sx, sy, int(kRaster), int(kRaster), [&](int x, int y, float w0, float w1, float w2) {
                    const float depth = w0 * sd[0] + w1 * sd[1] + w2 * sd[2];
                    Pixel& p = px[size_t(y) * kRaster + x];

                    if (depth <= p.depth) {
                        return;
                    }

                    float rgb[3] = { 0.35f, 0.3f, 0.2f }, a = 1.0f;

                    if (L.tex) {
                        float u = w0 * uv[0][0] + w1 * uv[1][0] + w2 * uv[2][0];
                        float vv = w0 * uv[0][1] + w1 * uv[1][1] + w2 * uv[2][1];
                        u -= floorf(u);
                        vv -= floorf(vv);
                        const Texture* t = L.tex;
                        const uint8_t* q = &t->rgba[(size_t(std::min(uint32_t(vv * t->h), t->h - 1)) * t->w + std::min(uint32_t(u * t->w), t->w - 1)) * 4];

                        for (int j = 0; j < 3; j++) {
                            rgb[j] = q[j] / 255.0f;
                        }

                        a = L.blend == 0 ? 1.0f : q[3] / 255.0f;
                    }

                    // The game's own key: 224/255 for blend 1 (alpha-ref table 0x00AD8B7C), half for blend 2.
                    if (a < (L.blend == 1 ? 224.0f / 255.0f : 0.5f)) {
                        return;
                    }

                    for (int j = 0; j < 3; j++) {
                        p.rgb[j] = rgb[j] * L.colour[j];
                    }

                    p.depth = depth;
                    p.set = true;
                    anyDrawn = true;
                });
            }
        }

        // Down to 64 px: coverage in alpha, colour averaged over what drew; then the empty pixels take their
        // drawn neighbours' colour, so filtering never pulls a dark rim into the edge.
        std::vector<float> lv(size_t(kViewPx) * kViewPx * 4, 0.0f);

        for (uint32_t y = 0; y < kViewPx; y++) {
            for (uint32_t x = 0; x < kViewPx; x++) {
                float acc[3] = {};
                int n = 0;

                for (int k = 0; k < 4; k++) {
                    const Pixel& p = px[size_t(y * 2 + k / 2) * kRaster + x * 2 + k % 2];

                    if (p.set) {
                        for (int j = 0; j < 3; j++) {
                            acc[j] += p.rgb[j];
                        }

                        n++;
                    }
                }

                float* o = &lv[(size_t(y) * kViewPx + x) * 4];
                o[3] = n / 4.0f;

                for (int j = 0; n && j < 3; j++) {
                    o[j] = acc[j] / n;
                }
            }
        }

        std::vector<bool> has(size_t(kViewPx) * kViewPx);

        for (size_t k = 0; k < has.size(); k++) {
            has[k] = lv[k * 4 + 3] > 0.0f;
        }

        for (int pass = 0; pass < 8; pass++) {
            std::vector<float> next = lv;
            std::vector<bool> nextHas = has;

            for (uint32_t y = 0; y < kViewPx; y++) {
                for (uint32_t x = 0; x < kViewPx; x++) {
                    if (has[y * kViewPx + x]) {
                        continue;
                    }

                    float acc[3] = {};
                    int n = 0;

                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const int xx = int(x) + dx, yy = int(y) + dy;

                            if (xx < 0 || yy < 0 || xx >= int(kViewPx) || yy >= int(kViewPx) || !has[yy * kViewPx + xx]) {
                                continue;
                            }

                            for (int j = 0; j < 3; j++) {
                                acc[j] += lv[(size_t(yy) * kViewPx + xx) * 4 + j];
                            }

                            n++;
                        }
                    }

                    if (n) {
                        for (int j = 0; j < 3; j++) {
                            next[(size_t(y) * kViewPx + x) * 4 + j] = acc[j] / n;
                        }

                        nextHas[y * kViewPx + x] = true;
                    }
                }
            }

            lv.swap(next);
            has.swap(nextHas);
        }

        // Levels: box-filtered, alpha scaled so each keeps level 0's coverage (a far tree does not thin out).
        float coverage0 = 0.0f;

        for (size_t k = 0; k < size_t(kViewPx) * kViewPx; k++) {
            coverage0 += lv[k * 4 + 3] >= 0.5f;
        }

        coverage0 /= float(kViewPx * kViewPx);
        uint32_t size = kViewPx;

        for (uint32_t l = 0; l < kViewLevels; l++) {
            if (l) {
                const uint32_t half = size / 2;
                std::vector<float> down(size_t(half) * half * 4, 0.0f);

                for (uint32_t y = 0; y < half; y++) {
                    for (uint32_t x = 0; x < half; x++) {
                        for (int k = 0; k < 4; k++) {
                            const float* q = &lv[(size_t(y * 2 + k / 2) * size + x * 2 + k % 2) * 4];
                            float* o = &down[(size_t(y) * half + x) * 4];

                            for (int j = 0; j < 4; j++) {
                                o[j] += q[j] * 0.25f;
                            }
                        }
                    }
                }

                size = half;
                lv.swap(down);
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

            const uint32_t ox = (slotX + view * kViewPx) >> l;
            const uint32_t oy = slotY >> l;

            for (uint32_t by = 0; by < size / 4; by++) {
                for (uint32_t bx = 0; bx < size / 4; bx++) {
                    EncodeBlock(&lv[(size_t(by * 4) * size + bx * 4) * 4], size, l, ox + bx * 4, oy + by * 4);
                }
            }

            if (l == kViewLevels - 1) {
                for (uint32_t y = 0; y < size; y++) {
                    memcpy(&s_fol.level4[(size_t(oy + y) * (kAtlasW >> 4) + ox) * 4], &lv[size_t(y) * size * 4], size * 4 * sizeof(float));
                }
            }
        }
    }

    model.frame = frame;
    model.bottom = zLo;
    return anyDrawn;
}

// Levels 5..9, where a texel spans views or models: box-filtered from level 4.
void StoreSmallLevels() {
    std::vector<float> cur = s_fol.level4;
    uint32_t w = kAtlasW >> 4, h = kAtlasH >> 4;

    for (uint32_t l = 5; l < kAtlasLevels; l++) {
        std::vector<float> down(size_t(w / 2) * (h / 2) * 4, 0.0f);

        for (uint32_t y = 0; y < h / 2; y++) {
            for (uint32_t x = 0; x < w / 2; x++) {
                for (int k = 0; k < 4; k++) {
                    const float* q = &cur[(size_t(y * 2 + k / 2) * w + x * 2 + k % 2) * 4];

                    for (int j = 0; j < 4; j++) {
                        down[(size_t(y) * (w / 2) + x) * 4 + j] += q[j] * 0.25f;
                    }
                }
            }
        }

        w /= 2;
        h /= 2;
        cur.swap(down);

        for (uint32_t by = 0; by < h / 4; by++) {
            for (uint32_t bx = 0; bx < w / 4; bx++) {
                EncodeBlock(&cur[(size_t(by * 4) * w + bx * 4) * 4], w, l, bx * 4, by * 4);
            }
        }
    }
}

// The next queued model's picture (one a frame: a few ms for a big tree).
void PumpPictures() {
    while (!s_fol.queue.empty()) {
        const uint32_t id = s_fol.queue.front();
        s_fol.queue.erase(s_fol.queue.begin());
        Model& model = s_fol.models[id];

        if (s_fol.slots >= kMaxModels) {
            model.pic = Pic_Failed;  // ponytail: the atlas is full and the model goes without, as wraithhost's kImpMax
            continue;
        }

        const bool fresh = !s_fol.chain;

        if (fresh) {
            uint32_t size = 0;

            for (uint32_t l = 0; l < kAtlasLevels; l++) {
                s_fol.levelOffset[l] = size;
                size += (kAtlasW >> l) * (kAtlasH >> l);  // DXT5: a byte a pixel
            }

            s_fol.chain = new uint8_t[size]();
            s_fol.level4.assign(size_t(kAtlasW >> 4) * (kAtlasH >> 4) * 4, 0.0f);
            s_fol.atlas = TextureCreate(kAtlasW, kAtlasH, GxTex_Dxt5, GxTex_Dxt5, CGxTexFlags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1), nullptr, AtlasUserFunc, "FarFoliageAtlas", 0);
        }

        model.slot = static_cast<uint16_t>(s_fol.slots);

        if (!RenderPicture(model)) {
            model.pic = Pic_Failed;
            continue;
        }

        s_fol.slots++;
        model.pic = Pic_Baked;
        s_fol.bakeGen++;
        StoreSmallLevels();

        if (CGxTex* gxTex = TextureGetGxTex(s_fol.atlas, 0, nullptr)) {
            const uint32_t x = (model.slot % kCols) * kViews * kViewPx;
            const uint32_t y = (model.slot / kCols) * kViewPx;
            // ponytail: levels 5..9 change across the whole atlas width only where this slot's row is; the rect
            // update reaches them through the slot's own rectangle.
            GxTexUpdate(gxTex, fresh ? 0 : x, fresh ? 0 : y, fresh ? kAtlasW : x + kViews * kViewPx, fresh ? kAtlasH : y + kViewPx, 1);
        }

        break;
    }
}

// --- Cards ---

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

// Block b's cards, facing the camera where it is now, sorted by size class (one draw per class and chunk).
void BuildBlock(uint32_t b, const C3Vector& cam) {
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
        c.cards = std::min(kChunkCards, static_cast<uint32_t>(cards.size()) - first);

        if (c.capacity < c.cards) {
            FreeChunk(c);
            c.cards = std::min(kChunkCards, static_cast<uint32_t>(cards.size()) - first);
            c.capacity = std::min(kChunkCards, (c.cards + 1023) & ~1023u);
            c.pool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Vertex, GxPoolUsage_Dynamic, c.capacity * 4 * sizeof(CGxVertexPNCT), GxPoolHintBit_Unk0, "FarFoliageVB");
            c.buf = g_theGxDevicePtr->BufCreate(c.pool, sizeof(CGxVertexPNCT), c.capacity * 4, 0);
        }

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

            // The view nearest the camera: view k was drawn from the model's +x turned k x 90 deg.
            const float phi = atan2f(cam.y - p.y, cam.x - p.x);
            float rel = (phi - p.yaw) / 1.5707963f;
            const uint32_t view = static_cast<uint32_t>(static_cast<int32_t>(floorf(rel + 0.5f)) & 3);
            const float side = model.frame * p.scale;
            const float rx = -sinf(phi) * side * 0.5f, ry = cosf(phi) * side * 0.5f;
            const float x = p.x - origin.x, y = p.y - origin.y;
            const float z0 = p.z + model.bottom * p.scale;
            const float u0 = float((model.slot % kCols) * kViews * kViewPx + view * kViewPx) / kAtlasW;
            const float u1 = u0 + float(kViewPx) / kAtlasW;
            const float v0 = float((model.slot / kCols) * kViewPx) / kAtlasH;
            const float v1 = v0 + float(kViewPx) / kAtlasH;
            // Lit as a canopy: up, leaning toward the camera.
            C3Vector n(cosf(phi) * 0.45f, sinf(phi) * 0.45f, 0.9f);
            const C3Vector corner[4] = { C3Vector(x - rx, y - ry, z0), C3Vector(x + rx, y + ry, z0), C3Vector(x + rx, y + ry, z0 + side), C3Vector(x - rx, y - ry, z0 + side) };
            const C2Vector uv[4] = { C2Vector(u0, v1), C2Vector(u1, v1), C2Vector(u1, v0), C2Vector(u0, v0) };

            for (uint32_t i = 0; i < 4; i++) {
                v[k * 4 + i].m_position = corner[i];
                v[k * 4 + i].m_normal = n;
                v[k * 4 + i].m_color = CImVector(255, 255, 255, 255);
                v[k * 4 + i].m_texture = uv[i];
            }
        }

        g_theGxDevicePtr->BufUnlock(c.buf, 0);
        c.buf->m_hasData = 1;
    }

    block.builtAt = cam;
    block.builtGen = s_fol.bakeGen;
    block.built = true;
}

}  // namespace

void FarField::FoliageScanTile(uint32_t tile, const uint8_t* b, uint32_t n) {
    const uint32_t kMMDX = 0x4D4D4458;
    const uint32_t kMMID = 0x4D4D4944;
    const uint32_t kMDDF = 0x4D444446;
    const uint8_t *mmdx = nullptr, *mmid = nullptr, *mddf = nullptr;
    uint32_t mmdxLen = 0, mmidLen = 0, mddfLen = 0;

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
        Block& block = s_fol.blocks[(row / 8) * 8 + col / 8];
        block.placed.push_back(p);
        block.lo[0] = std::min(block.lo[0], p.x);
        block.lo[1] = std::min(block.lo[1], p.y);
        block.hi[0] = std::max(block.hi[0], p.x);
        block.hi[1] = std::max(block.hi[1], p.y);

        if (model.pic == Pic_None) {
            model.pic = Pic_Queued;
            s_fol.queue.push_back(mi);
        }
    }

    (void)tile;
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

    if (s_fol.atlas) {
        HandleClose(reinterpret_cast<HOBJECT>(s_fol.atlas));
    }

    delete[] s_fol.chain;
    s_fol.chain = nullptr;
    s_fol.atlas = nullptr;
    s_fol.level4.clear();
    s_fol.byName.clear();
    s_fol.models.clear();
    s_fol.queue.clear();
    s_fol.uids.clear();
    s_fol.slots = 0;
    s_fol.bakeGen++;
}

void FarField::RenderFoliage(bool horizon) {
    if (!FarTreesCVar()->m_intValue || FarField::FogStretch() == 1.0f || !CDetailDoodad::s_bUseShaders) {
        return;
    }

    CGxShader* vertexShader = CDetailDoodad::s_vertexShader[0];
    CGxShader* pixelShader = CDetailDoodad::s_pixelShader[0];

    if (!vertexShader || !vertexShader->Valid() || !pixelShader || !pixelShader->Valid()) {
        return;
    }

    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float farZ = CGWorldFrame::GetActiveCamera()->m_farZ;
    uint32_t rebuilds = 0;

    if (!horizon) {
        PumpPictures();
    }

    if (!s_fol.atlas || !EnsureIndices()) {
        return;
    }

    // Blocks in this pass's depth range, front to back; at most two rebuilt a frame (main pass), nearest first.
    uint32_t order[64];
    float nearD[64];
    float farD[64];
    uint32_t count = 0;

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
        const float fd = sqrtf(fx * fx + fy * fy) + 150.0f;  // the tallest card's top

        if (horizon ? fd < farZ - 50.0f : nd > farZ) {
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

        const float moved = sqrtf((cam.x - block.builtAt.x) * (cam.x - block.builtAt.x) + (cam.y - block.builtAt.y) * (cam.y - block.builtAt.y));
        const bool stale = moved > 0.25f * std::max(nearD[i], 150.0f) || (block.missing && block.builtGen != s_fol.bakeGen);

        if (!block.built || lost || (stale && rebuilds < 2)) {
            BuildBlock(order[i], cam);
            rebuilds++;
        }
    }

    // Where each size class's model starts to fade (or farclip, past which no doodad is drawn), less the chunk
    // slack (the stock cull goes by the chunk's distance) and measured as view depth at the screen's edge (the
    // shader fades by view depth, the game by distance): the card is whole there, and comes in over one band.
    const C44Matrix& proj = g_theGxDevicePtr->m_projNative;
    const float tanX = proj.a0 != 0.0f ? 1.0f / fabsf(proj.a0) : 1.0f;
    const float cosEdge = 1.0f / sqrtf(1.0f + tanX * tanX);
    C4Vector fade[5];
    float fadeFrom[5];

    for (uint32_t c = 0; c < 5; c++) {
        const float end = std::min(CWorldScene::s_fadeDistMin[c] - 24.0f, farZ - 35.0f);
        const float whole = std::max(end * cosEdge, 10.0f);
        const float band = std::max(20.0f, whole * 0.1f);
        fadeFrom[c] = whole - band;
        fade[c].x = 1.0f / band;
        fade[c].y = -fadeFrom[c] / band;
        fade[c].z = 0.0f;
        fade[c].w = 0.0f;
    }

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_AlphaKey);
    GxRsSet(GxRs_AlphaRef, kAlphaKey);
    GxRsSet(GxRs_DepthWrite, 1);
    GxRsSet(GxRs_DepthTest, 3);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_VertexShader, vertexShader);
    GxRsSet(GxRs_PixelShader, pixelShader);
    GxRsSet(GxRs_Texture0, static_cast<void*>(TextureGetGxTex(s_fol.atlas, 0, nullptr)));
    GxRsSet(GxRs_ColorOp0, 0);
    GxRsSet(GxRs_AlphaOp0, 0);

    // The grass's own constants for this frame (projection bound now, DNInfo fog, sun; fog colour bound), so
    // the cards fog and light like the grass and the terrain. Restored after: the grass builds them again.
    const CDetailDoodad::SShaderConstants saved = CDetailDoodad::s_shaderConstants;
    CDetailDoodad::InitializeShaderConstants();
    CDetailDoodad::SShaderConstants consts = CDetailDoodad::s_shaderConstants;
    CDetailDoodad::s_shaderConstants = saved;

    CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[GxXform_View];
    const C44Matrix view = stack.m_mtx[stack.m_level];
    bool uploaded = false;
    int32_t boundFade = -1;

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t b = order[i];
        Block& block = s_fol.blocks[b];

        C44Matrix world;
        world.Translate(BlockOrigin(b) - cam);
        consts.m_worldView = world * view;

        if (!uploaded) {
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&consts), sizeof(consts) / sizeof(C4Vector));
            uploaded = true;
        } else {
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&consts.m_worldView), 4);
        }

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
                    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, offsetof(CDetailDoodad::SShaderConstants, m_fade) / sizeof(C4Vector), &fade[cls].x, 1);
                    boundFade = static_cast<int32_t>(cls);
                }

                CGxBatch batch;
                batch.m_primType = GxPrim_Triangles;
                batch.m_start = c.start[cls] * 6;
                batch.m_count = c.count[cls] * 6;
                batch.m_minIndex = c.start[cls] * 4;
                batch.m_maxIndex = (c.start[cls] + c.count[cls]) * 4 - 1;
                g_theGxDevicePtr->Draw(&batch, 1);
            }
        }
    }

    g_theGxDevicePtr->RsPop();
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
