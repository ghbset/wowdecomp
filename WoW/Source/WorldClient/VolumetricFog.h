// VFOG: native port of coa-vfog (volumetric fog, /root/wotlk-renderers/coa-vfog @ 4e31ddf, GPLv3).
// Not in the 3.3.5a binary. Architecture, pass list, register contract and work packages:
// docs/VFOG_PORT.md. This header is the frozen contract between the work packages; a package that
// needs more puts it in its own private header (VolumetricFog<Part>.h) instead of editing this one.
//
// Data flow per frame:
//   CWorld::Update          -> VolumetricFog::OnWorldUpdate()   decide, snapshot DNInfo fog, push stock fog out
//   OnWorldRender M2PASS_0  -> VolumetricFog::OnOpaqueDone()    capture view/proj/viewport, lights
//   after RenderLiquidPass  -> VolumetricFog::OnLiquidDone()    BuildFogParams + RenderFog (the composite)
//   CM2SceneRender::SetupLighting -> VolumetricFog::BatchFog()  fitted linear fog for later transparents
//   end of OnWorldRender    -> VolumetricFog::OnWorldDone()     restore DNInfo fog
#pragma once

#include <cstdint>

class CGxTex;
struct CVar;

namespace VolumetricFog {

// ---- constants (coa-vfog src/fog_model.h, src/fog_data.h, src/engine_lights.h) ----------------
constexpr int kFogLayers = 4;           // 3 scene layers + the distance layer
constexpr int kSceneLayers = 3;
constexpr int kDistanceFogLayer = 3;
constexpr int kAuthoredNoiseOctaves = 2;
constexpr int kMaxLocalLights = 8;

// ---- settings (CVars, VolumetricFog.cpp fills this every frame; coa-vfog src/config.h) --------
struct Config {
    bool enable = true;             // vfog
    int quality = 2;                // vfogQuality: 1 = 1/4 res 16 steps, 2 = 1/2 res 24, 3 = 1/2 res 32
    float density = 1.0f;           // vfogDensity
    float haze = 1.0f;              // vfogHaze
    float groundFog = 0.6f;         // vfogGroundFog
    float farFog = 1.0f;            // vfogFarFog (distance layer; replaces stock linear fog)
    bool transparentFog = true;     // vfogTransparent: early composite + fitted fog for later batches
    bool fadeEarly = true;          // vfogFadeEarly: fading M2s drawn before the early composite (wxl-vfog 0.6)
    int dataMode = 1;               // vfogData: 0 derived only, 1 Classic/Forever data where it covers
    int colorSpace = 1;             // 1 = linear-light scattering (gamma-decoded colours)
    int classicPhase = 0;
    float sunScatter = 1.0f;
    float ambient = 1.75f;          // vfogAmbient
    float exposure = 1.0f;
    float classicExposure = 0.5f;
    bool localLights = true;        // vfogLocalLights
    float localLightIntensity = 2.5f;
    float localLightPhase = 0.3f;
    bool interiorAware = true;
    float interiorDensity = 0.15f;
    float noiseAmount = 0.15f;      // vfogNoise
    float noiseScale = 0.025f;
    float noiseWindSpeed = 0.5f;
    bool classicNoise = true;
    bool glowCompensation = true;
    float maxDistance = 5000.0f;    // vfogMaxDistance
    float startOffset = 10.0f;      // vfogStartOffset (wxl-vfog 0.6.11)
    float temporal = 0.85f;         // vfogTemporal (0 = off, no jitter)
    int debugView = 0;              // vfogDebug 0..3
    float godRays = 1.0f;           // vfogGodRays: coa GodRays, 0 = off, 0..4 strength (default 1)
    bool grade = false;             // vfogGrade: coa ColorGrading > 0 (coa ships 0 = off)
    float gradeStrength = 1.0f;     // vfogGradeStrength: coa ColorGrading's 0..1 strength when on
    bool dither = true;             // vfogDither: +-0.5/255 dither on the 8-bit fog stores and the grade
};

// ---- per-frame engine inputs (coa-vfog src/engine.h FrameInputs, read natively) ----------------
struct LocalLight {
    float position[3];      // world
    float color[3];         // as uploaded (linear, x intensity)
    float attenuation[3];   // constant, linear, quadratic
    float cutoff;           // radius where the contribution falls under 1/256
};

struct FrameInputs {
    float cameraRelativeView[16];   // row-major, view with the camera translation removed
    float glProjection[16];         // main projection (GxuXformCreateProjection_SG), row-major
    float camPos[3];
    float camTarget[3];
    float viewport[6];              // world viewport px: minX, maxX, minY, maxY, minZ, maxZ (maxZ = 0.94)
    float horizonBand[4];           // depth minZ, maxZ (0.998046875, 0.9990234375), near yd, far yd
    float dayFraction;
    float toLight[3];               // world, unit, toward the visible sun or moon
    bool lightIsMoon;
    uint32_t fogColor;              // ARGB, DNInfo m_fogInterior.m_color (as coa: fog group 1)
    uint32_t sunColor;              // m_bands.m_sunCloudColors[0]
    uint32_t directColor;           // m_bands.m_diffuse (+4)
    uint32_t ambientColor;          // m_bands.m_ambient (+0)
    float fogStart;                 // m_fogInterior start/end SAVED BEFORE the push-out
    float fogEnd;
    float zoneFogDistance;          // m_bands.m_fogEnd
    float farClip;                  // World::s_farClip
    float clientGlowAmount;         // m_bands.m_glow (0 when FFX glow is off)
    float interiorBlend;            // DNInfo m_interiorFogBlend (camera WMO blend)
    bool inLiquid;
    int mapId;
    float stormBlend;               // m_stormBlend
    int screenEffectSlot;           // forced LightParams slot or -1
    int localLightCount;
    LocalLight localLights[kMaxLocalLights];
};

// ---- CPU fog model output (coa-vfog src/fog_model.h, layout = the shader's registers) ----------
struct FogLayer {                   // 6 float4 registers, c12 + 6*i
    float start;
    float density;
    float g;
    float isotropic;
    float emissive[3];
    float strength;
    float diffuse[3];
    float exponent;
    float upperHeight;
    float upperFalloff;
    float lowerHeight;
    float lowerFalloff;
    float shadowEmissive[3];
    float shadowDensity;
    float shadowed;
    float skyFalloff;
    float endDistance;
    float densityVariation;
};
static_assert(sizeof(FogLayer) == 6 * 16, "FogLayer uploads as six float4 registers");

struct LayerNoise {
    float alpha;
    float octaveWeight[kAuthoredNoiseOctaves];
    float inverseTileYards[kAuthoredNoiseOctaves];
    float velocity[kAuthoredNoiseOctaves][3];
    float fade[3];
};

struct LayerNoiseRegisters {        // 4 float4 registers, c36 + 4*i
    float octaveOffsetAndInverseTile[kAuthoredNoiseOctaves][4];
    float fadeAndAlpha[4];
    float octaveWeights[4];
};
static_assert(sizeof(LayerNoiseRegisters) == 4 * 16, "LayerNoiseRegisters uploads as four float4 registers");

struct FogParams {
    FogLayer layers[kFogLayers];
    LayerNoise noise[kSceneLayers];
    float lightColor[3];
    float rayColor[3];
    float lightVisibility;
    float lightAboveHorizon;
    float directLightMatch;
    float maxDistance;
    float horizonStart;
    float farClip;
    float referenceZ;
    float farLimit;
    bool linear;
    bool authored;
};

// Linear fog for stock-fogged batches drawn after the composite (coa-vfog src/transparent_fog.cpp).
struct StockFogFit {
    bool valid;
    float start;
    float end;
    float density;          // stock fog exponent (1 = linear)
    float color[3];         // display space, 0..1
};

// Noise volume packed as a 2D atlas of slices (no volume textures in the Gx API): docs/VFOG_PORT.md
// "Noise atlases". A8R8G8B8 texels, row-major, width*height*4 bytes.
struct NoiseAtlas {
    uint32_t width;
    uint32_t height;
    const uint8_t* pixels;
};

// ===== WP1 model: VolumetricFogModel.cpp / VolumetricFogData.cpp / VolumetricFogNoise.cpp / VolumetricFogFit.cpp
bool LoadFogData();                                                     // VFog\fogdata.bin through SFile
FogParams BuildFogParams(const FrameInputs& in, const Config& cfg);     // data resolve + derived + distance layer
bool AnyLayerNoise(const FogParams& fog);
void AdvanceNoise(const FogParams& fog, const float camPos[3], double seconds);
void NoiseRegisters(const FogParams& fog, LayerNoiseRegisters out[kSceneLayers]);
const NoiseAtlas& DensityNoiseAtlas();      // 32^3 hashed, 2 octaves source (s9)
const NoiseAtlas& AuthoredNoiseAtlas();     // 64^3 Perlin (s10), value in every channel
StockFogFit FitStockFog(const FogParams& fog, const FrameInputs& in, const Config& cfg);
StockFogFit FitStockFogAt(const FogParams& fog, const FrameInputs& in, const Config& cfg, const float worldPos[3]);

// ===== WP6 renderer: VolumetricFogRenderer.cpp
bool RendererReady();                       // shaders loaded and the device gives a scene depth texture
bool RenderFog(const FogParams& fog, const FrameInputs& in, const Config& cfg, uint32_t frame, double seconds);
void InvalidateHistory();
bool CompositeOverSceneCopy();              // last RenderFog blended over a scene copy (coa LinearOverSceneCopy)
void RenderGodRays();                       // OnWorldDone: the rays the last RenderFog masked, over the finished world
void RendererShutdown();

// ===== WP7 driver: VolumetricFog.cpp
void RegisterCVars();                       // from CWorldParam::Initialize
bool Active();                              // fog will draw (or drew) this frame; stock fog is pushed out
void OnWorldUpdate();                       // CWorld::Update, after CWorld::farFog is taken
void OnViewerLiquid();                      // CWorldScene::Render, right after UpdateViewerLiquid
void OnOpaqueDone();                        // after scene->Draw(M2PASS_0)
void OnLiquidDone();                        // after RenderLiquidPass, above-water branch only
void OnWorldDone();                         // end of CGWorldFrame::OnWorldRender
void OnWorldRendered();                     // CGWorldFrame::RenderWorld, after OnWorldRender (glow done), before names
bool GlareDrawn();                          // OnLiquidDone drew DayNight::RenderGlares under the fog: skip the stock call
bool LiquidDepthWrite();                    // force liquid depth writes while Active()
bool WaterHidesClientRipples();              // CMap::CreateWaterRipple: the modern water's ripples replace the client's sprites
bool BatchFog(const float modelPos[3], float& start, float& end, float& density, float color[3]);

}  // namespace VolumetricFog
