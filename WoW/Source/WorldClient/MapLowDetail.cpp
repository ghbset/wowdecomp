#include "MapLowDetail.h"
#include <cmath>
#include <cstring>
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
#include "WoW/Source/Ui/MinimapFrame.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapRenderChunk.h"

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
    this->m_minimapTex = nullptr;
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

// Not in 12340 (wdlTextured): the tile's minimap texture, created on first use (asynchronously, a
// few per frame) and freed with the tile (CMapAreaLowHeap::Delete); null until it has loaded or
// when the map has no minimap tile there.
static CGxTex* WdlMinimapTexture(CMapAreaLow* area) {
    if (!area->m_minimapTex) {
        static uint32_t s_frame;
        static uint32_t s_requests;

        if (s_frame != CWorld::frameCnt) {
            s_frame = CWorld::frameCnt;
            s_requests = 0;
        }

        char path[260];

        // ponytail: 8 requests a frame, so a teleport does not queue the whole horizon at once.
        if (s_requests >= 8 || !MinimapTileTexturePath(CMap::mapName, area->m_index.x, area->m_index.y, path, sizeof(path))) {
            return nullptr;
        }

        s_requests++;
        CStatus status;
        area->m_minimapTex = TextureCreate(path, CGxTexFlags(GxTex_LinearMipLinear, 0, 0, 0, 0, 0, 1), &status, 1);

        if (!area->m_minimapTex) {
            return nullptr;
        }
    }

    return TextureGetGxTex(area->m_minimapTex, 0, nullptr);
}

// Not in 12340 (horizonFog): the grid as PN for the terrain vertex shader (the same 24-byte stride
// as PCT, so the same VBB blocks). Normals from the WDL heights: central differences on the 17x17
// outer grid (one-sided at the tile's edge), an inner vertex from its cell's four corners. x falls
// along the rows and y along the columns, 33.33 yd apart.
static void BuildLowDetailGridNormals(CMapAreaLow* area, CGxBuf* buf) {
    CGxVertexPN* vertex = reinterpret_cast<CGxVertexPN*>(g_theGxDevicePtr->BufLock(buf));
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

    g_theGxDevicePtr->BufUnlock(buf, 0);
    buf->m_hasData = 1;
}

static void BuildLowDetailGrid(CMapAreaLow* area, CGxBuf* buf, bool normals) {
    if (normals) {
        BuildLowDetailGridNormals(area, buf);
    } else {
        CMap::BuildLowDetailGridVertices(area, buf);
    }
}

// Not in 12340 (wdlTextured): texture the WDL horizon with the minimap tiles.
static CVar* WdlTexturedCVar() {
    static CVar* s_cvar = CVar::Register("wdlTextured", "Engine option: texture the WDL horizon terrain with the map's minimap tiles (0: the stock fog-coloured silhouette)", 0x1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
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
    CGxTex* texture = WdlTexturedCVar()->m_intValue ? WdlMinimapTexture(area) : nullptr;
    uint32_t tint = 0xFFFFFFFF;

    // Not in 12340 (horizonFog): with the world's fog stretched over it, the tile is drawn as one
    // more terrain chunk -- the terrain vertex shader and one-layer pixel shader with this frame's
    // terrain constants, so the same fog curve (rate included), sun and ambient -- textured with the
    // minimap tile. Its texture coordinates come from the position, as a chunk's do.
    CGxShader* vertexShader = CMap::vertexShader_Terrain[0];
    CGxShader* pixelShader = CMap::pixelShader_Terrain1[0];
    const bool shaded = texture && FarField::FogStretch() != 1.0f && CMap::enableTerrainShaderVertex && vertexShader && vertexShader->Valid() && pixelShader && pixelShader->Valid();

    if (shaded) {
        CMapRenderChunk::SVertexShaderConstants consts;
        FarField::TerrainConstants(consts);

        // The layer (c13 / c18) and alpha (c14 / c19) coordinates both span the tile: stage 1
        // samples the minimap again, whose opaque alpha reads as "no shadow" (0.7 + 0.3 a).
        const float scale = -1.0f / (16.0f * 33.333332f);
        consts.m_position.x = area->m_topLeft.x;
        consts.m_position.y = area->m_topLeft.y;
        consts.m_position.z = 0.0f;
        consts.m_position.w = 0.0f;

        for (int32_t i = 0; i < 2; i++) {
            consts.m_texOffset[i].x = consts.m_texOffset[i].y = consts.m_texOffset[i].z = consts.m_texOffset[i].w = 0.0f;
            consts.m_texScale[i + 1].x = consts.m_texScale[i + 1].y = scale;
            consts.m_texScale[i + 1].z = consts.m_texScale[i + 1].w = 0.0f;
        }

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
        // without an edge. Then the outdoor light over the minimap's baked daylight (ambient + 0.6
        // diffuse: the knob for night darkness).
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
    // tile's minimap coordinates, u along the columns (the minimap's x), v along the rows.
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
            vertex->tc = C2Vector(col * 0.0625f, row * 0.0625f);
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
            vertex->tc = C2Vector((col + 0.5f) * 0.0625f, (row + 0.5f) * 0.0625f);
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
    uint16_t* indices = reinterpret_cast<uint16_t*>(g_theGxDevicePtr->BufLock(buf));
    uint32_t outer = 0;
    uint32_t inner = 289;

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

    g_theGxDevicePtr->BufUnlock(buf, 0);
    buf->m_hasData = 1;
}

// VERIFY: f803b515 manual
// OFFSET: 0x007CC770
void CMapLowDetail::Unload() {
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
