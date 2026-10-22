#ifndef WOW_SOURCE_UI_WORLDLIGHTING_H
#define WOW_SOURCE_UI_WORLDLIGHTING_H

// CUSTOM (Badgermilk0 "modern lighting" port): HDR, bloom, SSAO and height fog as post passes of the D3D11 and
// Vulkan devices (Engine/Source/Gx/GxLighting.h). Every CVar defaults to off; all off, nothing is called on the
// device. D3D9 has no g_gxLighting, so the CVars do nothing there.

class C3Vector;

void WorldLightingRegisterCVars();
void WorldLightingBeginWorld();
void WorldLightingSetupView(const C3Vector& camera);
void WorldLightingOpaqueDone();
void WorldLightingEndWorld();

#endif
