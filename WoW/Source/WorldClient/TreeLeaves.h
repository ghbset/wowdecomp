#ifndef WOW_SOURCE_WORLDCLIENT_TREELEAVES_H
#define WOW_SOURCE_WORLDCLIENT_TREELEAVES_H

// TREELEAVES: not in 12340. Port of wxl-treewind Leaves.cpp (branch leaves). The trees M2TreeSway sways also
// shed: each of the nearest live trees owns a handful of leaf slots that start in its crown and fall to its
// base with the wind, more while its gust is up. No per-leaf state: a leaf's position is a function of
// (tree, slot, time), worked out in the TreeLeavesVS vertex shader and drawn as one batch from a static
// buffer at the entry of CWorldScene::RenderLiquidPass (the plugin's hook on 0x00790A80), writing depth so
// the vfog composite and the water treat a leaf as a solid model.

#include <cstdint>

class CM2Model;

namespace TreeLeaves {

// CVars treeLeaves / treeLeavesDensity (WorldParam_CVar.cpp).
extern int32_t s_enabled;
extern float s_perTree;   // leaf slots per tree, 1..48 (the plugin's PerTree)

// From M2TreeSway::World (any thread) for a swaying tree: its unswayed world transform (row vectors,
// translation in [12..14]), its sway seed (0..2pi), primary wind direction and speed, scene time in ms.
void NoteTree(const CM2Model* model, const float* world, float seed, float gust, float windX, float windY, float windSpeed, uint32_t nowMs);

// Start of CWorldScene::RenderLiquidPass (main thread).
void Draw();

} // namespace TreeLeaves

#endif
