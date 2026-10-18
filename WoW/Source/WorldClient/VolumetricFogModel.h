// VFOG WP1: private types of the CPU fog model (port of coa-vfog src/fog_model.h, src/fog_data.h,
// src/transparent_fog.cpp @ 4e31ddf). Shared by VolumetricFogModel/Data/Noise/Fit.cpp; plain C++17.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "WoW/Source/WorldClient/VolumetricFog.h"

namespace VolumetricFog {

// ---- Classic/Forever authored fog (coa-vfog src/fog_data.h) -----------------------------------
constexpr int kGradingCurveEntries = 32;
constexpr int kMaxAuthoredLayers = 3;
constexpr float kMinimumClassicCoverage = 0.5f;
constexpr int kMaxSphereLights = 3;
constexpr int kMaxZoneLightNesting = 3;
constexpr int kMaxBlendedLights = kMaxSphereLights + kMaxZoneLightNesting + 1;
constexpr int kNoScreenEffectLightSlot = -1;
constexpr float kZoneLightEdgeFade = 100.0f;
constexpr int kLightParamsSlots = 8;

struct AuthoredNoise {
    float presence;
    float fade[3];
    float octaveShare[kAuthoredNoiseOctaves];
    float tileYards[kAuthoredNoiseOctaves];
    float velocity[kAuthoredNoiseOctaves][3];
    float unmappedToggle;
};

struct AuthoredLayer {
    float diffuse[3];
    float emissive[3];
    float shadowEmissive[3];
    float start;
    float density;
    float shadowMultiplier;
    float upperDensity;
    float upperHeight;
    float lowerDensity;
    float lowerHeight;
    float intensity;
    float g;
    float strength;
    float exponent;
    uint32_t flags;
    AuthoredNoise noise;
};

struct LightParamsSelection {
    float stormBlend = 0.0f;
    int screenEffectSlot = kNoScreenEffectLightSlot;
};

struct AuthoredFog {
    int layerCount;
    AuthoredLayer layers[kMaxAuthoredLayers];
    int lightCount;
    uint32_t lightIds[kMaxBlendedLights];
    float lightWeights[kMaxBlendedLights];
    float coverage;
    bool hasClassicDirectLight;
    float classicDirectLight[3];
    bool hasGlow;
    float glow;
    bool hasGradingCurve;                       // vfogGrade (VolumetricFog.cpp PrepareGrade)
    float gradingCurve[kGradingCurveEntries];
};

// VolumetricFogData.cpp. LoadFogData() reads the archive file and hands it to LoadFogDataFromMemory.
bool LoadFogDataFromMemory(const void* data, size_t size);
bool FogDataLoaded();
void EnsureFogData();   // LoadFogData once, whoever asks first (the driver's first enabled frame, or ResolveFogData)
// True when Classic fog covers the position (coverage >= 0.5 on a map with fog); out is filled either way.
bool ResolveFogData(int mapId, const float position[3], float dayFraction, const LightParamsSelection& selection,
                    AuthoredFog& out);

// ---- model (VolumetricFogModel.cpp, coa-vfog src/fog_model.cpp) --------------------------------
constexpr float kMoonLightScale = 0.35f;
constexpr float kNoiseCurveContrast = 20.0f;
constexpr float kNoiseCurvePivot = 0.5f;
constexpr float kNoiseCurveMean = 0.5f;

void UnpackColor(uint32_t argb, float* rgb);
// The LightParams selection of the frame's inputs (storm blend, screen-effect slot).
LightParamsSelection SelectionFor(const FrameInputs& in);

inline float SmoothStep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float Dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
// BuildFogParams with the data already resolved (nullptr = derived layers).
FogParams BuildFogParamsFrom(const FrameInputs& in, const Config& cfg, const AuthoredFog* authored);
float EnergyNormalisedPhaseScale(float g);
float NoiseDensityCurve(float x);
float MeanNoiseDensity(const LayerNoise& noise);
void MeanNoiseEmissive(const LayerNoise& noise, const float* emissive, float* out);
FogLayer MeanNoiseLayer(const FogLayer& layer, const LayerNoise& noise, float lightAboveHorizon);
FogParams WithMeanNoise(const FogParams& fog);

void Mul4x4(const float* a, const float* b, float* out);
bool Invert4x4(const float* m, float* out);
void TransformDirection(const float* v, const float* m, float* out);

// ---- CPU ray integration (coa-vfog src/transparent_fog.cpp; the CPU twin of vf_integrate.hlsli) ---
struct FogRay {
    float world[3];                 // unit direction, world
    float distancePerViewDepth;     // ray length per unit of planar view depth
    float weight;
};

struct FogSample {
    float transmittance;
    float inScatter[3];             // premultiplied, before exposure
};

struct FogRayLights {               // local lights relative to the camera; count 0 = none
    const LocalLight* lights = nullptr;
    uint32_t count = 0;
    float phase = 0.0f;
    float relative[kMaxLocalLights][3] = {};
};

FogRayLights RayLightsFor(const FrameInputs& in, uint32_t count, float phase);
// Marches `steps` steps of `stepDepth` view depth; samples[0..steps] (samples[0] is the camera).
void IntegrateFogRay(const FogParams& fog, const FogRayLights& lights, const FogRay& ray, const FrameInputs& in,
                     float stepDepth, int steps, FogSample* samples);

}  // namespace VolumetricFog
