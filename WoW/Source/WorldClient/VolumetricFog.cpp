// VFOG WP7: frame driver, CVars and engine inputs (port of coa-vfog src/hooks.cpp, src/engine.cpp,
// src/engine_lights.cpp onto the decomp's own symbols). Stub.
#include "WoW/Source/WorldClient/VolumetricFog.h"

namespace VolumetricFog {

void RegisterCVars() {
}

bool Active() {
    return false;
}

void OnWorldUpdate() {
}

void OnOpaqueDone() {
}

void OnLiquidDone() {
}

void OnWorldDone() {
}

bool LiquidDepthWrite() {
    return false;
}

bool BatchFog(const float modelPos[3], float& start, float& end, float& density, float color[3]) {
    return false;
}

}  // namespace VolumetricFog
