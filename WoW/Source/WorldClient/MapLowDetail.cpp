#include "MapLowDetail.h"
#include "FarTerrainLod.h"
#include <cmath>
#include <cctype>
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

// Not in 12340 (wdlTextured): the WDL horizon's ground colour and its batched draw. docs/FARFIELD_FOG.md.
//
// The colour is the map's own ground, baked in the client the way wxl-wraithhost bakes its far-land
// colour (FarLod.cpp, the 8.3 yd colour wxl-farfield puts on the coarse WDL): each ADT chunk's layers, one
// average colour per layer texture, composited by their alpha maps as the terrain shader composites them,
// times the vertex colours (MCCV) and the baked shadow, water cells in their liquid's colour. The ADT is
// read through the async file queue, nearest tile first, and one tile a frame is baked into a 4096 px DXT1
// atlas of the whole 64 x 64 map (64 px a tile, so a texel is 8.3 yd). The atlas is laid out like the
// world, so its uv is a linear function of the position for every tile: one set of shader constants and
// one texture bind for the whole horizon, and a mip or a bilinear tap that crosses a tile edge reaches the
// tile that really is next door. A tile not baked yet is drawn fully fogged, as 12340 drew every tile.
//
// The geometry is the WDL heights in static buffers, one vertex and one index buffer per 8 x 8 tiles,
// built when first seen (the heights never change). A frame draws each block's visible tiles as merged
// index runs, blocks front to back, state set once.
namespace {

constexpr float kOrigin = 17066.666f;
constexpr float kTileYd = 533.33331f;
constexpr uint32_t kTilePx = 64;
constexpr uint32_t kAtlasPx = 64 * kTilePx;
constexpr uint32_t kLevels = 11;          // 4096 .. 4 px: the DXT chain CGxDevice::ITexWHDStartEnd asks for
constexpr uint32_t kTileVerts = 545;
constexpr uint32_t kTileIndices = 3072;
constexpr uint32_t kReadsInFlight = 2;
// wxl-farfield's match_chroma default: an average over 8.3 yd is greyer than the ground it averages
// (measured 0.27 against 0.39 saturation), so the spread about its own brightness is opened back up.
// ponytail: fixed; farfield also closes a per-channel gain measured live against the near ground on the
// GPU, which this skips.
constexpr float kChroma = 1.4f;
// wraithhost's liquid colours (water, ocean, magma, slime).
constexpr float kLiquid[4][3] = { { 34, 62, 70 }, { 22, 48, 66 }, { 235, 95, 25 }, { 90, 140, 40 } };

enum : uint8_t { Tile_None, Tile_Reading, Tile_Baked, Tile_Failed };

struct FarRead {
    CAsyncObject* obj;
    uint8_t* buf;
    uint32_t size;
    uint32_t tile;
    uint32_t gen;
    bool bake;     // for the ground bake
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

struct FarState {
    uint32_t gen = 1;
    uint8_t tile[4096] = {};
    uint8_t avg[4096][3] = {};
    uint32_t start[4096][2] = {};          // per tile: index start / count of its culled-off and culled-on cells
    uint32_t count[4096][2] = {};
    uint8_t* chain = nullptr;
    uint32_t levelOffset[kLevels] = {};
    CTexture* atlas = nullptr;
    FarBlock blocks[64] = {};
    std::vector<FarRead*> ready;
    uint32_t inFlight = 0;
    std::unordered_map<std::string, uint32_t> colours;
};

FarState s_far;

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

// A ground texture's average colour (0xRRGGBB): its smallest mip that is still a DXT block across, read
// alone (wraithhost AverageColour). Mid grey when unreadable.
uint32_t TextureColour(const char* name) {
    std::string key(name);

    for (char& c : key) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    auto it = s_far.colours.find(key);

    if (it != s_far.colours.end()) {
        return it->second;
    }

    uint32_t rgb = 0x808080;
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

            const uint32_t bytes = dxt1 ? 8 : 16;
            const uint32_t blocks = ((w >> level) / 4 ? (w >> level) / 4 : 1) * ((ht >> level) / 4 ? (ht >> level) / 4 : 1);
            const uint32_t len = blocks * bytes;

            if (w && ht && w <= 4096 && ht <= 4096 && Rd32(h + 84 + level * 4) >= len) {
                std::vector<uint8_t> data(len);
                SFile::SetFilePointer(file, static_cast<int32_t>(Rd32(h + 20 + level * 4)), nullptr, 0);

                if (SFileReadFile(file, data.data(), len, &got, nullptr, 0) && got == len) {
                    double acc[3] = {};

                    for (uint32_t k = 0; k < blocks; k++) {
                        const uint8_t* blk = data.data() + k * bytes + (bytes - 8);
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
                                acc[j] += c[(bits >> (2 * p)) & 3][j];
                            }
                        }
                    }

                    rgb = 0;

                    for (int j = 0; j < 3; j++) {
                        rgb |= static_cast<uint32_t>(acc[j] / (blocks * 16.0)) << (16 - 8 * j);
                    }
                }
            }
        }

        SFileCloseFile(file);
    }

    s_far.colours.emplace(key, rgb);
    return rgb;
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

// The tile's ground as 64 x 64 RGB (row = the tile's rows, as the atlas). False when it is not an ADT.
bool BakeTile(const uint8_t* b, uint32_t n, float* img) {
    const uint32_t kMTEX = 0x4D544558;
    const uint32_t kMH2O = 0x4D48324F;
    const uint32_t kMCNK = 0x4D434E4B;
    const uint32_t kMCLY = 0x4D434C59;
    const uint32_t kMCAL = 0x4D43414C;
    const uint32_t kMCSH = 0x4D435348;
    const uint32_t kMCCV = 0x4D434356;
    const bool bigAlpha = (CMap::header.m_flags & 0x4) != 0;

    std::vector<const char*> textures;
    const uint8_t* mh2o = nullptr;
    uint32_t mh2oLen = 0;
    const uint8_t* chunks[256] = {};
    uint32_t chunkLen[256] = {};
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

                textures.push_back(name);
                i += len + 1;
            }
        } else if (magic == kMH2O) {
            mh2o = b + o + 8;
            mh2oLen = size;
        } else if (magic == kMCNK && size >= 0x80) {
            uint32_t ix = Rd32(b + o + 8 + 4);
            uint32_t iy = Rd32(b + o + 8 + 8);

            if (ix > 15 || iy > 15) {
                ix = seen % 16;
                iy = seen / 16;
            }

            if (seen < 256) {
                chunks[iy * 16 + ix] = b + o;
                chunkLen[iy * 16 + ix] = size + 8;
            }

            seen++;
        }

        o += 8 + size;
    }

    if (seen < 256) {
        return false;
    }

    uint8_t alpha[3][4096];

    for (uint32_t ch = 0; ch < 256; ch++) {
        const uint8_t* c = chunks[ch];

        if (!c) {
            return false;
        }

        const uint8_t* h = c + 8;
        const uint32_t len = chunkLen[ch];
        const uint32_t flags = Rd32(h);

        // Liquid per cell (8 x 8): its LiquidType's kind + 1, 0 = dry. MH2O, or the old per-chunk flags.
        uint8_t wet[64] = {};

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

        // Sized by the MCNK's sizeAlpha, as CMapChunk does; the MCAL header lies on Northrend.
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
        float col[4][3] = {};
        bool hasAlpha[4] = {};

        for (uint32_t l = 0; l < layers; l++) {
            const uint8_t* e = mcly + l * 16;
            const uint32_t tex = Rd32(e);
            const uint32_t rgb = tex < textures.size() ? TextureColour(textures[tex]) : 0x808080;

            for (int j = 0; j < 3; j++) {
                col[l][j] = static_cast<float>((rgb >> (16 - 8 * j)) & 0xFF);
            }

            if (l && (Rd32(e + 4) & 0x100)) {
                DecodeAlpha(mcal, mcalLen, Rd32(e + 8), Rd32(e + 4), bigAlpha, (flags & 0x8000) != 0, alpha[l - 1]);
                hasAlpha[l] = true;
            }
        }

        const uint32_t ix = ch % 16;
        const uint32_t iy = ch / 16;

        // 4 x 4 texels a chunk, each over a 16 x 16 patch of the alpha maps.
        for (uint32_t py = 0; py < 4; py++) {
            for (uint32_t px = 0; px < 4; px++) {
                float* out = img + ((iy * 4 + py) * 64 + ix * 4 + px) * 3;
                const uint8_t* cells[4] = { &wet[(py * 2) * 8 + px * 2], &wet[(py * 2) * 8 + px * 2 + 1], &wet[(py * 2 + 1) * 8 + px * 2], &wet[(py * 2 + 1) * 8 + px * 2 + 1] };
                uint32_t wetCount = 0;
                uint8_t kind = 0;

                for (const uint8_t* cell : cells) {
                    if (*cell) {
                        wetCount++;
                        kind = *cell;
                    }
                }

                if (wetCount >= 2) {
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

                    for (uint32_t yy = 0; yy < 16; yy++) {
                        for (uint32_t xx = 0; xx < 16; xx++) {
                            sum += alpha[l - 1][(py * 16 + yy) * 64 + px * 16 + xx];
                        }
                    }

                    a[l] = static_cast<float>(sum) / (256.0f * 255.0f);
                }

                // As the terrain pixel shader blends: big alpha (perm 14) weighs every layer, else each
                // layer is laid over the ones below (perm 6).
                if (!layers) {
                    out[0] = out[1] = out[2] = 128.0f;
                } else if (bigAlpha) {
                    float base = 1.0f;

                    for (uint32_t l = 1; l < layers; l++) {
                        base -= a[l];
                    }

                    for (int j = 0; j < 3; j++) {
                        out[j] = col[0][j] * (base > 0.0f ? base : 0.0f);

                        for (uint32_t l = 1; l < layers; l++) {
                            out[j] += col[l][j] * a[l];
                        }
                    }
                } else {
                    for (int j = 0; j < 3; j++) {
                        out[j] = col[0][j];

                        for (uint32_t l = 1; l < layers; l++) {
                            out[j] += (col[l][j] - out[j]) * a[l];
                        }
                    }
                }

                // Vertex colours (0x7F = unchanged), bilinear at the texel's centre on the 9 x 9 grid.
                if (mccv) {
                    const float gx = (px + 0.5f) * 2.0f;
                    const float gy = (py + 0.5f) * 2.0f;
                    const uint32_t c0 = static_cast<uint32_t>(gx);
                    const uint32_t r0 = static_cast<uint32_t>(gy);
                    const float fx = gx - c0;
                    const float fy = gy - r0;

                    for (int j = 0; j < 3; j++) {
                        auto v = [&](uint32_t r, uint32_t k) { return static_cast<float>(mccv[(r * 17 + k) * 4 + (2 - j)]); };
                        const float m = (v(r0, c0) * (1 - fx) + v(r0, c0 + 1) * fx) * (1 - fy) + (v(r0 + 1, c0) * (1 - fx) + v(r0 + 1, c0 + 1) * fx) * fy;
                        out[j] *= m * 2.0f / 255.0f;
                    }
                }

                // The baked shadow: the shader's 0.7 + 0.3 * alpha, over the patch.
                if (mcsh) {
                    uint32_t on = 0;

                    for (uint32_t y = 0; y < 16; y++) {
                        for (uint32_t x = 0; x < 16; x++) {
                            on += (mcsh[(py * 16 + y) * 8 + (px * 16 + x) / 8] >> ((px * 16 + x) % 8)) & 1;
                        }
                    }

                    for (int j = 0; j < 3; j++) {
                        out[j] *= 1.0f - 0.3f * static_cast<float>(on) / 256.0f;
                    }
                }

                const float luma = 0.299f * out[0] + 0.587f * out[1] + 0.114f * out[2];

                for (int j = 0; j < 3; j++) {
                    const float v = luma + (out[j] - luma) * kChroma;
                    out[j] = v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v;
                }
            }
        }
    }

    return true;
}

uint16_t To565(const float* c) {
    const uint32_t r = static_cast<uint32_t>(c[0] * 31.0f / 255.0f + 0.5f);
    const uint32_t g = static_cast<uint32_t>(c[1] * 63.0f / 255.0f + 0.5f);
    const uint32_t b = static_cast<uint32_t>(c[2] * 31.0f / 255.0f + 0.5f);
    return static_cast<uint16_t>((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
}

// 16 RGB texels as one opaque DXT1 block: the box's corners as the ends, four-colour mode.
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
        float p[4][3];
        From565(c0, p[0]);
        From565(c1, p[1]);

        for (int j = 0; j < 3; j++) {
            p[2][j] = (2 * p[0][j] + p[1][j]) / 3;
            p[3][j] = (p[0][j] + 2 * p[1][j]) / 3;
        }

        for (uint32_t k = 0; k < 16; k++) {
            uint32_t best = 0;
            float bestD = 1e30f;

            for (uint32_t i = 0; i < 4; i++) {
                const float dr = px[k][0] - p[i][0], dg = px[k][1] - p[i][1], db = px[k][2] - p[i][2];
                const float d = dr * dr + dg * dg + db * db;

                if (d < bestD) {
                    bestD = d;
                    best = i;
                }
            }

            bits |= best << (2 * k);
        }
    }

    out[0] = static_cast<uint8_t>(c0);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1);
    out[3] = static_cast<uint8_t>(c1 >> 8);
    memcpy(out + 4, &bits, 4);
}

uint8_t* AtlasBlock(uint32_t level, uint32_t bx, uint32_t by) {
    return s_far.chain + s_far.levelOffset[level] + (by * ((kAtlasPx >> level) / 4) + bx) * 8;
}

void AtlasUserFunc(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    if (command == GxTex_Latch && s_far.chain && mipLevel < kLevels) {
        texelStrideInBytes = (kAtlasPx >> mipLevel) / 4 * 8;
        texels = s_far.chain + s_far.levelOffset[mipLevel];
    }
}

// The baked tile into the atlas: levels 0-4 (64 .. 4 px a tile) box-filtered from it, and the block that
// holds it on each smaller level, where a texel spans tiles, from the baked tiles' averages.
void StoreTile(uint32_t t, const float* img) {
    // A new atlas goes up whole the first time, so no texel on the GPU is left uninitialised.
    const bool fresh = !s_far.chain;

    if (fresh) {
        uint32_t size = 0;

        for (uint32_t l = 0; l < kLevels; l++) {
            s_far.levelOffset[l] = size;
            size += (kAtlasPx >> l) * (kAtlasPx >> l) / 2;
        }

        s_far.chain = new uint8_t[size]();
        s_far.atlas = TextureCreate(kAtlasPx, kAtlasPx, GxTex_Dxt1, GxTex_Dxt1, CGxTexFlags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1), nullptr, AtlasUserFunc, "WdlGroundAtlas", 0);
    }

    const uint32_t tx = t % 64;
    const uint32_t ty = t / 64;
    float sum[3] = {};

    for (uint32_t i = 0; i < 64 * 64; i++) {
        for (int j = 0; j < 3; j++) {
            sum[j] += img[i * 3 + j];
        }
    }

    for (int j = 0; j < 3; j++) {
        s_far.avg[t][j] = static_cast<uint8_t>(sum[j] / 4096.0f);
    }

    s_far.tile[t] = Tile_Baked;

    for (uint32_t l = 0; l < 5; l++) {
        const uint32_t f = 1u << l;
        const uint32_t bw = (kTilePx >> l) / 4;

        for (uint32_t by = 0; by < bw; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                float px[16][3] = {};

                for (uint32_t k = 0; k < 16; k++) {
                    const uint32_t x0 = (bx * 4 + k % 4) * f;
                    const uint32_t y0 = (by * 4 + k / 4) * f;

                    for (uint32_t yy = 0; yy < f; yy++) {
                        for (uint32_t xx = 0; xx < f; xx++) {
                            for (int j = 0; j < 3; j++) {
                                px[k][j] += img[((y0 + yy) * 64 + x0 + xx) * 3 + j];
                            }
                        }
                    }

                    for (int j = 0; j < 3; j++) {
                        px[k][j] /= static_cast<float>(f * f);
                    }
                }

                EncodeDxt1(px, AtlasBlock(l, tx * bw + bx, ty * bw + by));
            }
        }
    }

    for (uint32_t l = 5; l < kLevels; l++) {
        const uint32_t bx = ((tx * kTilePx) >> l) / 4;
        const uint32_t by = ((ty * kTilePx) >> l) / 4;
        float px[16][3] = {};

        for (uint32_t k = 0; k < 16; k++) {
            const uint32_t p = bx * 4 + k % 4;
            const uint32_t q = by * 4 + k / 4;
            const uint32_t c0 = (p << l) / kTilePx, c1 = (((p + 1) << l) - 1) / kTilePx;
            const uint32_t r0 = (q << l) / kTilePx, r1 = (((q + 1) << l) - 1) / kTilePx;
            float acc[3] = {};
            uint32_t got = 0;

            for (uint32_t r = r0; r <= r1; r++) {
                for (uint32_t c = c0; c <= c1; c++) {
                    if (s_far.tile[r * 64 + c] == Tile_Baked) {
                        for (int j = 0; j < 3; j++) {
                            acc[j] += s_far.avg[r * 64 + c][j];
                        }

                        got++;
                    }
                }
            }

            for (int j = 0; j < 3; j++) {
                px[k][j] = got ? acc[j] / got : 0.0f;
            }
        }

        EncodeDxt1(px, AtlasBlock(l, bx, by));
    }

    if (CGxTex* gxTex = TextureGetGxTex(s_far.atlas, 0, nullptr)) {
        if (fresh) {
            GxTexUpdate(gxTex, 0, 0, kAtlasPx, kAtlasPx, 1);
        } else {
            GxTexUpdate(gxTex, tx * kTilePx, ty * kTilePx, (tx + 1) * kTilePx, (ty + 1) * kTilePx, 1);
        }
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

// Nearest tile first, within the horizon's reach: a couple of ADT reads in flight on the async queue, and
// one finished read baked a frame.
void PumpBake() {
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float cc = (kOrigin - cam.y) / kTileYd;
    const float cr = (kOrigin - cam.x) / kTileYd;
    const float reach = World::s_horizonFarClipScale * World::s_farClip / kTileYd + 1.5f;

    while (s_far.inFlight < kReadsInFlight) {
        int32_t best = -1;
        float bestD = reach * reach;

        for (uint32_t i = 0; i < 4096; i++) {
            if ((s_far.tile[i] != Tile_None && !FarLod::WantsAdt(i)) || !CMap::s_lowDetail.m_areas[i]) {
                continue;
            }

            const float dx = static_cast<float>(i % 64) + 0.5f - cc;
            const float dy = static_cast<float>(i / 64) + 0.5f - cr;
            const float d = dx * dx + dy * dy;

            if (d < bestD) {
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

        const bool bake = s_far.tile[best] == Tile_None;
        const bool heights = FarLod::WantsAdt(best);

        if (heights) {
            FarLod::MarkReading(best);
        }

        if (!SFileOpenFile(name, &file) || !file) {
            if (bake) {
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
        if (bake) {
            s_far.tile[best] = Tile_Reading;
        }

        s_far.inFlight++;
        AsyncFileReadObject(read->obj, 0);
    }

    // ponytail: one tile a frame (about 2-3 ms the first time its textures are averaged, under 1 ms after),
    // not a timed budget.
    if (!s_far.ready.empty()) {
        FarRead* read = s_far.ready.front();
        s_far.ready.erase(s_far.ready.begin());
        static float s_img[64 * 64 * 3];

        if (!read->bake) {
        } else if (BakeTile(read->buf, read->size, s_img)) {
            StoreTile(read->tile, s_img);
        } else {
            s_far.tile[read->tile] = Tile_Failed;
        }

        if (read->heights) {
            FarLod::AdtRead(read->tile, read->buf, read->size);
        }

        delete[] read->buf;
        delete read;
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

// The atlas, when this tile's bake is in it.
CGxTex* FarAtlasFor(const CMapAreaLow* area) {
    const uint32_t t = static_cast<uint32_t>(area->m_index.y) * 64 + area->m_index.x;
    return s_far.atlas && s_far.tile[t] == Tile_Baked ? TextureGetGxTex(s_far.atlas, 0, nullptr) : nullptr;
}

// The terrain shader's uv for the atlas: uv = (pos.yx - c23.yx) * scale.yx, the map's corner at 0.
void AtlasTexConstants(CMapRenderChunk::SVertexShaderConstants& consts) {
    const float scale = -1.0f / (64.0f * kTileYd);
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

}  // namespace

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

    if (s_far.atlas) {
        HandleClose(reinterpret_cast<HOBJECT>(s_far.atlas));
        s_far.atlas = nullptr;
    }

    delete[] s_far.chain;
    s_far.chain = nullptr;
    memset(s_far.tile, 0, sizeof(s_far.tile));
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

    // The visible tiles per block, baked (textured) and not (fog-coloured). Tiles wholly nearer than the
    // horizon's near plane (farZ - 50) are skipped: every point of them is clipped.
    const C3Vector& cam = CWorldScene::s_activeWorldView;
    const float nearZ = CGWorldFrame::GetActiveCamera()->m_farZ - 50.0f;
    uint64_t textured[64] = {};
    uint64_t fogged[64] = {};
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
        const uint32_t b = (row / 8) * 8 + col / 8;
        const uint64_t bit = 1ull << ((row % 8) * 8 + col % 8);
        const bool baked = s_far.tile[row * 64 + col] == Tile_Baked && s_far.atlas;

        if (!FarLod::Claim(row * 64 + col, baked)) {  // a detailed level draws it instead (FarTerrainLod.cpp)
            (baked ? textured : fogged)[b] |= bit;
        }
    }

    // Blocks front to back.
    uint32_t order[64];
    float dist[64];
    uint32_t count = 0;

    for (uint32_t b = 0; b < 64; b++) {
        if (!(textured[b] | fogged[b]) || !BuildBlock(b)) {
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
    AtlasTexConstants(consts);
    GxRsSet(GxRs_VertexShader, vertexShader);
    GxRsSet(GxRs_PixelShader, pixelShader);
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&consts), sizeof(consts) / sizeof(C4Vector));
    FarField::BindFogColor();

    CImVector color = DayNight::GetInfo()->m_fog.m_color;
    CGxTex* atlas = s_far.atlas ? TextureGetGxTex(s_far.atlas, 0, nullptr) : nullptr;
    GxRsSet(GxRs_FogColor, *reinterpret_cast<int32_t*>(&color));
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Texture0, static_cast<void*>(atlas));
    GxRsSet(GxRs_Texture1, static_cast<void*>(atlas));
    GxRsSet(GxRs_ColorOp0, 0);
    GxRsSet(GxRs_AlphaOp0, 0);
    g_theGxDevicePtr->m_xforms[GxXform_Tex0].Identity();

    // Pass 0 textured; pass 1 the tiles not baked yet, fully fogged: fog = pow(max(viewZ * 0 + 0, 0), 1) = 0,
    // so the shader outputs exactly the fog colour (12340's silhouette).
    for (uint32_t set = 0; set < 2; set++) {
        const uint64_t* masks = set ? fogged : textured;

        if (set) {
            const float fogged4[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, offsetof(CMapRenderChunk::SVertexShaderConstants, m_fog) / sizeof(C4Vector), fogged4, 1);
        }

        for (uint32_t i = 0; i < count; i++) {
            const uint32_t b = order[i];

            if (!masks[b]) {
                continue;
            }

            GxPrimVertexPtr(s_far.blocks[b].vertexBuf, GxVBF_PN);
            g_theGxDevicePtr->PrimIndexPtr(s_far.blocks[b].indexBuf);
            GxRsSet(GxRs_Culling, 0);
            DrawRuns(b, masks[b], 0);
            GxRsSet(GxRs_Culling, 1);
            DrawRuns(b, masks[b], 1);
        }

        FarLod::Draw(set);
    }

    g_theGxDevicePtr->RsPop();
    FarLod::EndFrame();
    g_theGxDevicePtr->m_xforms[GxXform_Tex0].Identity();
    return true;
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
        AtlasTexConstants(consts);

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
