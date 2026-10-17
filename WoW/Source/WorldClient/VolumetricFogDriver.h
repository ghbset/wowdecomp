// VFOG WP7: private to the frame driver (VolumetricFog.cpp). Not in the 3.3.5a binary.
#pragma once

// Engine-side hooks defined in Engine/Source/Model2/CM2SceneRender.cpp (Engine does not include WoW headers, so the
// driver installs its functions there from RegisterCVars).
extern bool (*g_vfogM2BatchFog)(const float modelPos[3], float& start, float& end, float& density, float color[3]);
extern bool (*g_vfogM2FogParticles)();
