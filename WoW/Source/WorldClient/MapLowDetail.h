#ifndef WOW_SOURCE_WORLDCLIENT_MAPLOWDETAIL_H
#define WOW_SOURCE_WORLDCLIENT_MAPLOWDETAIL_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/CAaSphere.h"
#include "WoW/Source/WorldClient/FVBBList.h"
#include "WoW/Source/WorldClient/MapRenderChunk.h"

class CMapObjDef;
class CTexture;
struct SMMapObjDef;

class CMapAreaLow {
    public:
    /* 0000 */ uint32_t m_objectIndex;
    /* 0004 */ CAaBox m_bbox;
    /* 001C */ CAaSphere m_sphere;
    /* 002C */ C3Vector m_topLeft;
    /* 0038 */ C2iVector m_index;
    /* 0040 */ uint32_t m_indexCount;
    /* 0044 */ int16_t* m_heights;
    /* 0048 */ uint16_t* m_holes;
    /* 004C */ TSLink<CMapAreaLow> m_horizonLink;
    /* 0054 */ FVBBList::Block* m_block;
    /* 0058 */ uint32_t m_flags;
    // Not in 12340 (wdlTextured): the tint its vertices were built with (the per-tile fallback path).
    uint32_t m_builtTint;

    CMapAreaLow();
};

class CMapAreaLowHeap {
    public:
    /* 0000 */ uint32_t m_heapId;

    void Delete(CMapAreaLow* areaLow);
};

// Not in 12340 (horizonFog): the far fog and light every far pass shares (the WDL horizon now;
// far tree/grass billboards and WMOs on the WDL later). docs/FARFIELD_FOG.md.
namespace FarField {
    // The factor DayNight::UpdateFog stretches the world's fog end by; 1 when wdlTextured or
    // horizonFog is off (stock fog).
    float FogStretch();
    // This frame's terrain vertex constants for a far draw (fog c12, sun c24..c26), with the
    // projection bound now and no point lights. The caller sets m_position / m_texOffset /
    // m_texScale for its geometry, or reads m_fog / m_sun* into its own shader's constants.
    void TerrainConstants(CMapRenderChunk::SVertexShaderConstants& consts);
    // The fog colour where the terrain pixel shaders read it (pixel c2, or GxRs_FogColor).
    void BindFogColor();
    // The WDL terrain pass (CWorldScene::RenderHorizon): streams the ground bake and draws every horizon
    // tile from static per-block buffers in a few merged draws. False when the per-tile path must draw
    // instead (wdlTextured 0, horizonBatched 0, stock fog, no terrain shaders); the list is then untouched.
    bool RenderTerrain();
    // Drops the map's far-terrain buffers, bake and atlas (map load / unload).
    void Reset();
    // A 4 x 4 block of rgb (0..255) as DXT1, always 4-colour mode, so it also serves as a DXT5 colour block.
    void EncodeDxt1Block(const float (*px)[3], uint8_t* out);

    // Not in 12340 (farTrees, farGrass): far trees and bushes (FarFoliage.cpp). docs/FARFIELD_FOG.md.
    // The tile's tree and bush placements (MDDF), from the ADT the ground bake has just read.
    void FoliageScanTile(uint32_t tile, const uint8_t* adt, uint32_t size);
    // The cards: horizon false in the main scene (before RenderHorizon, normal projection, up to farclip),
    // true inside RenderHorizon after the WDL (horizon projection and depth band, past farZ - 50).
    void RenderFoliage(bool horizon);
    void FoliageReset();
    // farGrass: cut ground-effect grass by its size on screen; the distance where a tuft `height` yd tall
    // falls under farGrassMinPixels with the projection and viewport bound now.
    bool GrassSizeCull();
    float GrassCutDistance(float height);
    // The blend band (farTexBand): CWorldScene::RenderChunks notes every real chunk it draws, then
    // RenderBlend paints the far ground's colour over those reaching the last farTexBandWidth yards before
    // the view distance, fading in to all of it at the cut-off, so the seam with the far land shows no line.
    void NoteChunk(CMapRenderChunk* chunk);
    void RenderBlend();
}

class CMapLowDetail {
    public:
    /* 0000 */ char* m_data;
    /* 0004 */ char* m_mapObjNames;
    /* 0008 */ uint32_t* m_mapObjNameOffsets;
    /* 000C */ SMMapObjDef* m_mapObjDefs;
    /* 0010 */ uint32_t m_mapObjDefCount;
    /* 0014 */ uint32_t* m_areaOffsets;
    /* 0018 */ CMapAreaLow* m_areas [4096];
    /* 4018 */ TSGrowableArray<CMapObjDef*> m_mapObjDefList;

    CMapLowDetail();
    void CullHorizonAreas(const C3Vector* pos);
    void Load(const char* mapPath, const char* mapName);
    void Purge();
    void Unload();
};

#endif
