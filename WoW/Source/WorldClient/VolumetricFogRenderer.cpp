// VFOG WP6: GPU passes (port of coa-vfog src/renderer.cpp onto the Gx API). Stub.
#include "WoW/Source/WorldClient/VolumetricFog.h"

namespace VolumetricFog {

bool RendererReady() {
    return false;
}

void RenderFog(const FogParams& fog, const FrameInputs& in, const Config& cfg, uint32_t frame, double seconds) {
}

void InvalidateHistory() {
}

void RendererShutdown() {
}

}  // namespace VolumetricFog
