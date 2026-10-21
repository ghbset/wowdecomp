// VFOG water: native port of coa-vfog's modern water (src/water_*.cpp, shaders/vw_*.hlsl, @ 4e31ddf, GPLv3).
// Not in the 3.3.5a binary. docs/VFOG_PORT.md "Water" is the spec: passes, mask scheme, registers, CVars, deviations.
//
//   VolumetricFogWater.cpp       data (VFog\waterdata.bin), classification, CVars, the GPU passes, the liquid hooks
//   VolumetricFogWaterWaves.cpp  the waves: coa's spectra and FFT on the CPU (water_spectrum.cpp reference path),
//                                surface moments, foam and their mip chains, simulated on worker threads
//   VolumetricFogWaterWavesGpu.cpp  the same waves as GPU passes (default; the CPU path is the fallback)
#pragma once

#include <cstdint>
#include <vector>
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/WorldClient/VolumetricFog.h"

class CGxShader;
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

struct WorldViewport;

// ---- waves (VolumetricFogWaterWaves.cpp) ----------------------------------------------------------------------
// Shared by the CPU and GPU paths.
constexpr double kLoopSeconds = 1024.0;
constexpr double kMaxFoamStepSeconds = 0.1;                     // coa kMaxFoamStepSeconds
constexpr float kWindDirection[2] = { 0.8f, 0.6f };             // coa water_renderer.cpp kWindDirection

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

// GPU waves (VolumetricFogWaterWavesGpu.cpp, vfogWaterGpuWaves): the same simulation as off-screen passes,
// VFogWave{Evolve,Fft,Surface,Mip,Pack}; docs/VFOG_PORT.md "Waves". WaveSpectrum = the CPU Configure's h0 for it:
// RGBA16F texels (half towards.re | towards.im << 16 | away.re << 32 | loop frequency mod 2048 << 48), prescaled n^2 / 256^2 * 2^-exponent (the 1/2 per
// butterfly stage gives the rest of the CPU's 1/256^2; the frequency is not) with exponent >= 0 keeping |texel| <= 16384.
struct WaveSpectrum {
    std::vector<uint64_t> h0;           // coarse halves (exact) + the frequency
    std::vector<uint64_t> h0Low;        // residuals: h0 + h0Low = the CPU's float h0 to ~2^-20
    int exponent = 0;
    float loopPerRootRadius = 0.0f;
    float halfTexelsPerUnit = 0.0f;     // the surface stencil, as Step
    float inverseWide = 0.0f;
    int wide = 1;
};
void ComputeWaveSpectrum(const WaterFftTile& tile, const WaveSettings& settings, WaveSpectrum& out);
// One step of the tiles in `tileMask` into their GPU maps (restores vp's target and state). False: not available
// (no RGBA16F targets, a shader missing, a target failed); the caller runs the CPU path.
bool WaveGpuStep(const std::vector<WaterFftTile>& tiles, uint32_t tileMask, const WaveSettings& settings, double seconds, const WorldViewport& vp);
bool WaveGpuMaps(int tile, CGxTex*& slopes, CGxTex*& state);    // false: no GPU maps for it (bind the CPU ones)
void WaveGpuRelease();                      // drops the GPU textures (CPU path chosen)
void WaveGpuDeviceRestored();               // drops them and retries the GPU path
void WaveGpuShutdown();

// ---- ripples and wakes (VolumetricFogWaterRipples.cpp) --------------------------------------------------------
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
    uint32_t outerHighWater;    // SetPs high-water of the enclosing pass group
};

void SetPs(uint32_t reg, const float* values, uint32_t count);
CGxShader* Shader(CGxShader*& slot, const char* name);         // the pixel shader Shaders\Pixel\name, nullptr unless valid
void DropInvalidShaders(CGxShader** slots, size_t count);       // device restored: a failed create is retried
void DestroyTex(CGxTex*& tex);
void Bind(int stage, CGxTex* tex);
// gxNormalBuffer: the device's normal target on s4 with c9.x = 1, or c9.x = 0 (vf_gbuffer.hlsli rebuilds from depth).
void BindNormalBuffer();
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
// LEGION FFX post: fxaa 0 (colour shift only), 1 low, 2 high, then the 3x3 linear-light colour matrix (rows in xyz).
bool RenderPost(int fxaa, const float colourShift[3][4], const WorldViewport& world);

// vf_common c0-c8 (VolumetricFogWater.cpp): world viewport vp, the depth texture's size, in's view and projection.
bool CommonRows(const FrameInputs& in, const WorldViewport& vp, const CGxTex* depth, float common[9][4]);
// vf_shadow.hlsli c56-c62 and its two maps (VolumetricFogRenderer.cpp, SHADOW_PORT_PLAN S3) for the view space whose
// view-to-world is viewToWorld; all zero (unshadowed) when off or without the bands.
void SunShadowRegisters(bool on, float canopy, const float viewToWorld[16], float shadow[7][4], CGxTex* maps[2]);
void SetShadowFadeRegister();   // c64 (gfxShadowFade), with the c56-c62 above

// ---- caustics and the underwater tint (VolumetricFogCaustics.cpp, wxl-water caustics; docs/BACKLOG_PORT.md) ------
void RegisterCausticCVars();                // from RegisterCVars
bool CausticsUnderWanted();                 // waterCaustics or waterUnderwaterTint on
// WaterFlushDone: the bed under the water (bed = the pre-water view Z copy, water = the copy with it, |r| = view Z),
// over the shaded water; stencil = the water class tags are in the stencil (the interior class is skipped).
void CausticsAbove(const FrameInputs& in, const float common[9][4], const WorldViewport& vp, CGxTex* bed, CGxTex* water, bool stencil);
// OnWorldDone with the camera in liquid: caustics on what the scene depth shows below the surface + the screen tint.
void CausticsUnder(const FrameInputs& in);

// Driver accessors (VolumetricFog.cpp): this frame's inputs once OnOpaqueDone captured them.
bool WaterFrame(const FrameInputs*& in, const Config*& cfg);
bool StockFogReplaced();   // the stock fog is pushed out this frame (coa !stockFogApplies)
bool WaterArmed();         // modern water draws in this liquid pass

}  // namespace VolumetricFog
