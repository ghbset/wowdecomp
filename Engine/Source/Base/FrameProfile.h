#ifndef ENGINE_SOURCE_BASE_FRAMEPROFILE_H
#define ENGINE_SOURCE_BASE_FRAMEPROFILE_H

// CUSTOM (Badgermilk0 port): performance diagnostics. Where the main thread's world work goes, per section,
// smoothed, in milliseconds (GetSceneTimings() and /frametimes in Lua). Sections add up over a frame, so a section that
// runs several times (one Animate per model scene: world, portraits, UI models) reports their sum. FrameProfileEndFrame
// runs once per Present.
//
// Two ways to time: FrameProfileSwitch charges the time since the last switch to the current section and makes another
// current (FRAMEPROFILE_NONE stops), for long functions cut into consecutive parts; FrameProfileAdd charges an explicit
// interval, for code that may run inside a switched part (CM2Scene::Animate), which then switches to NONE around it.

#include <cstdint>

enum FrameProfileSection {
    FRAMEPROFILE_UNIT_UPDATE,   // CGUnit_C/CGGameObject_C::UpdateDisplay for every visible object (movement, effects)
    FRAMEPROFILE_WORLD_UPDATE,  // the rest of CGWorldFrame::OnWorldUpdate (camera, day/night, CWorld::Update)
    FRAMEPROFILE_CULL,          // CWorldScene::Render up to model animation: portal/frustum/occlusion culling
    FRAMEPROFILE_ANIM_PARALLEL, // CM2Model::AnimateMT, bone matrices, split across threads
    FRAMEPROFILE_ANIM_SERIAL,   // AnimateST (particles, ribbons, lights) and SetupLighting, main thread
    FRAMEPROFILE_ANIM_BATCH,    // CM2Scene::Animate's draw-list walk that builds the render elements
    FRAMEPROFILE_TERRAIN,       // CWorldScene::RenderChunks and RenderHorizon: terrain and the far horizon
    FRAMEPROFILE_WMO,           // RenderMapObjDefGroups, portal lists and sky: buildings and caves
    FRAMEPROFILE_WORLD_OTHER,   // the rest of CWorldScene::Render: map entities, liquid instances and flush
    FRAMEPROFILE_NAMEPLATES,    // after the world render: footprints, unit PostAnimate, nameplates
    FRAMEPROFILE_MODELS,        // CM2Scene::Draw (three passes) and the opaque Gxu draw list: doodads, units, objects
    FRAMEPROFILE_DETAIL,        // CWorldScene::RenderDetailDoodads: grass and ground clutter
    FRAMEPROFILE_LIQUID,        // liquid pass, weather, barriers
    FRAMEPROFILE_EFFECTS,       // spell visuals, particulate, transparent Gxu draw list, glares, chat bubbles
    FRAMEPROFILE_SHADOWS,       // CShadowQuery::Update: gathering casters and rendering the shadow maps
    // parts of a section above, timed with FrameProfileAdd; never switched to
    FRAMEPROFILE_GRASS_FILL,    // of DETAIL: grass batches rebuilt on the CPU (FillVertices/FillIndices)
    FRAMEPROFILE_GRASS_LIGHTS,  // of DETAIL: the lights picked for each grass chunk
    FRAMEPROFILE_COUNT,
    FRAMEPROFILE_NONE = FRAMEPROFILE_COUNT
};

enum FrameProfileCounter {
    FRAMEPROFILE_MODELS_ANIMATED, // models animated (attached ones count with their root)
    FRAMEPROFILE_DRAW_CALLS,      // CGxDevice::Draw calls, one API draw each (UI included)
    FRAMEPROFILE_GRASS_CHUNKS,    // terrain chunks with grass drawn
    FRAMEPROFILE_GRASS_DRAWS,     // grass batches drawn (up to four per chunk, one per texture)
    FRAMEPROFILE_GRASS_FILLS,     // grass batches rebuilt on the CPU
    FRAMEPROFILE_GRASS_STREAMED,  // grass batches that found no free baked buffer and went to a stream buffer
    FRAMEPROFILE_COUNTERS
};

struct FrameProfileStats {
    double m_ms[FRAMEPROFILE_COUNT];
    double m_counters[FRAMEPROFILE_COUNTERS];
    // GPU time per switched section and for the whole frame (first mark to Present), from a device that can time
    // its own work (D3D11 timestamp queries); m_gpuValid is false on the others
    double m_gpuMs[FRAMEPROFILE_COUNT];
    double m_gpuFrame;
    bool m_gpuValid;
};

extern FrameProfileStats g_frameProfile;

// set by a device that times the GPU: called at every FrameProfileSwitch with the section starting there
extern void (*g_frameProfileGpuMark)(FrameProfileSection section);

// the device's results for a finished frame, some frames late: ms per section and for the frame
void FrameProfileGpuRecord(const double* ms, double frame);

uint64_t FrameProfileNow();
void FrameProfileAdd(FrameProfileSection section, uint64_t start, uint64_t end);
void FrameProfileSwitch(FrameProfileSection section);
void FrameProfileCount(FrameProfileCounter counter, uint32_t value);
void FrameProfileEndFrame();

#endif
