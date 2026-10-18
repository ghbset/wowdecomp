// VFOG water: native port of coa-vfog's modern water (src/water_*.cpp, shaders/vw_*.hlsl, @ 4e31ddf, GPLv3).
// Not in the 3.3.5a binary. docs/VFOG_PORT.md "Water" is the spec: passes, mask scheme, registers, CVars, deviations.
//
//   VolumetricFogWater.cpp       data (VFog\waterdata.bin), classification, CVars, the GPU passes, the liquid hooks
//   VolumetricFogWaterWaves.cpp  the waves: coa's spectra and FFT on the CPU (water_spectrum.cpp reference path),
//                                surface moments, foam and their mip chains, simulated on worker threads
#pragma once

#include <cstdint>
#include <vector>
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/WorldClient/VolumetricFog.h"

class CGxTex;

namespace Liquid {
    class CInstance;
}

namespace VolumetricFog {

// ---- data (coa src/water_types.h, layout = tools/convert_forever_water.py) -------------------------------------
enum class WaterClass : uint8_t { None = 0, Lake = 1, River = 2, Ocean = 3, Interior = 4 };
constexpr int kWaterClassCount = 5;
constexpr int kWaterPresetTiles = 4;
constexpr int kWaterMaxTiles = 8;
constexpr int kWaterMaskSlots = 6;

struct WaterFftTile {
    uint32_t foreverTileId;
    float size;
    float amplitude;
    float windMultiplier;
    float windAlignment;
    float foam[3];      // bias, rate, decay
    float oxygen[3];
};
static_assert(sizeof(WaterFftTile) == 44, "waterdata.bin tile record");

struct WaterPreset {
    uint32_t foreverLiquidId;
    float absorption[4];
    float scatteringIntensities[4];
    float scatteringTop[4];
    float scatteringBottom[4];
    float depthFadeFoam[4];
    float shoreFoam[4];
    float waveFoam[4];
    float waveFoamScaling[4];
    float flow[4];
    float roughness[4];
    int32_t tiles[kWaterPresetTiles];
    int32_t masks[kWaterMaskSlots];
};
static_assert(sizeof(WaterPreset) == 204, "waterdata.bin preset record");

// ---- waves (VolumetricFogWaterWaves.cpp) ----------------------------------------------------------------------
// One tile's CPU maps, every mip level down to 1x1 back to back (level 0 first):
//   slopes: Gr1616F texels (half slope x | half slope y << 16), the box-filtered mean slope (coa moments.xy);
//   state:  Argb8888 texels, R = sqrt(variance / kVarianceRange), G = sqrt(wide slope^2 / kWideRange),
//           B = foam, A = oxygen (B, A read at level 0 only, as coa's foam state).
struct WaveMaps {
    uint32_t resolution = 0;
    uint32_t levels = 0;
    uint32_t offset[16] = {};       // first texel of each level
    std::vector<uint32_t> slopes;
    std::vector<uint32_t> state;
};

struct WaveSettings {
    int resolution;         // 64 / 128 / 256
    float windSpeed;        // vfogWaterWind
    float amplitudeScale;   // vfogWaterWaves^2 (coa PrepareWaves)
};

// Starts a simulation step for the tiles in `tileMask` at `seconds` on worker threads unless one is running.
void WaveLaunch(const std::vector<WaterFftTile>& tiles, uint32_t tileMask, const WaveSettings& settings, double seconds);
// True once a launched step has finished; its maps are then current (WaveMapsOf) until the next WaveCollect.
bool WaveCollect();
const WaveMaps* WaveMapsOf(int tile);       // nullptr: not simulated yet with the current settings
void WaveReset();                           // drops every map (settings or data changed)

// ---- ripples and wakes (VolumetricFogWaterRipples.cpp) --------------------------------------------------------
struct WorldViewport;
struct RippleShading {
    CGxTex* map;            // nullptr: no ripples this frame (bind the flat texture)
    float window[4];        // c65: origin xy (yd), 1 / extent, 1 / texels
    float shape[4];         // c66: step weight, gain (vfogWaterRipples), extent
    float fade[4];          // c67: fade end, 1 / fade width, texel yards
};
// From the water pass's Begin (water in view): reads the units in water, runs the due 30 Hz steps into the ripple maps
// (render targets; restores vp's target and state) and fills the shading constants.
void RippleFrame(const FrameInputs& in, float strength, int quality, double seconds, const WorldViewport& vp, RippleShading& out);
bool RipplesHideClientSprites(bool allowed);    // allowed = the CVars let the ripples replace the client's sprites
void RippleRelease();                           // device restored, water textures dropped
void RippleShutdown();

// ---- hooks ----------------------------------------------------------------------------------------------------
void RegisterWaterCVars();                  // from RegisterCVars
// Liquid::CRenderer::Flush, transparent pass, after the sort: moves water to the end grouped by class (stable, so the
// stock order holds inside a class) and arms the passes. Returns false when modern water does not draw this pass.
bool WaterArm(Liquid::CInstance** instances, uint32_t count);
void WaterBeforeDraw(const Liquid::CInstance* instance);   // between instances: finishes a class when the next differs
void WaterFlushDone();                      // after the last instance: shades the last class, disarms
void WaterShutdown();

// ---- full-screen pass helpers (VolumetricFogRenderer.cpp) -----------------------------------------------------
constexpr uint32_t kPassSavedPixelRegisters = 132;     // c0-c131, the fog's whole register range

struct WorldViewport {
    CGxTex* target;     // nullptr = the back buffer
    float minX, maxX, minY, maxY, minZ, maxZ;
    float x, y, w, h;
};

// Full-screen pass state; EndPasses undoes all of it.
struct SavedPassState {
    float constants[kPassSavedPixelRegisters * 4];
    C44Matrix projection;
    C44Matrix view;
};

void SetPs(uint32_t reg, const float* values, uint32_t count);
void Bind(int stage, CGxTex* tex);
// One quad through the fixed-function VS over the Gx viewport [minX..maxX, minY..maxY] of a width x height target,
// TEXCOORD0 = uv across it with (i + 0.5) / size at pixel centres on every backend.
void DrawQuad(uint32_t width, uint32_t height, float minX = 0.0f, float maxX = 1.0f, float minY = 0.0f, float maxY = 1.0f);
WorldViewport LiveWorldViewport();
void BeginPasses(SavedPassState& saved);
void EndPasses(const SavedPassState& saved, const WorldViewport& vp);   // unbinds stages 0-4 (BeginPasses turns the stencil test off)
// The Gx stencil (GxRs_Stencil*, docs "Stencil"): false when the scene depth has no stencil bits (every user falls back).
bool StencilAvailable();
// Stencil bits shared in one frame: the fading-model alpha (DrawFadingEarly, read in RenderFog) and the MSAA sample split.
constexpr int32_t kFadeBits = 0xE0;
constexpr int32_t kSplitBits = 0x1F;
// Test on, front and back: func against ref under readMask; pass / zfail ops written under writeMask; fail keeps.
void SetStencil(EGxStencilCmp func, int32_t ref, int32_t readMask, int32_t writeMask, EGxStencilOp pass, EGxStencilOp zfail = GxStencilOp_Keep);

// coa GradingRenderer::Grade (VolumetricFogRenderer.cpp): the 32-entry curve over a copy of the world viewport,
// opaque, RGB. False when it could not draw (shader missing, world not on the back buffer, no copy).
bool RenderGrade(const float* curve32, float strength, int dither, uint32_t frame, const WorldViewport& world);

// Driver accessors (VolumetricFog.cpp): this frame's inputs once OnOpaqueDone captured them.
bool WaterFrame(const FrameInputs*& in, const Config*& cfg);
bool StockFogReplaced();
bool WaterArmed();         // modern water draws in this liquid pass   // the stock fog is pushed out this frame (coa !stockFogApplies)

}  // namespace VolumetricFog
