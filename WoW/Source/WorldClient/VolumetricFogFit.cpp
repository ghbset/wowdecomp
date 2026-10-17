// VFOG WP1: linear stock-fog fit for batches drawn after the composite (port of coa-vfog
// src/transparent_fog.cpp FitStockFog + wxl-vfog FitStockFogAt). Stub.
#include "WoW/Source/WorldClient/VolumetricFog.h"

namespace VolumetricFog {

StockFogFit FitStockFog(const FogParams& fog, const FrameInputs& in, const Config& cfg) {
    return StockFogFit{};
}

StockFogFit FitStockFogAt(const FogParams& fog, const FrameInputs& in, const Config& cfg, const float worldPos[3]) {
    return StockFogFit{};
}

}  // namespace VolumetricFog
