// VFOG WP1: CPU fog model (port of coa-vfog src/fog_model.cpp). Stub from "vfog: scaffolding".
#include "WoW/Source/WorldClient/VolumetricFog.h"

namespace VolumetricFog {

FogParams BuildFogParams(const FrameInputs& in, const Config& cfg) {
    return FogParams{};
}

bool AnyLayerNoise(const FogParams& fog) {
    return false;
}

void AdvanceNoise(const FogParams& fog, const float camPos[3], double seconds) {
}

void NoiseRegisters(const FogParams& fog, LayerNoiseRegisters out[kSceneLayers]) {
    for (int i = 0; i < kSceneLayers; i++) {
        out[i] = LayerNoiseRegisters{};
    }
}

}  // namespace VolumetricFog
