#include "MapLowDetail.h"
#include "FarTerrainLod.h"
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/CMath.h"
#include "WoW/Source/WorldClient/CWorldOcclusion.h"
#include "WoW/Source/WorldClient/FVBBList.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/World.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "Engine/Source/Gx/CGxVertexPCT.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapRenderChunk.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/SortTable.h"

// VERIFY: 3763adce auto
// OFFSET: 0x007C06E0
CMapAreaLow::CMapAreaLow() {
    this->m_sphere.r = 0.0f;
    this->m_index.x = 0;
    this->m_index.y = 0;
    this->m_horizonLink.m_prevLink = nullptr;
    this->m_horizonLink.m_next = nullptr;
    this->m_indexCount = 0;
    this->m_block = nullptr;
    this->m_flags = 0;
    this->m_heights = nullptr;
    this->m_holes = nullptr;
    this->m_builtTint = 0xFFFFFFFF;
}

// VERIFY: 0b659a65 auto
// OFFSET: 0x007CC2C0
CMapLowDetail::CMapLowDetail() {
    this->m_data = nullptr;
    this->m_mapObjNames = nullptr;
    this->m_mapObjNameOffsets = nullptr;
    this->m_mapObjDefs = nullptr;
    this->m_mapObjDefCount = 0;
    this->m_areaOffsets = nullptr;
    memset(this->m_areas, 0, sizeof(this->m_areas));
}

// OFFSET: 0x007CBFE0
void CMapLowDetail::Purge() {
    for (uint32_t i = 0; i < 4096; i++) {
        CMapAreaLow* areaLow = this->m_areas[i];

        if (areaLow) {
            if (areaLow->m_block) {
                CMap::s_vbbList.Release(areaLow->m_block);
            }

            this->m_areas[i]->m_block = nullptr;
        }
    }
}

// VERIFY: 3b80ddc7 manual
// OFFSET: 0x007CC0B0
void CMapLowDetail::CullHorizonAreas(const C3Vector* pos) {
    float y = -(pos->y - 17066.666f);
    float x = -(pos->x - 17066.666f) * 0.029999999f;
    int32_t row = CMath::fint_(x - 0.5f);
    y = y * 0.029999999f;
    int32_t col = CMath::fint_(y - 0.5f);
    int32_t radius = static_cast<int32_t>(static_cast<int64_t>(World::s_horizonFarClipScale * World::s_farClip * 0.030000001f));

    int32_t minCol = ((col - radius) >> 4) - 2;
    int32_t minRow = ((row - radius) >> 4) - 2;
    int32_t maxCol = ((col + radius) >> 4) + 2;
    int32_t maxRow = ((radius + row) >> 4) + 2;

    if (minRow < 0) {
        minRow = 0;
    }

    if (minCol < 0) {
        minCol = 0;
    }

    if (maxCol >= 64) {
        maxCol = 63;
    }

    if (maxRow >= 64) {
        maxRow = 63;
    }

    for (int32_t r = minRow; r <= maxRow; r++) {
        for (int32_t c = minCol; c <= maxCol; c++) {
            CMapAreaLow* area = this->m_areas[64 * r + c];

            if (!area || CWorldScene::FrustumCull(&area->m_bbox) || CWorldOcclusion::QueryVolumes(&area->m_sphere)) {
                continue;
            }

            if (World::s_farClip > 300.0f && CWorldOcclusion::QueryBuffer(&area->m_bbox, 40)) {
                continue;
            }

            CWorldScene::AddHorizonChunk(area);
        }
    }

    for (uint32_t i = 0; i < this->m_mapObjDefList.Count(); i++) {
        CMapObjDef* def = this->m_mapObjDefList.m_data[i];

        if (def->m_owner->m_isGroupLoaded && !CWorldScene::FrustumCull(&def->m_bbox) && !CWorldOcclusion::QueryVolumes(&def->m_sphere)) {
            CWorldScene::AddHorizonMapObjDef(def);
        }
    }
}

// VERIFY: ba76c774 manual
// OFFSET: 0x007CC310
void CMapLowDetail::Load(const char* mapPath, const char* mapName) {
    FarField::Reset();
    char fileName[256];
    SFileImpl* file;

    SStrPrintf(fileName, sizeof(fileName), "%s\\%s.wdl", mapPath, mapName);
    SFileOpenFile(fileName, &file);

    if (!file) {
        return;
    }

    uint32_t size = SFileGetFileSize(file, nullptr);
    this->m_data = static_cast<char*>(SMemAlloc(size, __FILE__, __LINE__, 0));
    SFileReadFile(file, this->m_data, size, nullptr, nullptr, 0);
    SFileCloseFile(file);

    uint32_t* chunk = reinterpret_cast<uint32_t*>(this->m_data + reinterpret_cast<uint32_t*>(this->m_data)[1] + 8);
    char* data = this->m_data + reinterpret_cast<uint32_t*>(this->m_data)[1] + 16;

    if (chunk[0] == 0x4D574D4F) {
        this->m_mapObjNames = data;
        uint32_t* mwid = reinterpret_cast<uint32_t*>(data + chunk[1]);
        this->m_mapObjNameOffsets = mwid + 2;
        uint32_t* modf = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(mwid) + mwid[1] + 8);
        this->m_mapObjDefs = reinterpret_cast<SMMapObjDef*>(modf + 2);
        this->m_mapObjDefCount = modf[1] >> 6;
        data = reinterpret_cast<char*>(modf + 2) + modf[1] + 8;
    }

    this->m_areaOffsets = reinterpret_cast<uint32_t*>(data);

    uint32_t index = 0;

    for (uint32_t y = 0; y < 1024; y += 16) {
        for (uint32_t x = 0; x < 1024; x += 16) {
            if (this->m_areaOffsets[index]) {
                char* mare = this->m_data + this->m_areaOffsets[index];
                CMapAreaLow* area = CMap::AllocAreaLow();
                this->m_areas[index] = area;
                area->m_heights = reinterpret_cast<int16_t*>(mare + 8);
                uint32_t* next = reinterpret_cast<uint32_t*>(mare + reinterpret_cast<uint32_t*>(mare)[1] + 8);

                int32_t minHeight = 100000;
                int32_t maxHeight = -100000;

                for (uint32_t i = 0; i < 545; i++) {
                    int32_t height = area->m_heights[i];

                    if (height > maxHeight) {
                        maxHeight = height;
                    }

                    if (height < minHeight) {
                        minHeight = height;
                    }
                }

                area->m_index.x = x >> 4;
                area->m_index.y = y >> 4;

                float top = 17066.666f - static_cast<float>(y) * 33.333332f;
                area->m_topLeft.x = top;
                area->m_bbox.t.x = top;

                float left = static_cast<float>(x) * -33.333332f + 17066.666f;
                area->m_topLeft.y = left;
                area->m_bbox.t.y = left;
                area->m_bbox.t.z = static_cast<float>(maxHeight);
                area->m_bbox.b.x = top - 533.33331f;
                area->m_bbox.b.y = left - 533.33331f;
                area->m_bbox.b.z = static_cast<float>(minHeight);

                area->m_sphere.c.x = (area->m_bbox.t.x + area->m_bbox.b.x) * 0.5f;
                area->m_sphere.c.y = (area->m_bbox.b.y + area->m_bbox.t.y) * 0.5f;
                area->m_sphere.c.z = 0.5f * (area->m_bbox.b.z + area->m_bbox.t.z);
                float dz = area->m_bbox.t.z - area->m_sphere.c.z;
                float dy = area->m_bbox.t.y - area->m_sphere.c.y;
                float dx = area->m_bbox.t.x - area->m_sphere.c.x;
                area->m_sphere.r = sqrtf(dx * dx + dy * dy + dz * dz);

                if (next[0] == 0x4D41484F) {
                    uint16_t* holes = reinterpret_cast<uint16_t*>(next + 2);
                    area->m_holes = holes;
                    area->m_indexCount = 0;

                    for (uint32_t row = 0; row < 16; row++) {
                        for (uint32_t bit = 0; bit < 16; bit++) {
                            if (!((1 << bit) & holes[row])) {
                                area->m_indexCount += 12;
                            }
                        }
                    }
                } else {
                    area->m_holes = nullptr;
                    area->m_indexCount = 3072;
                }
            }

            index++;
        }
    }

    C3Vector center(17066.666f, 17066.666f, 0.0f);
    this->m_mapObjDefList.SetCount(this->m_mapObjDefCount);

    for (uint32_t i = 0; i < this->m_mapObjDefCount; i++) {
        SMMapObjDef* def = &this->m_mapObjDefs[i];
        CMapObjDef* mapObjDef = CMap::CreateMapObjDef(this->m_mapObjNames + this->m_mapObjNameOffsets[def->m_nameId], def, &center, false);

        if (!CMap::s_isStreamingMode) {
            CMapObj* mapObj = mapObjDef->m_owner;

            if (!mapObj->m_isGroupLoaded) {
                mapObj->WaitLoad();
            }

            uint32_t groupCount = mapObj->m_groupInfoCount;

            for (uint32_t group = 0; group < groupCount; group++) {
                if (!mapObj->IsGroupLoaded(group)) {
                    mapObj->ReadGroup(group);
                    mapObj->WaitLoadGroup(group);
                }
            }
        }

        mapObjDef->m_refCount++;
        this->m_mapObjDefList.m_data[i] = mapObjDef;
    }
}

// Not in 12340 (horizonFog): the grid as PN for the terrain vertex shader (the same 24-byte stride
// as PCT, so the same VBB blocks). Normals from the WDL heights: central differences on the 17x17
// outer grid (one-sided at the tile's edge), an inner vertex from its cell's four corners. x falls
// along the rows and y along the columns, 33.33 yd apart.
static void WriteGridNormals(const CMapAreaLow* area, CGxVertexPN* vertex) {
    const int16_t* h = area->m_heights;
    const float step = 33.333332f;

    for (int32_t row = 0; row < 17; row++) {
        const int32_t r0 = row > 0 ? row - 1 : row;
        const int32_t r1 = row < 16 ? row + 1 : row;

        for (int32_t col = 0; col < 17; col++) {
            const int32_t c0 = col > 0 ? col - 1 : col;
            const int32_t c1 = col < 16 ? col + 1 : col;
            const float dzdx = (h[17 * r1 + col] - h[17 * r0 + col]) / (-(r1 - r0) * step);
            const float dzdy = (h[17 * row + c1] - h[17 * row + c0]) / (-(c1 - c0) * step);
            vertex->m_position = C3Vector(area->m_topLeft.x - row * step, area->m_topLeft.y - col * step, h[17 * row + col]);
            vertex->m_normal = C3Vector(-dzdx, -dzdy, 1.0f);
            vertex->m_normal.Normalize();
            vertex++;
        }
    }

    for (int32_t row = 0; row < 16; row++) {
        for (int32_t col = 0; col < 16; col++) {
            const float a = h[17 * row + col];
            const float b = h[17 * row + col + 1];
            const float c = h[17 * (row + 1) + col];
            const float d = h[17 * (row + 1) + col + 1];
            const float dzdx = ((c + d) - (a + b)) / (-2.0f * step);
            const float dzdy = ((b + d) - (a + c)) / (-2.0f * step);
            vertex->m_position = C3Vector(area->m_topLeft.x - (row + 0.5f) * step, area->m_topLeft.y - (col + 0.5f) * step, h[289 + 16 * row + col]);
            vertex->m_normal = C3Vector(-dzdx, -dzdy, 1.0f);
            vertex->m_normal.Normalize();
            vertex++;
        }
    }
}

static void BuildLowDetailGridNormals(CMapAreaLow* area, CGxBuf* buf) {
    WriteGridNormals(area, reinterpret_cast<CGxVertexPN*>(g_theGxDevicePtr->BufLock(buf)));
    g_theGxDevicePtr->BufUnlock(buf, 0);
    buf->m_hasData = 1;
}

// 12340's index pattern (CMap::BuildLowDetailGridIndices), into memory and offset by `base`: four
// triangles a cell around its inner vertex, the cells that are (pass 0) or are not (pass 1) holes.
// Returns the end of what it wrote.
static uint16_t* WriteGridIndices(uint16_t* indices, const uint16_t* holes, int32_t pass, uint32_t base) {
    uint32_t outer = base;
    uint32_t inner = base + 289;

    for (uint32_t row = 0; row < 16; row++) {
        for (int32_t col = 0; col < 16; col++) {
            if (!holes || (pass ? (holes[row] & (1 << col)) != 0 : (holes[row] & (1 << col)) == 0)) {
                *indices++ = inner;
                *indices++ = outer + 1;
                *indices++ = outer;
                *indices++ = inner;
                *indices++ = outer + 18;
                *indices++ = outer + 1;
                *indices++ = inner;
                *indices++ = outer + 17;
                *indices++ = outer + 18;
                *indices++ = inner;
                *indices++ = outer;
                *indices++ = outer + 17;
            }

            outer++;
            inner++;
        }

        outer++;
    }

    return indices;
}

static void BuildLowDetailGrid(CMapAreaLow* area, CGxBuf* buf, bool normals) {
    if (normals) {
        BuildLowDetailGridNormals(area, buf);
    } else {
        CMap::BuildLowDetailGridVertices(area, buf);
    }
}

// Not in 12340 (wdlTextured): texture the WDL horizon with the map's own ground, baked in the client.
static CVar* WdlTexturedCVar() {
    static CVar* s_cvar = CVar::Register("wdlTextured", "Engine option: texture the WDL horizon terrain with the map's own ground, baked from its ADT layers (0: the stock fog-coloured silhouette)", 0x1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

// Not in 12340 (horizonFog): one fog for the world and the textured horizon.
static CVar* HorizonFogCVar() {
    static CVar* s_cvar = CVar::Register("horizonFog", "Engine option: the world and the textured WDL horizon share one fog, the world's stretched out to the horizon's far plane, and the horizon is drawn lit and fogged by the terrain shaders (0: stock fog; the horizon keeps its own)", 0x1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

// Not in 12340 (horizonFog): the far fog and light, shared by every far pass. docs/FARFIELD_FOG.md.
//
// Stock fog ends at or before farclip, where the horizon begins, so a textured horizon could only
// be fog-coloured (stock) or carry a fog of its own that does not meet the terrain's. Instead the
// world's fog curve is stretched by horizonFarclipScale (as wxl-farfield's game_fog did for the
// stock exe; DayNight::UpdateFog applies it), so the terrain is not fully fogged at its edge and a
// far pass drawn with the same constants carries on from it to full fog at the horizon's far plane.
float FarField::FogStretch() {
    if (!WdlTexturedCVar()->m_intValue || !HorizonFogCVar()->m_intValue) {
        return 1.0f;
    }

    return World::s_horizonFarClipScale > 1.0f ? World::s_horizonFarClipScale : 1.0f;
}

void FarField::TerrainConstants(CMapRenderChunk::SVertexShaderConstants& consts) {
    // This frame's terrain constants (CWorldScene::RenderChunks -> InitializeVertexShaderConstants):
    // world-view, fog (c12), sun direction / ambient / diffuse (c24..c26). The projection is the
    // one bound now (CWorldScene::RenderHorizon's), and no point lights (SetVertexShader leaves the
    // last chunk's behind).
    consts = CMapRenderChunk::s_vertexShaderConsts;
    consts.m_projection = g_theGxDevicePtr->m_projNative;

    if (g_theGxDevicePtr->DeviceApi() == GxApi_OpenGl) {
        consts.m_projection.c0 *= -1.0f;
        consts.m_projection.c1 *= -1.0f;
        consts.m_projection.c2 *= -1.0f;
        consts.m_projection.c3 *= -1.0f;
    }

    memset(consts.m_pointLights, 0, sizeof(consts.m_pointLights));

    for (auto& light : consts.m_pointLights) {
        light.m_attenuation.x = 1.0f;
    }
}

void FarField::BindFogColor() {
    // Where the terrain pixel shaders read it, as CWorldScene::RenderChunks binds it.
    DayNight::DNInfo* info = DayNight::GetInfo();

    if (g_theGxDevicePtr->Caps().unk_0134) {
        GxRsSet(GxRs_FogColor, *reinterpret_cast<int32_t*>(&info->m_fog.m_color));
    } else {
        C4Vector color(info->m_fog.m_color);
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 2, reinterpret_cast<const float*>(&color), 1);
    }
}

// Not in 12340 (wdlTextured): the WDL horizon's ground texture and its batched draw. docs/FARFIELD_FOG.md.
//
// The ground is the map's own, baked in the client the way wxl-wraithhost bakes wxl-farfield's `.faralb`:
// each ADT chunk's layer textures (their real texels, at the mip the distance calls for), tiled as the
// terrain shader tiles them and composited by the alpha maps as it composites them, times the vertex
// colours (MCCV) and the baked shadow; water cells in their liquid's colour. The ADT is read through the
// async file queue, nearest tile first, and baked a slice at a time under a per-frame budget into three
// 4096 px DXT1 atlases: near (512 px a tile, 1.04 yd a texel, the 8 x 8 tiles round the camera), mid
// (256 px, 2.08 yd, 16 x 16 tiles: farfield's) and coarse (64 px, 8.3 yd, the whole map). A window atlas
// keeps each tile in the slot its own position modulo the window, so its uv is still a linear function of
// the position (one constant set per atlas, wrap addressing) and a bilinear tap or a mip that crosses a
// slot edge reaches the tile that really is next door. Each drawn tile takes the coarsest atlas whose
// texel is at most farTexPixels on screen (by distance only, never by what is in view).
//
// The geometry is the WDL heights in static buffers, one vertex and one index buffer per 8 x 8 tiles,
// built when first seen (the heights never change). A frame draws each block's visible tiles as merged
// index runs, blocks front to back, one set per atlas, state set once.
namespace {

constexpr float kOrigin = 17066.666f;
constexpr float kTileYd = 533.33331f;
constexpr uint32_t kAtlasPx = 4096;
constexpr uint32_t kLevels = 11;          // 4096 .. 4 px: the DXT chain CGxDevice::ITexWHDStartEnd asks for
constexpr uint32_t kAtlases = 3;          // near, mid, coarse
constexpr uint32_t kTilePxOf[kAtlases] = { 512, 256, 64 };
constexpr uint32_t kBakePx = 512;
constexpr uint32_t kTileVerts = 545;
constexpr uint32_t kTileIndices = 3072;
constexpr uint32_t kReadsInFlight = 2;
constexpr double kBakeBudgetMs = 2.0;     // ponytail: fixed; one slice always runs, so a slow PC still progresses
// wraithhost's liquid colours (water, ocean, magma, slime).
constexpr float kLiquid[4][3] = { { 34, 62, 70 }, { 22, 48, 66 }, { 235, 95, 25 }, { 90, 140, 40 } };

enum : uint8_t { Tile_None, Tile_Reading, Tile_Baked, Tile_Failed };

struct FarRead {
    CAsyncObject* obj;
    uint8_t* buf;
    uint32_t size;
    uint32_t tile;
    uint32_t gen;
    bool bake;     // for the ground bake (any atlas)
    bool heights;  // for the detailed levels (FarTerrainLod.cpp)
};

struct FarBlock {
    CGxPool* vertexPool;
    CGxPool* indexPool;
    CGxBuf* vertexBuf;
    CGxBuf* indexBuf;
    uint32_t verts;
    bool init;
};

// One ground texture's texels as the bake samples them: its 4 x 4 (one tile repeat over 4 texels), 2 x 2
// and 1 x 1 reductions, RGB 0..255.
struct LayerTex {
    float m4[16][3];
    float m2[4][3];
    float m1[3];
};

struct Atlas {
    uint8_t* chain = nullptr;   // the whole DXT chain, kept for the texture's user callback
    CTexture* tex = nullptr;
    int16_t owner[4096];        // per slot: the tile baked into it, -1 none
};

// The tile being baked, a slice a frame: heights, 16 chunk rows composited at kBakePx, then each wanted
// atlas's levels encoded a few block rows at a time.
struct BakeJob {
    FarRead* read = nullptr;
    uint32_t step = 0;          // 0 heights, 1..16 chunk rows, then store
    uint32_t px = 0;            // composite resolution a tile
    bool want[kAtlases] = {};
    uint32_t atlas = 0, level = 0, row = 0;
    bool prepared = false;      // the current atlas's level images are built
    double ms = 0.0;
    // parsed once
    std::vector<const char*> textures;
    const uint8_t* mh2o = nullptr;
    uint32_t mh2oLen = 0;
    const uint8_t* chunks[256] = {};
    uint32_t chunkLen[256] = {};
};

struct FarState {
    uint32_t gen = 1;
    uint8_t tile[4096] = {};               // the coarse bake (and whether the ADT is readable at all)
    uint8_t busy[4096] = {};               // a read or bake of it is under way
    uint8_t avg[4096][3] = {};
    uint32_t start[4096][2] = {};          // per tile: index start / count of its culled-off and culled-on cells
    uint32_t count[4096][2] = {};
    uint32_t levelOffset[kLevels] = {};
    Atlas atlas[kAtlases];
    FarBlock blocks[64] = {};
    std::vector<FarRead*> ready;
    uint32_t inFlight = 0;
    BakeJob job;
    std::unordered_map<std::string, LayerTex> textures;
    // farTerrainLodStats
    uint32_t baked = 0;
    double bakeMs = 0.0;
    std::vector<CMapRenderChunk*> band;    // this frame's real chunks reaching into the blend band
    size_t lastBand = 0;
    CGxShader* blendPs = nullptr;
    bool blendTried = false;
};

FarState s_far;

CVar* TexQualityCVar() {
    static CVar* s_cvar = CVar::Register("farTexQuality", "Engine option: far ground texture detail (0: 8.3 yd a texel everywhere; 1: also 2.08 yd within 16 x 16 tiles; 2: also 1.04 yd within 8 x 8 tiles, the view distance's)", 0x1, "2", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* TexPixelsCVar() {
    static CVar* s_cvar = CVar::Register("farTexPixels", "Engine option: the largest far ground texel allowed on screen, in pixels; a tile takes the coarsest baked texture that stays under it", 0x1, "1.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* TexMemoryCVar() {
    static CVar* s_cvar = CVar::Register("farTexMemory", "Engine option: megabytes the far ground textures may hold (each atlas is 10.7 MB on the GPU plus its 10.7 MB copy; coarse first, then mid, then near)", 0x1, "72", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* BandCVar() {
    static CVar* s_cvar = CVar::Register("farTexBand", "Engine option: fade the last stretch of real terrain into the far ground's colour, so the view distance shows no line (0: off)", 0x1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* BandWidthCVar() {
    static CVar* s_cvar = CVar::Register("farTexBandWidth", "Engine option: yards of real terrain the blend band fades over, ending just short of the view distance", 0x1, "150", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

CVar* HorizonBatchedCVar() {
    static CVar* s_cvar = CVar::Register("horizonBatched", "Engine option: draw the WDL horizon from static buffers in a few batched draws (0: one draw per tile, the old path, for comparison)", 0x1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    return s_cvar;
}

uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void From565(uint16_t v, float* c) {
    c[0] = static_cast<float>((v >> 11) * 255 / 31);
    c[1] = static_cast<float>(((v >> 5) & 63) * 255 / 63);
    c[2] = static_cast<float>((v & 31) * 255 / 31);
}

// One DXT block's 16 colours (the colour half; DXT3/5 carry it in their last 8 bytes).
void DecodeDxtBlock(const uint8_t* blk, bool dxt1, float (*out)[3]) {
    const uint16_t v0 = static_cast<uint16_t>(blk[0] | blk[1] << 8);
    const uint16_t v1 = static_cast<uint16_t>(blk[2] | blk[3] << 8);
    float c[4][3];
    From565(v0, c[0]);
    From565(v1, c[1]);

    for (int j = 0; j < 3; j++) {
        if (v0 > v1 || !dxt1) {
            c[2][j] = (2 * c[0][j] + c[1][j]) / 3;
            c[3][j] = (c[0][j] + 2 * c[1][j]) / 3;
        } else {
            c[2][j] = (c[0][j] + c[1][j]) / 2;
            c[3][j] = 0.0f;
        }
    }

    const uint32_t bits = Rd32(blk + 4);

    for (uint32_t p = 0; p < 16; p++) {
        for (int j = 0; j < 3; j++) {
            out[p][j] = c[(bits >> (2 * p)) & 3][j];
        }
    }
}

// A ground texture as the bake samples it: its smallest mip that is still 4 x 4 or more, read alone and
// box-reduced to 4 x 4 (at 1.04 yd a texel a tile repeat, 4.17 yd, is 4 texels, so this is the mip the
// real terrain's sampler is on at that density), then 2 x 2 and 1 x 1. Cached by name. Mid grey when
// unreadable.
const LayerTex& LayerTexOf(const char* name) {
    std::string key(name);

    for (char& c : key) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    auto it = s_far.textures.find(key);

    if (it != s_far.textures.end()) {
        return it->second;
    }

    LayerTex tex;

    for (auto& c : tex.m4) {
        c[0] = c[1] = c[2] = 128.0f;
    }

    SFileImpl* file = nullptr;

    if (SFileOpenFile(name, &file) && file) {
        uint8_t h[148];
        uint32_t got = 0;

        // ponytail: DXT (compression 2) only, which every terrain texture is; anything else stays grey.
        if (SFileReadFile(file, h, sizeof(h), &got, nullptr, 0) && got == sizeof(h) && !memcmp(h, "BLP2", 4) && h[8] == 2) {
            const uint32_t w = Rd32(h + 12);
            const uint32_t ht = Rd32(h + 16);
            const bool dxt1 = h[9] <= 1 || h[10] == 0;
            uint32_t level = 0;

            for (uint32_t l = 1; h[11] && l < 16 && (w >> l) >= 4 && (ht >> l) >= 4 && Rd32(h + 20 + l * 4) && Rd32(h + 84 + l * 4); l++) {
                level = l;
            }

            const uint32_t mw = w >> level, mh = ht >> level;
            const uint32_t bytes = dxt1 ? 8 : 16;
            const uint32_t bw = mw / 4, bh = mh / 4;
            const uint32_t len = bw * bh * bytes;

            if (w && ht && w <= 4096 && ht <= 4096 && bw && bh && mw <= 64 && mh <= 64 && Rd32(h + 84 + level * 4) >= len) {
                std::vector<uint8_t> data(len);
                SFile::SetFilePointer(file, static_cast<int32_t>(Rd32(h + 20 + level * 4)), nullptr, 0);

                if (SFileReadFile(file, data.data(), len, &got, nullptr, 0) && got == len) {
                    std::vector<float> px(mw * mh * 3);

                    for (uint32_t by = 0; by < bh; by++) {
                        for (uint32_t bx = 0; bx < bw; bx++) {
                            float c[16][3];
                            DecodeDxtBlock(data.data() + (by * bw + bx) * bytes + (bytes - 8), dxt1, c);

                            for (uint32_t k = 0; k < 16; k++) {
                                for (int j = 0; j < 3; j++) {
                                    px[((by * 4 + k / 4) * mw + bx * 4 + k % 4) * 3 + j] = c[k][j];
                                }
                            }
                        }
                    }

                    const uint32_t fx = mw / 4, fy = mh / 4;

                    for (uint32_t k = 0; k < 16; k++) {
                        for (int j = 0; j < 3; j++) {
                            float acc = 0.0f;

                            for (uint32_t y = 0; y < fy; y++) {
                                for (uint32_t x = 0; x < fx; x++) {
                                    acc += px[(((k / 4) * fy + y) * mw + (k % 4) * fx + x) * 3 + j];
                                }
                            }

                            tex.m4[k][j] = acc / static_cast<float>(fx * fy);
                        }
                    }
                }
            }
        }

        SFileCloseFile(file);
    }

    for (uint32_t k = 0; k < 4; k++) {
        const uint32_t x = (k % 2) * 2, y = (k / 2) * 2;

        for (int j = 0; j < 3; j++) {
            tex.m2[k][j] = (tex.m4[y * 4 + x][j] + tex.m4[y * 4 + x + 1][j] + tex.m4[y * 4 + x + 4][j] + tex.m4[y * 4 + x + 5][j]) / 4.0f;
        }
    }

    for (int j = 0; j < 3; j++) {
        tex.m1[j] = (tex.m2[0][j] + tex.m2[1][j] + tex.m2[2][j] + tex.m2[3][j]) / 4.0f;
    }

    return s_far.textures.emplace(key, tex).first->second;
}

// One alpha map as 64 x 64 bytes (wraithhost DecodeAlpha): RLE, 8-bit (big alpha) or 4-bit, the last
// row and column of a 4-bit map repeated unless the chunk says it is a full 64 x 64.
void DecodeAlpha(const uint8_t* mcal, uint32_t mcalLen, uint32_t offset, uint32_t layerFlags, bool bigAlpha, bool noFix, uint8_t* out) {
    memset(out, 0, 4096);

    if (!mcal || offset >= mcalLen) {
        return;
    }

    const uint8_t* s = mcal + offset;
    const uint32_t avail = mcalLen - offset;

    if (layerFlags & 0x200) {
        uint32_t i = 0;
        uint32_t o = 0;

        while (o < 4096 && i < avail) {
            const uint8_t c = s[i++];
            const uint32_t n = (c & 0x7F) < 4096 - o ? (c & 0x7F) : 4096 - o;

            if (c & 0x80) {
                if (i < avail) {
                    memset(out + o, s[i++], n);
                }
            } else {
                const uint32_t k = n < avail - i ? n : avail - i;
                memcpy(out + o, s + i, k);
                i += k;
            }

            o += n;
        }
    } else if (bigAlpha) {
        memcpy(out, s, avail < 4096 ? avail : 4096);
    } else {
        for (uint32_t i = 0; i < 2048 && i < avail; i++) {
            out[i * 2] = static_cast<uint8_t>((s[i] & 0x0F) * 17);
            out[i * 2 + 1] = static_cast<uint8_t>((s[i] >> 4) * 17);
        }

        if (!noFix) {
            for (uint32_t r = 0; r < 64; r++) {
                out[r * 64 + 63] = out[r * 64 + 62];
            }

            memcpy(out + 63 * 64, out + 62 * 64, 64);
        }
    }
}

// A sub-chunk of an MCNK (offsets from the MCNK's own header), checked by its tag.
const uint8_t* FindSub(const uint8_t* mcnk, uint32_t len, uint32_t ofs, uint32_t tag, uint32_t& size) {
    if (ofs < 8 || ofs + 8 > len || Rd32(mcnk + ofs) != tag) {
        size = 0;
        return nullptr;
    }

    size = Rd32(mcnk + ofs + 4);

    if (ofs + 8ull + size > len) {
        size = len - ofs - 8;
    }

    return mcnk + ofs + 8;
}

// The ADT's chunks, textures and MH2O, for the composite. False when it is not a whole ADT.
bool ParseAdt(BakeJob& job) {
    const uint32_t kMTEX = 0x4D544558;
    const uint32_t kMH2O = 0x4D48324F;
    const uint32_t kMCNK = 0x4D434E4B;
    const uint8_t* b = job.read->buf;
    const uint32_t n = job.read->size;
    uint32_t seen = 0;

    for (uint32_t o = 0; o + 8 <= n;) {
        const uint32_t magic = Rd32(b + o);
        const uint32_t size = Rd32(b + o + 4);

        if (o + 8ull + size > n) {
            break;
        }

        if (magic == kMTEX) {
            for (uint32_t i = 0; i < size;) {
                const char* name = reinterpret_cast<const char*>(b + o + 8 + i);
                const uint32_t len = static_cast<uint32_t>(strnlen(name, size - i));

                if (i + len >= size) {
                    break;  // unterminated: not a name to open
                }

                job.textures.push_back(name);
                i += len + 1;
            }
        } else if (magic == kMH2O) {
            job.mh2o = b + o + 8;
            job.mh2oLen = size;
        } else if (magic == kMCNK && size >= 0x80) {
            uint32_t ix = Rd32(b + o + 8 + 4);
            uint32_t iy = Rd32(b + o + 8 + 8);

            if (ix > 15 || iy > 15) {
                ix = seen % 16;
                iy = seen / 16;
            }

            if (seen < 256) {
                job.chunks[iy * 16 + ix] = b + o;
                job.chunkLen[iy * 16 + ix] = size + 8;
            }

            seen++;
        }

        o += 8 + size;
    }

    for (const uint8_t* c : job.chunks) {
        if (!c) {
            return false;
        }
    }

    return seen >= 256;
}

// One row of 16 chunks into img (job.px a tile, RGB 0..255, rows = the tile's rows): per texel, each layer's
// texture at the texel's place in its repeat (8 repeats a chunk, u along the columns and v along the rows
// as the terrain vertex shader maps them, at the 4 x 4 / 2 x 2 / 1 x 1 reduction the density calls for),
// composited by the alpha maps as the terrain pixel shader does (big alpha: weighted sum, perm 14; else each
// layer over the ones below, perm 6), times MCCV (x 2 / 255, bilinear on the 9 x 9 grid) and the baked shadow
// (the shader's 0.7 + 0.3 lit). MCAL is sized by the MCNK's sizeAlpha, as CMapChunk does: the MCAL header lies
// on Northrend. No saturation boost: these are the texels the real terrain samples at this density.
void CompositeRow(const BakeJob& job, uint32_t iy, float* img) {
    const uint32_t kMCLY = 0x4D434C59;
    const uint32_t kMCAL = 0x4D43414C;
    const uint32_t kMCSH = 0x4D435348;
    const uint32_t kMCCV = 0x4D434356;
    const bool bigAlpha = (CMap::header.m_flags & 0x4) != 0;
    const uint32_t P = job.px;
    const uint32_t tp = P / 16;                         // texels a chunk
    const uint32_t as = 64 / tp;                        // alpha / shadow texels an atlas texel spans
    const uint32_t rp = tp >= 32 ? 4 : tp >= 16 ? 2 : 1; // texels a texture repeat (4 x 4 / 2 x 2 / 1 x 1)
    const uint32_t cs = tp >= 8 ? 1 : 8 / tp;           // liquid cells an atlas texel spans
    static uint8_t alpha[3][4096];

    for (uint32_t ix = 0; ix < 16; ix++) {
        const uint32_t ch = iy * 16 + ix;
        const uint8_t* c = job.chunks[ch];
        const uint8_t* h = c + 8;
        const uint32_t len = job.chunkLen[ch];
        const uint32_t flags = Rd32(h);

        // Liquid per cell (8 x 8): its LiquidType's kind + 1, 0 = dry. MH2O, or the old per-chunk flags.
        uint8_t wet[64] = {};
        const uint8_t* mh2o = job.mh2o;
        const uint32_t mh2oLen = job.mh2oLen;

        if (mh2o && mh2oLen >= 256 * 12) {
            const uint32_t ofs = Rd32(mh2o + ch * 12);
            const uint32_t layers = Rd32(mh2o + ch * 12 + 4);

            for (uint32_t l = 0; l < layers && ofs + (l + 1) * 24 <= mh2oLen; l++) {
                const uint8_t* inst = mh2o + ofs + l * 24;
                const LiquidTypeRec* rec = g_liquidTypeDB.GetRecord(inst[0] | inst[1] << 8);
                const uint8_t kind = static_cast<uint8_t>((rec ? rec->m_soundBank & 3 : 0) + 1);
                const uint32_t x0 = inst[12], y0 = inst[13], w = inst[14], hh = inst[15];
                const uint32_t exists = Rd32(inst + 16);

                for (uint32_t y = 0; y < hh && y0 + y < 8; y++) {
                    for (uint32_t x = 0; x < w && x0 + x < 8; x++) {
                        const uint32_t bit = y * w + x;

                        if (!exists || (exists + bit / 8 < mh2oLen && ((mh2o[exists + bit / 8] >> (bit % 8)) & 1))) {
                            wet[(y0 + y) * 8 + x0 + x] = kind;
                        }
                    }
                }
            }
        } else if (flags & 0x3C) {
            const uint8_t kind = flags & 0x4 ? 1 : flags & 0x8 ? 2 : flags & 0x10 ? 3 : 4;
            memset(wet, kind, sizeof(wet));
        }

        uint32_t mclyLen = 0;
        uint32_t mcalLen = 0;
        uint32_t mcshLen = 0;
        uint32_t mccvLen = 0;
        const uint8_t* mcly = FindSub(c, len, Rd32(h + 0x1C), kMCLY, mclyLen);
        const uint8_t* mcal = FindSub(c, len, Rd32(h + 0x24), kMCAL, mcalLen);

        if (mcal) {
            const uint32_t declared = Rd32(h + 0x28);
            const uint32_t room = static_cast<uint32_t>(c + len - mcal);

            if (declared > 8) {
                mcalLen = declared - 8 < room ? declared - 8 : room;
            }
        }

        const uint8_t* mcsh = FindSub(c, len, Rd32(h + 0x2C), kMCSH, mcshLen);
        const uint8_t* mccv = FindSub(c, len, Rd32(h + 0x74), kMCCV, mccvLen);

        if (mcshLen < 512 || !(flags & 1)) {
            mcsh = nullptr;
        }

        if (mccvLen < 145 * 4) {
            mccv = nullptr;
        }

        uint32_t layers = Rd32(h + 0xC);
        layers = layers < (mcly ? mclyLen / 16 : 0) ? layers : (mcly ? mclyLen / 16 : 0);
        layers = layers < 4 ? layers : 4;
        const LayerTex* tex[4] = {};
        bool hasAlpha[4] = {};

        for (uint32_t l = 0; l < layers; l++) {
            const uint8_t* e = mcly + l * 16;
            const uint32_t t = Rd32(e);
            static const LayerTex s_grey = [] { LayerTex g; for (auto& v : g.m4) { v[0] = v[1] = v[2] = 128.0f; } for (auto& v : g.m2) { v[0] = v[1] = v[2] = 128.0f; } g.m1[0] = g.m1[1] = g.m1[2] = 128.0f; return g; }();
            tex[l] = t < job.textures.size() ? &LayerTexOf(job.textures[t]) : &s_grey;

            if (l && (Rd32(e + 4) & 0x100)) {
                DecodeAlpha(mcal, mcalLen, Rd32(e + 8), Rd32(e + 4), bigAlpha, (flags & 0x8000) != 0, alpha[l - 1]);
                hasAlpha[l] = true;
            }
        }

        for (uint32_t py = 0; py < tp; py++) {
            for (uint32_t px = 0; px < tp; px++) {
                float* out = img + ((iy * tp + py) * P + ix * tp + px) * 3;
                const uint32_t cx0 = px * 8 / tp, cy0 = py * 8 / tp;
                uint32_t wetCount = 0;
                uint8_t kind = 0;

                for (uint32_t y = 0; y < cs; y++) {
                    for (uint32_t x = 0; x < cs; x++) {
                        if (const uint8_t k = wet[(cy0 + y) * 8 + cx0 + x]) {
                            wetCount++;
                            kind = k;
                        }
                    }
                }

                if (wetCount * 2 >= cs * cs && wetCount) {
                    for (int j = 0; j < 3; j++) {
                        out[j] = kLiquid[kind - 1][j];
                    }

                    continue;
                }

                float a[4] = {};

                for (uint32_t l = 1; l < layers; l++) {
                    if (!hasAlpha[l]) {
                        continue;
                    }

                    uint32_t sum = 0;

                    for (uint32_t yy = 0; yy < as; yy++) {
                        for (uint32_t xx = 0; xx < as; xx++) {
                            sum += alpha[l - 1][(py * as + yy) * 64 + px * as + xx];
                        }
                    }

                    a[l] = static_cast<float>(sum) / (static_cast<float>(as * as) * 255.0f);
                }

                const uint32_t k = (py % rp) * rp + px % rp;
                auto colour = [&](uint32_t l) { return rp == 4 ? tex[l]->m4[k] : rp == 2 ? tex[l]->m2[k] : tex[l]->m1; };

                if (!layers) {
                    out[0] = out[1] = out[2] = 128.0f;
                } else if (bigAlpha) {
                    float base = 1.0f;

                    for (uint32_t l = 1; l < layers; l++) {
                        base -= a[l];
                    }

                    for (int j = 0; j < 3; j++) {
                        out[j] = colour(0)[j] * (base > 0.0f ? base : 0.0f);

                        for (uint32_t l = 1; l < layers; l++) {
                            out[j] += colour(l)[j] * a[l];
                        }
                    }
                } else {
                    for (int j = 0; j < 3; j++) {
                        out[j] = colour(0)[j];

                        for (uint32_t l = 1; l < layers; l++) {
                            out[j] += (colour(l)[j] - out[j]) * a[l];
                        }
                    }
                }

                if (mccv) {
                    const float gx = (px + 0.5f) * 8.0f / tp;
                    const float gy = (py + 0.5f) * 8.0f / tp;
                    const uint32_t c0 = static_cast<uint32_t>(gx) < 7 ? static_cast<uint32_t>(gx) : 7;
                    const uint32_t r0 = static_cast<uint32_t>(gy) < 7 ? static_cast<uint32_t>(gy) : 7;
                    const float fx = gx - c0;
                    const float fy = gy - r0;

                    for (int j = 0; j < 3; j++) {
                        auto v = [&](uint32_t r, uint32_t q) { return static_cast<float>(mccv[(r * 17 + q) * 4 + (2 - j)]); };
                        const float m = (v(r0, c0) * (1 - fx) + v(r0, c0 + 1) * fx) * (1 - fy) + (v(r0 + 1, c0) * (1 - fx) + v(r0 + 1, c0 + 1) * fx) * fy;
                        out[j] *= m * 2.0f / 255.0f;
                    }
                }

                if (mcsh) {
                    uint32_t on = 0;

                    for (uint32_t y = 0; y < as; y++) {
                        for (uint32_t x = 0; x < as; x++) {
                            on += (mcsh[(py * as + y) * 8 + (px * as + x) / 8] >> ((px * as + x) % 8)) & 1;
                        }
                    }

                    for (int j = 0; j < 3; j++) {
                        out[j] *= 1.0f - 0.3f * static_cast<float>(on) / static_cast<float>(as * as);
                    }
                }

                for (int j = 0; j < 3; j++) {
                    out[j] = out[j] < 0.0f ? 0.0f : out[j] > 255.0f ? 255.0f : out[j];
                }
            }
        }
    }
}

uint16_t To565(const float* c) {
    const uint32_t r = static_cast<uint32_t>(c[0] * 31.0f / 255.0f + 0.5f);
    const uint32_t g = static_cast<uint32_t>(c[1] * 63.0f / 255.0f + 0.5f);
    const uint32_t b = static_cast<uint32_t>(c[2] * 31.0f / 255.0f + 0.5f);
    return static_cast<uint16_t>((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
}

// 16 RGB texels as one opaque DXT1 block: the box's corners as the ends, four-colour mode, each texel the
// palette entry its projection on the end-to-end line falls nearest (range fit; about 3x the old nearest-of-4
// search, which mattered once a tile became 28 000 blocks).
void EncodeDxt1(const float (*px)[3], uint8_t* out) {
    float lo[3] = { 255.0f, 255.0f, 255.0f };
    float hi[3] = { 0.0f, 0.0f, 0.0f };

    for (uint32_t k = 0; k < 16; k++) {
        for (int j = 0; j < 3; j++) {
            lo[j] = px[k][j] < lo[j] ? px[k][j] : lo[j];
            hi[j] = px[k][j] > hi[j] ? px[k][j] : hi[j];
        }
    }

    uint16_t c0 = To565(hi);
    uint16_t c1 = To565(lo);
    uint32_t bits = 0;

    if (c0 < c1) {
        const uint16_t t = c0;
        c0 = c1;
        c1 = t;
    }

    if (c0 != c1) {
        float e0[3], e1[3], axis[3];
        From565(c0, e0);
        From565(c1, e1);
        float len = 0.0f;

        for (int j = 0; j < 3; j++) {
            axis[j] = e0[j] - e1[j];
            len += axis[j] * axis[j];
        }

        const float inv = len > 0.0f ? 1.0f / len : 0.0f;
        static const uint32_t s_index[4] = { 1, 3, 2, 0 };  // t from c1 (0) to c0 (1): c1, 1/3, 2/3, c0

        for (uint32_t k = 0; k < 16; k++) {
            const float t = ((px[k][0] - e1[0]) * axis[0] + (px[k][1] - e1[1]) * axis[1] + (px[k][2] - e1[2]) * axis[2]) * inv;
            const int32_t q = static_cast<int32_t>(t * 3.0f + 0.5f);
            bits |= s_index[q < 0 ? 0 : q > 3 ? 3 : q] << (2 * k);
        }
    }

    out[0] = static_cast<uint8_t>(c0);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1);
    out[3] = static_cast<uint8_t>(c1 >> 8);
    memcpy(out + 4, &bits, 4);
}

uint8_t* AtlasBlock(Atlas& at, uint32_t level, uint32_t bx, uint32_t by) {
    return at.chain + s_far.levelOffset[level] + (by * ((kAtlasPx >> level) / 4) + bx) * 8;
}

void AtlasUserFunc(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    const Atlas* at = static_cast<const Atlas*>(userArg);

    if (command == GxTex_Latch && at && at->chain && mipLevel < kLevels) {
        texelStrideInBytes = (kAtlasPx >> mipLevel) / 4 * 8;
        texels = at->chain + s_far.levelOffset[mipLevel];
    }
}

uint32_t Slots(uint32_t a) {
    return kAtlasPx / kTilePxOf[a];
}

uint32_t SlotOf(uint32_t a, uint32_t t) {
    const uint32_t n = Slots(a);
    return ((t / 64) % n) * n + (t % 64) % n;
}

// The atlases farTexQuality and farTexMemory allow: coarse always (the old 8.3 yd ground), then mid, then near,
// each 21.3 MB (10.7 on the GPU, the same again for its copy).
bool AtlasOn(uint32_t a) {
    if (a == 2) {
        return true;
    }

    const int32_t quality = TexQualityCVar()->m_intValue;
    const float mb = static_cast<float>(TexMemoryCVar()->m_intValue);
    return quality >= static_cast<int32_t>(2 - a) && mb >= 21.4f * static_cast<float>(3 - a);
}

// A window atlas holds the tiles within half its slots less one of the camera's tile (so no two of them share
// a slot); the coarse one holds the map.
bool InWindow(uint32_t a, uint32_t t) {
    if (a == 2) {
        return true;
    }

    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const int32_t cc = static_cast<int32_t>(floorf((kOrigin - cam.y) / kTileYd));
    const int32_t cr = static_cast<int32_t>(floorf((kOrigin - cam.x) / kTileYd));
    const int32_t half = static_cast<int32_t>(Slots(a) / 2) - 1;
    return abs(static_cast<int32_t>(t % 64) - cc) <= half && abs(static_cast<int32_t>(t / 64) - cr) <= half;
}

bool Has(uint32_t a, uint32_t t) {
    return s_far.atlas[a].tex && (a == 2 ? s_far.tile[t] == Tile_Baked : s_far.atlas[a].owner[SlotOf(a, t)] == static_cast<int16_t>(t));
}

bool Wants(uint32_t a, uint32_t t) {
    return AtlasOn(a) && InWindow(a, t) && (a == 2 ? s_far.tile[t] == Tile_None : !Has(a, t));
}

CGxTex* AtlasTex(uint32_t a) {
    return s_far.atlas[a].tex ? TextureGetGxTex(s_far.atlas[a].tex, 0, nullptr) : nullptr;
}

// The atlas a tile draws with: the coarsest one baked whose texel stays under farTexPixels at pxPerYd, else
// the finest baked; -1 when none has it (drawn fog-coloured). Distance only: the same whatever is in view.
int32_t PickAtlas(uint32_t t, float pxPerYd) {
    const float limit = fmaxf(TexPixelsCVar()->m_floatValue, 0.1f);
    int32_t finest = -1;

    for (int32_t a = 2; a >= 0; a--) {
        if (!AtlasOn(a) || !Has(a, t)) {
            continue;
        }

        if (kTileYd / kTilePxOf[a] * pxPerYd <= limit) {
            return a;
        }

        finest = a;
    }

    return finest;
}

// Box-filter src (n x n RGB) to n / 2.
void Half(const float* src, uint32_t n, float* dst) {
    const uint32_t m = n / 2;

    for (uint32_t y = 0; y < m; y++) {
        for (uint32_t x = 0; x < m; x++) {
            for (int j = 0; j < 3; j++) {
                dst[(y * m + x) * 3 + j] = (src[((2 * y) * n + 2 * x) * 3 + j] + src[((2 * y) * n + 2 * x + 1) * 3 + j] + src[((2 * y + 1) * n + 2 * x) * 3 + j] + src[((2 * y + 1) * n + 2 * x + 1) * 3 + j]) * 0.25f;
            }
        }
    }
}

float s_img[kBakePx * kBakePx * 3];
float s_lv[kBakePx * kBakePx * 3 * 4 / 3 + 64];   // the current atlas's tile levels, largest first
uint32_t s_lvOffset[kLevels];

void EnsureAtlas(uint32_t a) {
    Atlas& at = s_far.atlas[a];

    if (at.tex) {
        return;
    }

    if (!s_far.levelOffset[1]) {
        uint32_t size = 0;

        for (uint32_t l = 0; l < kLevels; l++) {
            s_far.levelOffset[l] = size;
            size += (kAtlasPx >> l) * (kAtlasPx >> l) / 2;
        }
    }

    const uint32_t bytes = s_far.levelOffset[kLevels - 1] + 8;
    at.chain = new uint8_t[bytes]();
    memset(at.owner, 0xFF, sizeof(at.owner));
    static const char* const s_names[kAtlases] = { "WdlGroundNear", "WdlGroundMid", "WdlGroundAtlas" };
    at.tex = TextureCreate(kAtlasPx, kAtlasPx, GxTex_Dxt1, GxTex_Dxt1, CGxTexFlags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1), &at, AtlasUserFunc, s_names[a], 0);

    if (CGxTex* gxTex = AtlasTex(a)) {
        GxTexUpdate(gxTex, 0, 0, kAtlasPx, kAtlasPx, 1);  // the whole chain once, so nothing on the GPU is uninitialised
    }
}

// The job's current atlas: its tile's levels from the composite (box-filtered down from job.px).
void PrepareLevels(BakeJob& job) {
    const uint32_t q = kTilePxOf[job.atlas];
    float* l0 = s_lv;
    const uint32_t f = job.px / q;

    for (uint32_t y = 0; y < q; y++) {
        for (uint32_t x = 0; x < q; x++) {
            for (int j = 0; j < 3; j++) {
                float acc = 0.0f;

                for (uint32_t yy = 0; yy < f; yy++) {
                    for (uint32_t xx = 0; xx < f; xx++) {
                        acc += s_img[((y * f + yy) * job.px + x * f + xx) * 3 + j];
                    }
                }

                l0[(y * q + x) * 3 + j] = acc / static_cast<float>(f * f);
            }
        }
    }

    uint32_t off = 0;

    for (uint32_t l = 0; l < kLevels && (q >> l) >= 1; l++) {
        s_lvOffset[l] = off;

        if (l) {
            Half(s_lv + s_lvOffset[l - 1], q >> (l - 1), s_lv + off);
        }

        off += (q >> l) * (q >> l) * 3;
    }

    const uint32_t last = s_lvOffset[__builtin_ctz(q)];  // the 1 x 1 level: the tile's average

    for (int j = 0; j < 3; j++) {
        s_far.avg[job.read->tile][j] = static_cast<uint8_t>(s_lv[last + j]);
    }
}

// One slice of the store: up to 16 block rows of the current level, or (past the tile's own levels) the
// blocks where a texel spans tiles, from the slots' averages. True when the atlas is finished.
bool StoreSlice(BakeJob& job) {
    const uint32_t a = job.atlas;
    Atlas& at = s_far.atlas[a];
    const uint32_t q = kTilePxOf[a];
    const uint32_t n = Slots(a);
    const uint32_t t = job.read->tile;
    const uint32_t sx = (t % 64) % n, sy = (t / 64) % n;

    if ((q >> job.level) >= 4) {
        const uint32_t w = q >> job.level;
        const uint32_t bw = w / 4;
        const float* src = s_lv + s_lvOffset[job.level];

        for (uint32_t end = job.row + 16 < bw ? job.row + 16 : bw; job.row < end; job.row++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                float px[16][3];

                for (uint32_t k = 0; k < 16; k++) {
                    for (int j = 0; j < 3; j++) {
                        px[k][j] = src[((job.row * 4 + k / 4) * w + bx * 4 + k % 4) * 3 + j];
                    }
                }

                EncodeDxt1(px, AtlasBlock(at, job.level, sx * bw + bx, sy * bw + job.row));
            }
        }

        if (job.row >= bw) {
            job.row = 0;
            job.level++;
        }

        return false;
    }

    // Levels where a block spans slots: each texel the average of the slots it covers that hold a tile.
    at.owner[sy * n + sx] = static_cast<int16_t>(t);

    if (a == 2) {
        s_far.tile[t] = Tile_Baked;
    }

    for (uint32_t l = job.level; l < kLevels; l++) {
        const uint32_t bx = ((sx * q) >> l) / 4;
        const uint32_t by = ((sy * q) >> l) / 4;
        float px[16][3] = {};

        for (uint32_t k = 0; k < 16; k++) {
            const uint32_t p = bx * 4 + k % 4;
            const uint32_t r = by * 4 + k / 4;
            const uint32_t c0 = (p << l) / q, c1 = (((p + 1) << l) - 1) / q;
            const uint32_t r0 = (r << l) / q, r1 = (((r + 1) << l) - 1) / q;
            float acc[3] = {};
            uint32_t got = 0;

            for (uint32_t y = r0; y <= r1 && y < n; y++) {
                for (uint32_t x = c0; x <= c1 && x < n; x++) {
                    const int16_t o = at.owner[y * n + x];

                    if (o >= 0 && (a != 2 || s_far.tile[o] == Tile_Baked)) {
                        for (int j = 0; j < 3; j++) {
                            acc[j] += s_far.avg[o][j];
                        }

                        got++;
                    }
                }
            }

            for (int j = 0; j < 3; j++) {
                px[k][j] = got ? acc[j] / got : 0.0f;
            }
        }

        EncodeDxt1(px, AtlasBlock(at, l, bx, by));
    }

    if (CGxTex* gxTex = AtlasTex(a)) {
        GxTexUpdate(gxTex, sx * q, sy * q, (sx + 1) * q, (sy + 1) * q, 1);
    }

    return true;
}

void EndJob(BakeJob& job) {
    if (job.read) {
        s_far.busy[job.read->tile] = 0;
        delete[] job.read->buf;
        delete job.read;
    }

    job = BakeJob {};
}

// One slice of the job: the heights, one chunk row of the composite, or one store slice.
void StepJob(BakeJob& job) {
    FarRead* read = job.read;
    const uint32_t t = read->tile;

    if (job.step == 0) {
        job.step = 1;

        if (read->heights) {
            FarLod::AdtRead(t, read->buf, read->size);
        }

        // Not in 12340 (farTrees): the same ADT's tree and bush placements, once a tile (its first bake read).
        if (read->bake && s_far.tile[t] == Tile_None) {
            FarField::FoliageScanTile(t, read->buf, read->size);
        }

        uint32_t px = 0;

        for (uint32_t a = 0; a < kAtlases; a++) {
            job.want[a] = read->bake && Wants(a, t);
            px = job.want[a] && kTilePxOf[a] > px ? kTilePxOf[a] : px;
        }

        if (!px) {
            EndJob(job);
        } else if (!ParseAdt(job)) {
            s_far.tile[t] = Tile_Failed;
            EndJob(job);
        } else {
            job.px = px;
        }

        return;
    }

    if (job.step <= 16) {
        CompositeRow(job, job.step - 1, s_img);

        if (++job.step > 16) {
            delete[] read->buf;  // the composite is all the store needs
            read->buf = nullptr;
            job.atlas = 0;
        }

        return;
    }

    while (job.atlas < kAtlases && (!job.want[job.atlas] || !AtlasOn(job.atlas) || !InWindow(job.atlas, t))) {
        job.atlas++;  // not wanted, or the camera has moved it out of the window since
    }

    if (job.atlas >= kAtlases) {
        s_far.baked++;
        s_far.bakeMs += job.ms;
        EndJob(job);
        return;
    }

    if (!job.prepared) {
        EnsureAtlas(job.atlas);
        PrepareLevels(job);
        job.prepared = true;
        job.level = 0;
        job.row = 0;
        return;
    }

    if (StoreSlice(job)) {
        job.atlas++;
        job.prepared = false;
    }
}

void ReadDone(void* param) {
    FarRead* read = static_cast<FarRead*>(param);
    AsyncFileReadDestroyObject(read->obj);  // closes the file

    if (read->gen != s_far.gen) {
        delete[] read->buf;
        delete read;
        return;
    }

    s_far.inFlight--;
    s_far.ready.push_back(read);
}

// Nearest tile first, within the horizon's reach: a couple of ADT reads in flight on the async queue, and the
// finished ones baked a slice at a time, about kBakeBudgetMs a frame (one slice always). Which tiles and
// atlases is by distance only, so turning the camera changes nothing.
void PumpBake() {
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float cc = (kOrigin - cam.y) / kTileYd;
    const float cr = (kOrigin - cam.x) / kTileYd;
    const float reach = World::s_horizonFarClipScale * World::s_farClip / kTileYd + 1.5f;

    while (s_far.inFlight < kReadsInFlight && s_far.ready.size() < kReadsInFlight) {
        int32_t best = -1;
        float bestD = reach * reach;

        for (uint32_t i = 0; i < 4096; i++) {
            if (s_far.busy[i] || s_far.tile[i] == Tile_Failed || !CMap::s_lowDetail.m_areas[i]) {
                continue;
            }

            const float dx = static_cast<float>(i % 64) + 0.5f - cc;
            const float dy = static_cast<float>(i / 64) + 0.5f - cr;
            const float d = dx * dx + dy * dy;

            if (d < bestD && (Wants(2, i) || Wants(1, i) || Wants(0, i) || FarLod::WantsAdt(i))) {
                bestD = d;
                best = static_cast<int32_t>(i);
            }
        }

        if (best < 0) {
            break;
        }

        char name[256];
        SStrPrintf(name, sizeof(name), "%s\\%s_%d_%d.adt", CMap::mapPath, CMap::mapName, best % 64, best / 64);
        SFileImpl* file = nullptr;

        const bool bake = Wants(2, best) || Wants(1, best) || Wants(0, best);
        const bool heights = FarLod::WantsAdt(best);

        if (heights) {
            FarLod::MarkReading(best);
        }

        if (!SFileOpenFile(name, &file) || !file) {
            if (s_far.tile[best] != Tile_Baked) {
                s_far.tile[best] = Tile_Failed;
            }

            if (heights) {
                FarLod::AdtRead(best, nullptr, 0);
            }

            continue;
        }

        FarRead* read = new FarRead;
        read->bake = bake;
        read->heights = heights;
        read->size = SFileGetFileSize(file, nullptr);
        read->buf = new uint8_t[read->size ? read->size : 1];
        read->tile = static_cast<uint32_t>(best);
        read->gen = s_far.gen;
        read->obj = AsyncFileReadAllocObject();
        read->obj->m_file = file;
        read->obj->m_buffer = read->buf;
        read->obj->m_size = read->size;
        read->obj->m_userArg = read;
        read->obj->m_userPostloadCallback = reinterpret_cast<void*>(&ReadDone);
        s_far.busy[best] = 1;
        s_far.inFlight++;
        AsyncFileReadObject(read->obj, 0);
    }

    const auto begin = std::chrono::steady_clock::now();

    for (;;) {
        if (!s_far.job.read) {
            if (s_far.ready.empty()) {
                break;
            }

            s_far.job.read = s_far.ready.front();
            s_far.ready.erase(s_far.ready.begin());
        }

        const auto t0 = std::chrono::steady_clock::now();
        BakeJob& job = s_far.job;
        StepJob(job);
        const auto t1 = std::chrono::steady_clock::now();

        if (job.read) {
            job.ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        }

        if (std::chrono::duration<double, std::milli>(t1 - begin).count() >= kBakeBudgetMs) {
            break;
        }
    }
}

// Block b's static buffers: every WDL tile of its 8 x 8 as PN, and per tile its culled-off cells then (in
// a second region) its culled-on hole cells, so adjacent tiles' runs are adjacent in the buffer.
bool BuildBlock(uint32_t b) {
    FarBlock& block = s_far.blocks[b];
    CMapAreaLow* tiles[64] = {};
    uint32_t count = 0;

    for (uint32_t k = 0; k < 64; k++) {
        tiles[k] = CMap::s_lowDetail.m_areas[((b / 8) * 8 + k / 8) * 64 + (b % 8) * 8 + k % 8];
        count += tiles[k] ? 1 : 0;
    }

    if (!block.init) {
        block.init = true;
        block.verts = count * kTileVerts;

        if (!count) {
            return false;
        }

        block.vertexPool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Vertex, GxPoolUsage_Static, block.verts * sizeof(CGxVertexPN), GxPoolHintBit_Unk0, "FarTerrainVB");
        block.indexPool = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Index, GxPoolUsage_Static, count * kTileIndices * sizeof(uint16_t), GxPoolHintBit_Unk0, "FarTerrainIB");
        block.vertexBuf = g_theGxDevicePtr->BufCreate(block.vertexPool, sizeof(CGxVertexPN), block.verts, 0);
        block.indexBuf = g_theGxDevicePtr->BufCreate(block.indexPool, sizeof(uint16_t), count * kTileIndices, 0);
    }

    if (!block.verts || !block.vertexBuf || !block.indexBuf) {
        return false;
    }

    if (block.vertexBuf->m_hasData && block.vertexBuf->m_valid && block.indexBuf->m_hasData && block.indexBuf->m_valid) {
        return true;
    }

    CGxVertexPN* vertex = reinterpret_cast<CGxVertexPN*>(g_theGxDevicePtr->BufLock(block.vertexBuf));
    uint16_t* first = reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(block.indexBuf));
    uint16_t* index = first;
    uint32_t base = 0;

    for (uint32_t pass = 0; pass < 2; pass++) {
        base = 0;

        for (uint32_t k = 0; k < 64; k++) {
            CMapAreaLow* area = tiles[k];

            if (!area) {
                continue;
            }

            const uint32_t t = static_cast<uint32_t>(area->m_index.y) * 64 + area->m_index.x;

            if (!pass) {
                WriteGridNormals(area, vertex + base);
            }

            // No holes: every cell culled off (12340 drew a tile without holes that way); with holes the
            // solid cells culled off, the hole cells culled on.
            uint16_t* end = (pass && !area->m_holes) ? index : WriteGridIndices(index, area->m_holes, static_cast<int32_t>(pass), base);
            s_far.start[t][pass] = static_cast<uint32_t>(index - first);
            s_far.count[t][pass] = static_cast<uint32_t>(end - index);
            index = end;
            base += kTileVerts;
        }
    }

    g_theGxDevicePtr->BufUnlock(block.vertexBuf, 0);
    g_theGxDevicePtr->BufUnlock(block.indexBuf, 0);
    block.vertexBuf->m_hasData = 1;
    block.indexBuf->m_hasData = 1;
    return true;
}

void DrawRuns(uint32_t b, uint64_t mask, uint32_t pass) {
    uint32_t runStart = 0;
    uint32_t runEnd = 0;

    auto flush = [&]() {
        if (runEnd > runStart) {
            CGxBatch batch;
            batch.m_primType = GxPrim_Triangles;
            batch.m_start = runStart;
            batch.m_count = runEnd - runStart;
            batch.m_minIndex = 0;
            batch.m_maxIndex = s_far.blocks[b].verts - 1;
            g_theGxDevicePtr->Draw(&batch, 1);
        }

        runStart = runEnd = 0;
    };

    for (uint32_t k = 0; k < 64; k++) {
        if (!(mask & (1ull << k))) {
            continue;
        }

        const uint32_t t = ((b / 8) * 8 + k / 8) * 64 + (b % 8) * 8 + k % 8;
        const uint32_t start = s_far.start[t][pass];
        const uint32_t count = s_far.count[t][pass];

        if (!count) {
            continue;
        }

        if (start != runEnd || runEnd == runStart) {
            flush();
            runStart = start;
        }

        runEnd = start + count;
    }

    flush();
}

// The coarse atlas, when this tile's bake is in it (the per-tile path and the fixed-function fallback).
CGxTex* FarAtlasFor(const CMapAreaLow* area) {
    const uint32_t t = static_cast<uint32_t>(area->m_index.y) * 64 + area->m_index.x;
    return Has(2, t) ? AtlasTex(2) : nullptr;
}

// The terrain shader's uv for atlas a: uv = (pos.yx - c23.yx) * scale.yx, the map's corner at 0, one window of
// tiles to 1 (wrap addressing takes the rest).
void AtlasTexConstants(CMapRenderChunk::SVertexShaderConstants& consts, uint32_t a) {
    const float scale = -1.0f / (static_cast<float>(Slots(a)) * kTileYd);
    consts.m_position.x = kOrigin;
    consts.m_position.y = kOrigin;
    consts.m_position.z = 0.0f;
    consts.m_position.w = 0.0f;

    for (int32_t i = 0; i < 2; i++) {
        consts.m_texOffset[i].x = consts.m_texOffset[i].y = consts.m_texOffset[i].z = consts.m_texOffset[i].w = 0.0f;
        consts.m_texScale[i + 1].x = consts.m_texScale[i + 1].y = scale;
        consts.m_texScale[i + 1].z = consts.m_texScale[i + 1].w = 0.0f;
    }
}

// Binds atlas a on stages 0 and 1 (wrapping) and uploads its uv scale (c18, c19).
void BindAtlas(CMapRenderChunk::SVertexShaderConstants& consts, uint32_t a) {
    CGxTex* tex = AtlasTex(a);

    if (tex) {
        GxTexSetWrap(tex, GxTex_Wrap, GxTex_Wrap);
    }

    GxRsSet(GxRs_Texture0, static_cast<void*>(tex));
    GxRsSet(GxRs_Texture1, static_cast<void*>(tex));
    AtlasTexConstants(consts, a);
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, offsetof(CMapRenderChunk::SVertexShaderConstants, m_texScale[1]) / sizeof(C4Vector), reinterpret_cast<const float*>(&consts.m_texScale[1]), 2);
}

}  // namespace

void FarField::EncodeDxt1Block(const float (*px)[3], uint8_t* out) {
    EncodeDxt1(px, out);
}

void FarField::Reset() {
    if (!g_theGxDevicePtr) {
        return;
    }

    s_far.gen++;  // reads still in flight free themselves (ReadDone)
    FarLod::Reset();
    s_far.inFlight = 0;

    for (FarRead* read : s_far.ready) {
        delete[] read->buf;
        delete read;
    }

    s_far.ready.clear();

    for (FarBlock& block : s_far.blocks) {
        if (block.vertexBuf) {
            g_theGxDevicePtr->BufDestroy(block.vertexBuf);
        }

        if (block.indexBuf) {
            g_theGxDevicePtr->BufDestroy(block.indexBuf);
        }

        if (block.vertexPool) {
            g_theGxDevicePtr->PoolDestroy(block.vertexPool);
        }

        if (block.indexPool) {
            g_theGxDevicePtr->PoolDestroy(block.indexPool);
        }

        block = FarBlock {};
    }

    EndJob(s_far.job);

    for (Atlas& at : s_far.atlas) {
        if (at.tex) {
            HandleClose(reinterpret_cast<HOBJECT>(at.tex));
            at.tex = nullptr;
        }

        delete[] at.chain;
        at.chain = nullptr;
    }

    memset(s_far.tile, 0, sizeof(s_far.tile));
    FarField::FoliageReset();
    memset(s_far.busy, 0, sizeof(s_far.busy));
    s_far.band.clear();
}

bool FarField::RenderTerrain() {
    if (!WdlTexturedCVar()->m_intValue) {
        return false;
    }

    PumpBake();

    CGxShader* vertexShader = CMap::vertexShader_Terrain[0];
    CGxShader* pixelShader = CMap::pixelShader_Terrain1[0];

    if (!HorizonBatchedCVar()->m_intValue || FarField::FogStretch() == 1.0f || !CMap::enableTerrainShaderVertex || !vertexShader || !vertexShader->Valid() || !pixelShader || !pixelShader->Valid()) {
        return false;
    }

    // The visible tiles per block, one set per atlas (by distance: PickAtlas) and the not-baked ones
    // (fog-coloured). Tiles wholly nearer than the horizon's near plane (farZ - 50) are skipped: every point of
    // them is clipped.
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float nearZ = CGWorldFrame::GetActiveCamera()->m_farZ - 50.0f;
    const CRect& rect = g_theGxDevicePtr->m_curWindowRect;
    const float pxScale = 0.5f * (rect.maxY - rect.minY) * fabsf(g_theGxDevicePtr->m_projNative.b1);
    uint64_t masks[kAtlases + 1][64] = {};
    CMapAreaLow* next;
    FarLod::BeginFrame();

    for (CMapAreaLow* area = CWorldScene::sortTable.m_horizonChunkList.Head(); area; area = next) {
        next = CWorldScene::sortTable.m_horizonChunkList.Next(area);
        area->m_horizonLink.Unlink();

        const float dx = fmaxf(fabsf(cam.x - area->m_bbox.b.x), fabsf(cam.x - area->m_bbox.t.x));
        const float dy = fmaxf(fabsf(cam.y - area->m_bbox.b.y), fabsf(cam.y - area->m_bbox.t.y));
        const float dz = fmaxf(fabsf(cam.z - area->m_bbox.b.z), fabsf(cam.z - area->m_bbox.t.z));

        if (static_cast<int32_t>(area->m_indexCount) <= 0 || dx * dx + dy * dy + dz * dz < nearZ * nearZ) {
            continue;
        }

        const uint32_t col = area->m_index.x;
        const uint32_t row = area->m_index.y;
        const uint32_t t = row * 64 + col;
        const uint32_t b = (row / 8) * 8 + col / 8;
        const uint64_t bit = 1ull << ((row % 8) * 8 + col % 8);
        const float nx = fmaxf(fmaxf(area->m_bbox.b.x - cam.x, cam.x - area->m_bbox.t.x), 0.0f);
        const float ny = fmaxf(fmaxf(area->m_bbox.b.y - cam.y, cam.y - area->m_bbox.t.y), 0.0f);
        const int32_t a = PickAtlas(t, pxScale / fmaxf(sqrtf(nx * nx + ny * ny), nearZ));
        const uint32_t set = a < 0 ? kAtlases : static_cast<uint32_t>(a);

        if (!FarLod::Claim(t, set)) {  // a detailed level draws it instead (FarTerrainLod.cpp)
            masks[set][b] |= bit;
        }
    }

    // Blocks front to back.
    uint32_t order[64];
    float dist[64];
    uint32_t count = 0;

    for (uint32_t b = 0; b < 64; b++) {
        if (!(masks[0][b] | masks[1][b] | masks[2][b] | masks[3][b]) || !BuildBlock(b)) {
            continue;
        }

        const float cx = kOrigin - ((b / 8) * 8 + 4) * kTileYd - cam.x;
        const float cy = kOrigin - ((b % 8) * 8 + 4) * kTileYd - cam.y;
        uint32_t i = count++;

        for (; i > 0 && dist[i - 1] > cx * cx + cy * cy; i--) {
            order[i] = order[i - 1];
            dist[i] = dist[i - 1];
        }

        order[i] = b;
        dist[i] = cx * cx + cy * cy;
    }

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_Fog, 1);
    GxRsSet(GxRs_DepthTest, 3);

    CMapRenderChunk::SVertexShaderConstants consts;
    FarField::TerrainConstants(consts);
    AtlasTexConstants(consts, 2);
    GxRsSet(GxRs_VertexShader, vertexShader);
    GxRsSet(GxRs_PixelShader, pixelShader);
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&consts), sizeof(consts) / sizeof(C4Vector));
    FarField::BindFogColor();

    CImVector color = DayNight::GetInfo()->m_fog.m_color;
    GxRsSet(GxRs_FogColor, *reinterpret_cast<int32_t*>(&color));
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_ColorOp0, 0);
    GxRsSet(GxRs_AlphaOp0, 0);
    g_theGxDevicePtr->m_xforms[GxXform_Tex0].Identity();

    // Sets 0-2 textured from their atlas; set 3 the tiles not baked yet, fully fogged: fog =
    // pow(max(viewZ * 0 + 0, 0), 1) = 0, so the shader outputs exactly the fog colour (12340's silhouette).
    for (uint32_t set = 0; set <= kAtlases; set++) {
        bool any = false;

        for (uint32_t b = 0; b < 64 && !any; b++) {
            any = masks[set][b] != 0;
        }

        if (!any && !FarLod::Drawing(set)) {
            continue;
        }

        if (set < kAtlases) {
            BindAtlas(consts, set);
        } else {
            BindAtlas(consts, 2);
            const float fogged4[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, offsetof(CMapRenderChunk::SVertexShaderConstants, m_fog) / sizeof(C4Vector), fogged4, 1);
        }

        for (uint32_t i = 0; i < count; i++) {
            const uint32_t b = order[i];

            if (!masks[set][b]) {
                continue;
            }

            GxPrimVertexPtr(s_far.blocks[b].vertexBuf, GxVBF_PN);
            g_theGxDevicePtr->PrimIndexPtr(s_far.blocks[b].indexBuf);
            GxRsSet(GxRs_Culling, 0);
            DrawRuns(b, masks[set][b], 0);
            GxRsSet(GxRs_Culling, 1);
            DrawRuns(b, masks[set][b], 1);
        }

        FarLod::Draw(set);
    }

    g_theGxDevicePtr->RsPop();
    FarLod::EndFrame();

    static uint32_t s_frame = 0;
    static CVar* s_stats = CVar::Lookup("farTerrainLodStats");

    if (s_stats && s_stats->m_intValue && ++s_frame % 600 == 0) {
        uint32_t held[kAtlases] = {};

        for (uint32_t a = 0; a < kAtlases; a++) {
            for (uint32_t i = 0; i < Slots(a) * Slots(a); i++) {
                held[a] += s_far.atlas[a].owner[i] >= 0 ? 1 : 0;
            }
        }

        uint32_t on = 0;

        for (const Atlas& at : s_far.atlas) {
            on += at.tex ? 1 : 0;
        }

        char line[256];
        SStrPrintf(line, sizeof(line), "farTex: near/mid/coarse tiles %u/%u/%u; %u bakes, %.2f ms a tile (in %.1f ms slices); %u atlases, %.1f MB; band %u chunks",
            held[0], held[1], held[2], s_far.baked, s_far.baked ? s_far.bakeMs / s_far.baked : 0.0, kBakeBudgetMs, on, on * 2.0 * (s_far.levelOffset[kLevels - 1] + 8) / 1048576.0,
            static_cast<uint32_t>(s_far.lastBand));
        ConsoleWrite(line, DEFAULT_COLOR);
    }
    g_theGxDevicePtr->m_xforms[GxXform_Tex0].Identity();
    return true;
}

namespace {

C4Vector V4(float x, float y, float z, float w) {
    C4Vector v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    return v;
}

bool BandOn() {
    return BandCVar()->m_intValue && WdlTexturedCVar()->m_intValue && HorizonBatchedCVar()->m_intValue && FarField::FogStretch() != 1.0f && CMap::enableTerrainShaderVertex && !g_theGxDevicePtr->Caps().unk_0134;
}

// The band in ground-plane yards from the camera: it ends 4% short of the view distance, because it is
// measured along the ground and the real terrain is cut by view depth, which a point well below the camera
// reaches at a shorter ground distance.
void BandRange(float& start, float& end) {
    const float cut = CGWorldFrame::GetActiveCamera()->m_farZ;
    end = cut * 0.96f;
    start = fmaxf(end - fmaxf(static_cast<float>(BandWidthCVar()->m_intValue), 1.0f), cut * 0.3f);
}

}  // namespace

void FarField::NoteChunk(CMapRenderChunk* chunk) {
    if (!BandOn()) {
        return;
    }

    float start, end;
    BandRange(start, end);
    const C3Vector d = chunk->m_sphere.c - CWorldScene::s_activeWorldView;

    if (sqrtf(d.x * d.x + d.y * d.y) + chunk->m_sphere.r >= start) {
        s_far.band.push_back(chunk);
    }
}

void FarField::RenderBlend() {
    std::vector<CMapRenderChunk*> chunks;
    chunks.swap(s_far.band);
    s_far.lastBand = chunks.size();
    CGxShader* vertexShader = CMap::vertexShader_Terrain[0];

    if (chunks.empty() || !BandOn() || !vertexShader || !vertexShader->Valid()) {
        return;
    }

    if (!s_far.blendTried) {
        s_far.blendTried = true;
        g_theGxDevicePtr->ShaderCreate(&s_far.blendPs, GxSh_Pixel, "Shaders\\Pixel", "FarBlend", 1);
    }

    if (!s_far.blendPs || !s_far.blendPs->Valid()) {
        return;
    }

    // Each chunk paints from the finest atlas holding its tile; none, and it is left alone.
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    std::vector<CMapRenderChunk*> byAtlas[kAtlases];

    for (CMapRenderChunk* chunk : chunks) {
        const int32_t col = static_cast<int32_t>((kOrigin - chunk->m_sphere.c.y) / kTileYd);
        const int32_t row = static_cast<int32_t>((kOrigin - chunk->m_sphere.c.x) / kTileYd);

        if (col < 0 || row < 0 || col > 63 || row > 63) {
            continue;
        }

        for (uint32_t a = 0; a < kAtlases; a++) {
            if (AtlasOn(a) && Has(a, static_cast<uint32_t>(row * 64 + col))) {
                byAtlas[a].push_back(chunk);
                break;
            }
        }
    }

    float start, end;
    BandRange(start, end);

    // The stock Terrain vertex shader (permutation 0: no vertex colour, which the bake already holds) over the
    // chunks' own buffers: c23 = the camera, so texcoord1 is the ground offset in yards (c14 / c19) and
    // texcoord0 the atlas uv (c13 / c18, per atlas). The projection's z is pulled 4e-7 toward the camera (about
    // a yard at the band) so the paint wins the depth test against the very triangles it covers on every backend.
    CMapRenderChunk::SVertexShaderConstants consts;
    FarField::TerrainConstants(consts);
    C44Matrix& p = consts.m_projection;
    const float eps = 4e-7f;
    p.a2 -= eps * p.a3;
    p.b2 -= eps * p.b3;
    p.c2 -= eps * p.c3;
    p.d2 -= eps * p.d3;
    consts.m_position = V4(cam.x, cam.y, 0.0f, 0.0f);
    consts.m_texOffset[1] = V4(0.0f, 0.0f, 0.0f, 0.0f);
    consts.m_texScale[2] = V4(1.0f, 1.0f, 0.0f, 0.0f);

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
    GxRsSet(GxRs_VertexShader, vertexShader);
    GxRsSet(GxRs_PixelShader, s_far.blendPs);
    GxRsSet(GxRs_Texture1, static_cast<void*>(nullptr));
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&consts), sizeof(consts) / sizeof(C4Vector));
    FarField::BindFogColor();
    const float band[4] = { start, 1.0f / fmaxf(end - start, 1.0f), 1.0f, 0.0f };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 3, band, 1);

    for (uint32_t a = 0; a < kAtlases; a++) {
        if (byAtlas[a].empty()) {
            continue;
        }

        CGxTex* tex = AtlasTex(a);
        GxTexSetWrap(tex, GxTex_Wrap, GxTex_Wrap);
        GxRsSet(GxRs_Texture0, static_cast<void*>(tex));
        const float span = static_cast<float>(Slots(a)) * kTileYd;
        const C4Vector offset = V4((kOrigin - cam.x) / span, (kOrigin - cam.y) / span, 0.0f, 0.0f);
        const C4Vector scale = V4(-1.0f / span, -1.0f / span, 0.0f, 0.0f);
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, offsetof(CMapRenderChunk::SVertexShaderConstants, m_texOffset[0]) / sizeof(C4Vector), reinterpret_cast<const float*>(&offset), 1);
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, offsetof(CMapRenderChunk::SVertexShaderConstants, m_texScale[1]) / sizeof(C4Vector), reinterpret_cast<const float*>(&scale), 1);

        for (CMapRenderChunk* chunk : byAtlas[a]) {
            chunk->RenderSetup(1);
            g_theGxDevicePtr->Draw(&chunk->m_batch, 1);
        }
    }

    g_theGxDevicePtr->RsPop();
}

// VERIFY: c20afbce manual
// OFFSET: 0x007D5E70
void CMap::RenderLowDetail(CMapAreaLow* area) {
    if (static_cast<int32_t>(area->m_indexCount) <= 0) {
        return;
    }

    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_Fog, 1);
    GxRsSet(GxRs_DepthTest, 3);

    DayNight::DNInfo* info = DayNight::GetInfo();
    CGxTex* texture = WdlTexturedCVar()->m_intValue ? FarAtlasFor(area) : nullptr;
    uint32_t tint = 0xFFFFFFFF;

    // Not in 12340 (horizonFog): with the world's fog stretched over it, the tile is drawn as one
    // more terrain chunk -- the terrain vertex shader and one-layer pixel shader with this frame's
    // terrain constants, so the same fog curve (rate included), sun and ambient -- textured with the
    // ground atlas. Its texture coordinates come from the position, as a chunk's do.
    CGxShader* vertexShader = CMap::vertexShader_Terrain[0];
    CGxShader* pixelShader = CMap::pixelShader_Terrain1[0];
    const bool shaded = texture && FarField::FogStretch() != 1.0f && CMap::enableTerrainShaderVertex && vertexShader && vertexShader->Valid() && pixelShader && pixelShader->Valid();

    if (shaded) {
        CMapRenderChunk::SVertexShaderConstants consts;
        FarField::TerrainConstants(consts);

        // The layer (c13 / c18) and alpha (c14 / c19) coordinates both span the map's atlas: stage 1
        // samples it again, whose opaque alpha reads as "no shadow" (0.7 + 0.3 a; the bake has it).
        AtlasTexConstants(consts, 2);

        GxRsSet(GxRs_VertexShader, vertexShader);
        GxRsSet(GxRs_PixelShader, pixelShader);
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&consts), sizeof(consts) / sizeof(C4Vector));
        FarField::BindFogColor();

        // Built as PN (see BuildLowDetailGridNormals): no tint can be 0, its alpha is 0xFF.
        tint = 0;
    } else if (texture) {
        // The near terrain's fog stretched over the horizon's range: the world's fog ends at or
        // before farclip, where the horizon begins, so unstretched it would hide the tile entirely.
        // Full fog at farclip x horizonFarclipScale, the horizon's far plane, so it meets the sky
        // without an edge. Then the outdoor light over the baked ground (ambient + 0.6
        // diffuse: the knob for night darkness).
        // ponytail: this fixed-function fallback multiplies the atlas, whose bake already carries the
        // vertex colours and shadow, by the tint; fine for a path no shader-capable card takes.
        float scale = World::s_horizonFarClipScale > 1.0f ? World::s_horizonFarClipScale : 1.0f;
        GxRsSet(GxRs_FogStart, info->m_fog.m_start);
        // Not stretched twice when horizonFog has stretched the world's already (no terrain shaders).
        GxRsSet(GxRs_FogEnd, FarField::FogStretch() != 1.0f ? info->m_fog.m_end : info->m_fog.m_end * scale);
        const CImVector& a = info->m_light1.m_ambient;
        const CImVector& d = info->m_light1.m_diffuse;
        uint32_t r = a.r + d.r * 3 / 5;
        uint32_t g = a.g + d.g * 3 / 5;
        uint32_t b = a.b + d.b * 3 / 5;
        CImVector c(static_cast<uint8_t>(b > 255 ? 255 : b), static_cast<uint8_t>(g > 255 ? 255 : g), static_cast<uint8_t>(r > 255 ? 255 : r), 0xFF);
        tint = *reinterpret_cast<uint32_t*>(&c);
    } else {
        // 12340: fully fogged, a fog-coloured silhouette.
        GxRsSet(GxRs_FogStart, 0.0f);
        GxRsSet(GxRs_FogEnd, 1.0f);
    }

    CImVector color = info->m_fog.m_color;
    GxRsSet(GxRs_FogColor, *reinterpret_cast<int32_t*>(&color));
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Texture0, static_cast<void*>(texture));
    GxRsSet(GxRs_Texture1, static_cast<void*>(shaded ? texture : nullptr));
    GxRsSet(GxRs_ColorOp0, 0);
    GxRsSet(GxRs_AlphaOp0, 0);
    g_theGxDevicePtr->m_xforms[GxXform_Tex0].Identity();

    if (!area->m_block) {
        CMap::s_vbbList.AcquireBlock(&area->m_block);
    }

    if (area->m_block) {
        CGxBuf* buf = area->m_block->m_buf;

        if (!buf->m_hasData || !buf->m_valid || area->m_builtTint != tint) {
            area->m_builtTint = tint;
            BuildLowDetailGrid(area, buf, shaded);
        }

        area->m_block->m_lastUsedFrame = CWorld::frameCnt;
        GxPrimVertexPtr(area->m_block->m_buf, shaded ? GxVBF_PN : GxVBF_PCT);
    } else {
        area->m_builtTint = tint;
        CGxBuf* buf = g_theGxDevicePtr->BufStream(GxPoolTarget_Vertex, sizeof(CGxVertexPCT), 545);
        BuildLowDetailGrid(area, buf, shaded);
        GxPrimVertexPtr(buf, shaded ? GxVBF_PN : GxVBF_PCT);
    }

    uint32_t count;

    if (!(area->m_flags & 0x1) && area->m_indexCount == 3072) {
        GxRsSet(GxRs_Culling, 0);
        CGxBuf* indexBuf = CMap::lowDetailIndexBuf;

        if (!indexBuf->m_hasData || !indexBuf->m_valid) {
            CMap::BuildLowDetailGridIndices(nullptr, 0, CMap::lowDetailIndexBuf);
            indexBuf = CMap::lowDetailIndexBuf;
        }

        g_theGxDevicePtr->PrimIndexPtr(indexBuf);
        count = 3072;
    } else {
        GxRsSet(GxRs_Culling, 0);
        CGxBuf* indexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, 2, area->m_indexCount);
        CMap::BuildLowDetailGridIndices(area->m_holes, 0, indexBuf);
        g_theGxDevicePtr->PrimIndexPtr(indexBuf);

        CGxBatch batch;
        batch.m_primType = GxPrim_Triangles;
        batch.m_start = 0;
        batch.m_count = area->m_indexCount;
        batch.m_minIndex = 0;
        batch.m_maxIndex = 544;
        g_theGxDevicePtr->Draw(&batch, 1);

        GxRsSet(GxRs_Culling, 1);
        indexBuf = g_theGxDevicePtr->BufStream(GxPoolTarget_Index, 2, 3072 - area->m_indexCount);
        CMap::BuildLowDetailGridIndices(area->m_holes, 1, indexBuf);
        g_theGxDevicePtr->PrimIndexPtr(indexBuf);
        count = 3072 - area->m_indexCount;
    }

    CGxBatch batch;
    batch.m_primType = GxPrim_Triangles;
    batch.m_start = 0;
    batch.m_count = count;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 544;
    g_theGxDevicePtr->Draw(&batch, 1);

    g_theGxDevicePtr->RsPop();
    g_theGxDevicePtr->m_xforms[GxXform_Tex0].Identity();
}

// VERIFY: 894a0529 manual
// OFFSET: 0x007D5150
void CMap::BuildLowDetailGridVertices(CMapAreaLow* area, CGxBuf* buf) {
    // 12340 wrote CGxVertexPC in white. PCT here: the tint of the draw (white untextured) and the
    // map's ground-atlas coordinates, u along the columns, v along the rows.
    CGxVertexPCT* vertex = reinterpret_cast<CGxVertexPCT*>(g_theGxDevicePtr->BufLock(buf));
    const CImVector tint = *reinterpret_cast<const CImVector*>(&area->m_builtTint);
    uint32_t index = 0;
    float x = area->m_topLeft.x;

    for (int32_t row = 0; row < 17; row++) {
        float y = area->m_topLeft.y;

        for (int32_t col = 0; col < 17; col++) {
            vertex->p.x = x;
            vertex->p.y = y;
            vertex->p.z = area->m_heights[index++];
            vertex->c = tint;
            vertex->tc = C2Vector((area->m_index.x + col * 0.0625f) / 64.0f, (area->m_index.y + row * 0.0625f) / 64.0f);
            vertex++;
            y -= 33.333332f;
        }

        x -= 33.333332f;
    }

    x = area->m_topLeft.x - 16.666666f;

    for (int32_t row = 0; row < 16; row++) {
        float y = area->m_topLeft.y - 16.666666f;

        for (int32_t col = 0; col < 16; col++) {
            vertex->p.x = x;
            vertex->p.y = y;
            vertex->p.z = area->m_heights[index++];
            vertex->c = tint;
            vertex->tc = C2Vector((area->m_index.x + (col + 0.5f) * 0.0625f) / 64.0f, (area->m_index.y + (row + 0.5f) * 0.0625f) / 64.0f);
            vertex++;
            y -= 33.333332f;
        }

        x -= 33.333332f;
    }

    g_theGxDevicePtr->BufUnlock(buf, 0);
    buf->m_hasData = 1;
}

// VERIFY: 7d2e0b07 manual
// OFFSET: 0x007D5240
void CMap::BuildLowDetailGridIndices(uint16_t* holes, int32_t pass, CGxBuf* buf) {
    WriteGridIndices(reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(buf)), holes, pass, 0);
    g_theGxDevicePtr->BufUnlock(buf, 0);
    buf->m_hasData = 1;
}

// VERIFY: f803b515 manual
// OFFSET: 0x007CC770
void CMapLowDetail::Unload() {
    FarField::Reset();

    for (uint32_t i = 0; i < 4096; i++) {
        if (this->m_areas[i]) {
            CMap::FreeAreaLow(this->m_areas[i]);
            this->m_areas[i] = nullptr;
        }
    }

    for (uint32_t i = 0; i < this->m_mapObjDefList.m_count; i++) {
        this->m_mapObjDefList.m_data[i]->m_refCount--;
        CMap::PurgeMapObjDef(this->m_mapObjDefList.m_data[i]);
    }

    this->m_mapObjDefList.m_count = 0;
    this->m_mapObjNames = nullptr;
    this->m_mapObjNameOffsets = nullptr;
    this->m_mapObjDefs = nullptr;
    this->m_mapObjDefCount = 0;
    this->m_areaOffsets = nullptr;

    if (this->m_data) {
        SMemFree(this->m_data, nullptr, 0, 0);
        this->m_data = nullptr;
    }
}
