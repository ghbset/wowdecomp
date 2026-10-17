#ifndef WOW_SOURCE_WORLDCLIENT_FARTERRAINLOD_H
#define WOW_SOURCE_WORLDCLIENT_FARTERRAINLOD_H

#include <cstdint>

// Not in 12340 (farTerrainLod): the far terrain's detailed levels, from the real ADT heights. Near the
// view distance a horizon tile is drawn from the ADT's own MCVT/MCNR at full, half or quarter resolution
// instead of the 33 yd WDL grid, the level picked by its geometric error projected to pixels. Port of
// wxl-farfield's detailed level (FarTerrain.cpp, "<map>.farlod" baked by wxl-wraithhost), built here in
// the client from the ADTs the WDL ground bake already reads. docs/FARFIELD_FOG.md.
namespace FarLod {
    // The ground bake's pump: true when tile t's ADT should be read for its heights now.
    bool WantsAdt(uint32_t t);
    void MarkReading(uint32_t t);
    // A finished read of tile t's ADT (null: it could not be opened or parsed).
    void AdtRead(uint32_t t, const uint8_t* buf, uint32_t size);
    // RenderTerrain, before its tile loop: picks every loaded tile's level by distance and queues the missing.
    void BeginFrame();
    // Tile t is visible this frame: true when a detailed level draws it (the WDL pass then leaves it out).
    // `set`: the WDL pass's draw set it belongs to (0-2 the atlas it is textured from, 3 fog-coloured).
    // The level itself was picked in BeginFrame by distance alone.
    bool Claim(uint32_t t, uint32_t set);
    bool Drawing(uint32_t set);
    // Draws the claimed tiles of one set with the WDL pass's state and constants.
    void Draw(uint32_t set);
    // After the draws: builds the levels the claims asked for, nearest first, within the frame's budget.
    void EndFrame();
    void Reset();
}

#endif
