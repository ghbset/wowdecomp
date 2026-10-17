// VFOG WP1: noise volumes as 2D slice atlases (port of coa-vfog src/noise_volume.cpp). Stub.
#include "WoW/Source/WorldClient/VolumetricFog.h"

namespace VolumetricFog {

const NoiseAtlas& DensityNoiseAtlas() {
    static const NoiseAtlas empty = { 0, 0, nullptr };
    return empty;
}

const NoiseAtlas& AuthoredNoiseAtlas() {
    static const NoiseAtlas empty = { 0, 0, nullptr };
    return empty;
}

}  // namespace VolumetricFog
