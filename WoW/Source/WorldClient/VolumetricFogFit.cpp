// VFOG WP1: linear stock-fog fit for batches drawn after the composite (port of coa-vfog
// src/transparent_fog.cpp FitStockFog @ 4e31ddf, plus wxl-vfog's per-model FitStockFogAt).
#include "WoW/Source/WorldClient/VolumetricFogModel.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace VolumetricFog {

namespace {

constexpr float kStockFogFitDepth = 100.0f;
constexpr float kLinearStockFogExponent = 1.0f;
constexpr float kFitNdcColumns[] = { -0.8f, -0.4f, 0.0f, 0.4f, 0.8f };
constexpr float kFitNdcRows[] = { -0.6f, 0.0f, 0.6f };
constexpr int kFitRays = static_cast<int>(std::size(kFitNdcColumns) * std::size(kFitNdcRows));
constexpr float kCentreWeightFalloff = 1.5f;
constexpr int kFitDepthSteps = 25;
constexpr int kProfileSteps = 32;
constexpr float kThinNearFogTransmittance = 0.9f;
constexpr float kFogFreeTransmittance = 0.995f;
constexpr float kExtendedFitTransmittance = 0.5f;
constexpr double kMinFogSlope = -1.0e-6;
constexpr float kClampedTransmittance = 0.02f;
constexpr float kMinFogOpacity = 1.0e-4f;
constexpr float kHighlightKnee = 0.8f;
constexpr float kMinHighlightSpan = 1.0e-4f;
constexpr float kDisplayGamma = 2.2f;
constexpr double kMinLineDeterminant = 1.0e-12;
constexpr float kMidGreyLinear = 0.2176f;   // display 0.5 in linear light

using RaySamples = FogSample[kFitDepthSteps + 1];

struct FitLight {               // coa StockFogFitLight: what the composite was drawn with
    float exposure;
    float glowToCompensate;
};

struct WeightedLine {
    double weight = 0.0;
    double x = 0.0;
    double y = 0.0;
    double xx = 0.0;
    double xy = 0.0;

    void Add(double px, double py, double w) {
        this->weight += w;
        this->x += w * px;
        this->y += w * py;
        this->xx += w * px * px;
        this->xy += w * px * py;
    }

    bool Solve(double& slope, double& intercept) const {
        const double determinant = this->weight * this->xx - this->x * this->x;
        if (this->weight <= 0.0 || std::fabs(determinant) < kMinLineDeterminant) {
            return false;
        }
        slope = (this->weight * this->xy - this->x * this->y) / determinant;
        intercept = (this->y - slope * this->x) / this->weight;
        return true;
    }
};

void Normalize3(float* v) {
    const float length = std::sqrt(Dot3(v, v));
    if (length <= 0.0f) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        v[i] /= length;
    }
}

// coa renderer.cpp:1479 (composite c96.x, c98.w). Glow is compensated only when the composite blended over a
// scene copy (VolumetricFogRenderer.cpp), so the fit matches what was drawn.
FitLight CompositeLight(const FogParams& fog, const FrameInputs& in, const Config& cfg) {
    const float glow = CompositeOverSceneCopy() && cfg.glowCompensation ? in.clientGlowAmount : 0.0f;
    return { fog.authored ? cfg.classicExposure : cfg.exposure, glow };
}

FogRayLights CompositeLights(const FrameInputs& in, const Config& cfg) {
    return RayLightsFor(in, cfg.localLights ? static_cast<uint32_t>(kMaxLocalLights) : 0u, cfg.localLightPhase);
}

int BuildFitRays(const FrameInputs& in, FogRay* rays) {
    float viewToWorld[16];
    if (!Invert4x4(in.cameraRelativeView, viewToWorld)) {
        return 0;
    }
    const float* proj = in.glProjection;
    int count = 0;
    for (float row : kFitNdcRows) {
        for (float column : kFitNdcColumns) {
            float view[3] = { (column - proj[8]) / proj[0], (row - proj[9]) / proj[5], 1.0f };
            const float distancePerViewDepth = std::sqrt(Dot3(view, view));
            Normalize3(view);
            FogRay& ray = rays[count++];
            TransformDirection(view, viewToWorld, ray.world);
            Normalize3(ray.world);
            ray.distancePerViewDepth = distancePerViewDepth;
            ray.weight = std::exp(-kCentreWeightFalloff * (column * column + row * row));
        }
    }
    return count;
}

void IntegrateRays(const FogParams& fog, const FogRayLights& lights, const FogRay* rays, int rayCount,
                   const FrameInputs& in, float stepDepth, RaySamples* samples) {
    for (int r = 0; r < rayCount; ++r) {
        IntegrateFogRay(fog, lights, rays[r], in, stepDepth, kFitDepthSteps, samples[r]);
    }
}

bool Clamped(double slope, double intercept, float depth, float transmittance) {
    return slope * depth + intercept <= 0.0 && transmittance < kClampedTransmittance;
}

bool FitTransmittanceLine(const FogRay* rays, const RaySamples* samples, int rayCount, float stepDepth, double& slope,
                          double& intercept) {
    WeightedLine all;
    for (int r = 0; r < rayCount; ++r) {
        for (int step = 0; step <= kFitDepthSteps; ++step) {
            all.Add(step * stepDepth, samples[r][step].transmittance, rays[r].weight);
        }
    }
    if (!all.Solve(slope, intercept)) {
        return false;
    }
    WeightedLine unclamped;
    for (int r = 0; r < rayCount; ++r) {
        for (int step = 0; step <= kFitDepthSteps; ++step) {
            const float depth = step * stepDepth;
            if (!Clamped(slope, intercept, depth, samples[r][step].transmittance)) {
                unclamped.Add(depth, samples[r][step].transmittance, rays[r].weight);
            }
        }
    }
    double refinedSlope = 0.0;
    double refinedIntercept = 0.0;
    if (unclamped.Solve(refinedSlope, refinedIntercept)) {
        slope = refinedSlope;
        intercept = refinedIntercept;
    }
    return true;
}

float WindowEndTransmittance(const FogRay* rays, const RaySamples* samples, int rayCount) {
    float weighted = 0.0f;
    float weights = 0.0f;
    for (int r = 0; r < rayCount; ++r) {
        weighted += rays[r].weight * samples[r][kFitDepthSteps].transmittance;
        weights += rays[r].weight;
    }
    return weights > 0.0f ? weighted / weights : 1.0f;
}

const FogRay& ViewAxisRay(const FogRay* rays, int rayCount) {
    return *std::max_element(rays, rays + rayCount, [](const FogRay& a, const FogRay& b) { return a.weight < b.weight; });
}

struct TransmittanceProfile {
    float stepDepth;
    float transmittance[kProfileSteps + 1];
};

TransmittanceProfile ProfileToTheFarClip(const FogParams& fog, const FogRay& ray, const FrameInputs& in) {
    TransmittanceProfile profile = {};
    profile.stepDepth = std::max(kStockFogFitDepth, fog.farClip) / kProfileSteps;
    FogSample samples[kProfileSteps + 1];
    IntegrateFogRay(fog, FogRayLights{}, ray, in, profile.stepDepth, kProfileSteps, samples);
    for (int step = 0; step <= kProfileSteps; ++step) {
        profile.transmittance[step] = samples[step].transmittance;
    }
    return profile;
}

float DepthWhereTransmittanceFalls(const TransmittanceProfile& profile, float level) {
    for (int step = 1; step <= kProfileSteps; ++step) {
        const float before = profile.transmittance[step - 1];
        const float after = profile.transmittance[step];
        if (after > level) {
            continue;
        }
        const float along = before > after ? (before - level) / (before - after) : 0.0f;
        return (step - 1) * profile.stepDepth + along * profile.stepDepth;
    }
    return kProfileSteps * profile.stepDepth;
}

float ExtendedWindowDepth(const TransmittanceProfile& profile, float nearTransmittance) {
    const float reach = SmoothStep(kThinNearFogTransmittance, kFogFreeTransmittance, nearTransmittance);
    const float extended = std::max(DepthWhereTransmittanceFalls(profile, kExtendedFitTransmittance), kStockFogFitDepth);
    return kStockFogFitDepth + reach * (extended - kStockFogFitDepth);
}

float RollOffHighlight(float colour) {
    const float span = std::max(1.0f - kHighlightKnee, kMinHighlightSpan);
    if (colour <= kHighlightKnee) {
        return colour;
    }
    return kHighlightKnee + span * (1.0f - std::exp(-(colour - kHighlightKnee) / span));
}

float BeforeClientGlow(float onScreen, float glow) {
    return 2.0f * onScreen / (1.0f + std::sqrt(1.0f + 4.0f * glow * onScreen));
}

// transparent_fog.cpp FittedFogColour, unpacked: display-space 0..1 (coa packed it into ARGB bytes).
void FittedFogColour(const FogRay* rays, const RaySamples* samples, int rayCount, bool linear, const FitLight& light,
                     float* out) {
    double scattered[3] = {};
    double opacity = 0.0;
    for (int r = 0; r < rayCount; ++r) {
        for (int step = 1; step <= kFitDepthSteps; ++step) {
            const FogSample& sample = samples[r][step];
            const float sampleOpacity = 1.0f - sample.transmittance;
            if (sampleOpacity <= kMinFogOpacity) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                scattered[c] += rays[r].weight * sample.inScatter[c];
            }
            opacity += rays[r].weight * sampleOpacity;
        }
    }
    for (int c = 0; c < 3; ++c) {
        const float unpremultiplied = opacity > 0.0 ? static_cast<float>(light.exposure * scattered[c] / opacity) : 0.0f;
        float colour = RollOffHighlight(std::max(unpremultiplied, 0.0f));
        if (linear) {
            colour = BeforeClientGlow(std::pow(colour, 1.0f / kDisplayGamma), light.glowToCompensate);
        }
        out[c] = std::clamp(colour, 0.0f, 1.0f);
    }
}

}  // namespace

// transparent_fog.cpp:536-566: 15 rays, 100 yd window (extended in thin near fog), least-squares line of
// transmittance over planar view depth -> linear fog start/end, exponent 1.
StockFogFit FitStockFog(const FogParams& drawn, const FrameInputs& in, const Config& cfg) {
    const FogParams fog = WithMeanNoise(drawn);
    const FitLight light = CompositeLight(drawn, in, cfg);
    FogRay rays[kFitRays];
    const int rayCount = BuildFitRays(in, rays);
    if (rayCount == 0) {
        return StockFogFit{};
    }
    const FogRayLights lights = CompositeLights(in, cfg);
    RaySamples samples[kFitRays];
    float stepDepth = kStockFogFitDepth / kFitDepthSteps;
    IntegrateRays(fog, lights, rays, rayCount, in, stepDepth, samples);
    const float nearTransmittance = WindowEndTransmittance(rays, samples, rayCount);
    if (nearTransmittance > kThinNearFogTransmittance) {
        const TransmittanceProfile profile = ProfileToTheFarClip(fog, ViewAxisRay(rays, rayCount), in);
        if (profile.transmittance[kProfileSteps] > kFogFreeTransmittance) {
            return StockFogFit{};
        }
        stepDepth = ExtendedWindowDepth(profile, nearTransmittance) / kFitDepthSteps;
        IntegrateRays(fog, lights, rays, rayCount, in, stepDepth, samples);
    }
    double slope = 0.0;
    double intercept = 0.0;
    if (!FitTransmittanceLine(rays, samples, rayCount, stepDepth, slope, intercept) || slope > kMinFogSlope) {
        return StockFogFit{};
    }
    StockFogFit fit = {};
    fit.valid = true;
    fit.start = static_cast<float>((1.0 - intercept) / slope);
    fit.end = static_cast<float>(-intercept / slope);
    fit.density = kLinearStockFogExponent;
    FittedFogColour(rays, samples, rayCount, fog.linear, light, fit.color);
    return fit;
}

// wxl-vfog transparent_fog.cpp:571-620: the fog of ONE model, marched camera -> model; a linear fog from the
// camera that holds the marched opacity at the model's planar depth (start 0, end = depth / opacity).
StockFogFit FitStockFogAt(const FogParams& drawn, const FrameInputs& in, const Config& cfg, const float worldPos[3]) {
    float viewToWorld[16];
    if (!Invert4x4(in.cameraRelativeView, viewToWorld)) {
        return StockFogFit{};
    }
    const float axis[3] = { 0.0f, 0.0f, 1.0f };
    float forward[3];
    TransformDirection(axis, viewToWorld, forward);
    const float to[3] = { worldPos[0] - in.camPos[0], worldPos[1] - in.camPos[1], worldPos[2] - in.camPos[2] };
    const float distance = std::sqrt(Dot3(to, to));
    const float depth = Dot3(to, forward);
    if (!(distance > 1.0f) || !(depth > 1.0f) || !(distance < drawn.maxDistance)) {
        return StockFogFit{};
    }
    const FitLight light = CompositeLight(drawn, in, cfg);
    const FogRay ray = { { to[0] / distance, to[1] / distance, to[2] / distance }, distance / depth, 1.0f };
    const FogParams fog = WithMeanNoise(drawn);
    RaySamples samples[1];
    IntegrateFogRay(fog, FogRayLights{}, ray, in, depth / kFitDepthSteps, kFitDepthSteps, samples[0]);
    const FogSample& at = samples[0][kFitDepthSteps];
    const float opacity = 1.0f - at.transmittance;
    if (opacity <= kMinFogOpacity) {
        return StockFogFit{};
    }
    StockFogFit fit = {};
    fit.valid = true;
    fit.start = 0.0f;
    fit.end = depth / opacity;
    fit.density = kLinearStockFogExponent;
    FittedFogColour(&ray, samples, 1, fog.linear, light, fit.color);
    if (!fog.linear || !CompositeOverSceneCopy()) {
        return fit;     // the fixed-function composite mixes display colours, as the model's shader does
    }
    // The composite fogs in linear light (out = surface T + S); the model's shader mixes display colours
    // (out = surface (1 - f) + C f). f and C are the pair that matches the composite for a black surface and a
    // mid-grey one: C f = S^(1/2.2), (1 - f) / 2 = (0.2176 T + S)^(1/2.2) - S^(1/2.2).
    float scattered[3];
    float kept = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float s = std::clamp(light.exposure * at.inScatter[c], 0.0f, 1.0f);
        scattered[c] = std::pow(s, 1.0f / kDisplayGamma);
        kept += 2.0f * (std::pow(std::min(kMidGreyLinear * at.transmittance + s, 1.0f), 1.0f / kDisplayGamma) - scattered[c]) / 3.0f;
    }
    const float mixed = std::clamp(1.0f - kept, kMinFogOpacity, 1.0f);
    fit.end = depth / mixed;
    for (int c = 0; c < 3; ++c) {
        fit.color[c] = std::clamp(BeforeClientGlow(std::min(scattered[c] / mixed, 1.0f), light.glowToCompensate), 0.0f, 1.0f);
    }
    return fit;
}

}  // namespace VolumetricFog
