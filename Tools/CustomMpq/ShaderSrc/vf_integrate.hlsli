// Port of coa-vfog shaders/vf_integrate.hlsli with wxl-vfog's speedups (ec1c66b: LayerRayTerms hoisted out
// of the step loop, early-out before the nearest layer start). Every constant index is a literal (layer
// registers through LoadConstantFogLayer, unrolled or picked by branch): no relative addressing.
#include "vf_common.hlsli"
#include "vf_density_variation.hlsli"
#include "vf_authored_noise.hlsli"
#include "vf_shadow.hlsli"

#ifndef STEPS
#define STEPS 24
#endif

#ifndef LOCAL_LIGHTS
#define LOCAL_LIGHTS 1
#endif

static const int kFogLayers = 4;
static const int kRegistersPerLayer = 6;
static const float kGoldenRatioFraction = 0.618034;
static const bool kMarchesLocalLights = LOCAL_LIGHTS;
// As coa: the layers are unrolled without local lights and looped with them (the lit march would not fit
// ps_3_0's 32 temps unrolled). The looped path picks each layer's registers by branch, never by index.
static const bool kUnrollsLayers = !kMarchesLocalLights;

float4 cLight : register(c9);
float4 cMarch : register(c11);
float4 cLayers[kFogLayers * kRegistersPerLayer] : register(c12);

sampler2D sDepth : register(s0);

struct FogLayer
{
    float start;
    float density;
    float g;
    float isotropic;
    float3 emissive;
    float strength;
    float3 diffuse;
    float exponent;
    float upperHeight;
    float upperFalloff;
    float lowerHeight;
    float lowerFalloff;
    float3 shadowEmissive;
    float shadowDensity;
    float shadowed;
    float skyFalloff;
    float limit;
    float densityVariation;
};

FogLayer LoadConstantFogLayer(int index)
{
    int first = index * kRegistersPerLayer;
    FogLayer layer;
    layer.start = cLayers[first].x;
    layer.density = cLayers[first].y;
    layer.g = cLayers[first].z;
    layer.isotropic = cLayers[first].w;
    layer.emissive = cLayers[first + 1].rgb;
    layer.strength = cLayers[first + 1].w;
    layer.diffuse = cLayers[first + 2].rgb;
    layer.exponent = cLayers[first + 2].w;
    layer.upperHeight = cLayers[first + 3].x;
    layer.upperFalloff = cLayers[first + 3].y;
    layer.lowerHeight = cLayers[first + 3].z;
    layer.lowerFalloff = cLayers[first + 3].w;
    layer.shadowEmissive = cLayers[first + 4].rgb;
    layer.shadowDensity = cLayers[first + 4].w;
    layer.shadowed = cLayers[first + 5].x;
    layer.skyFalloff = cLayers[first + 5].y;
    layer.limit = cLayers[first + 5].z;
    layer.densityVariation = cLayers[first + 5].w;
    return layer;
}

FogLayer LoadFogLayer(int index)
{
    [branch] if (index == 0)
        return LoadConstantFogLayer(0);
    [branch] if (index == 1)
        return LoadConstantFogLayer(1);
    [branch] if (index == 2)
        return LoadConstantFogLayer(2);
    return LoadConstantFogLayer(3);
}

float3 DirectionToLightView()
{
    return cLight.xyz;
}

float LightAboveHorizon()
{
    return cLight.w;
}

bool JitterEnabled()
{
    return cMarch.x > 0;
}

float DistanceCurveRange()
{
    return cMarch.y;
}

float HorizonBlendStart()
{
    return cMarch.z;
}

float FarClip()
{
    return cMarch.w;
}

float PhaseHG(float g, float cosAngle)
{
    float r = (1 - g) / sqrt(max(1 + g * g - 2 * g * cosAngle, 1e-6));
    return r * r * r;
}

float DistanceCurve(FogLayer layer, float sampleDistance)
{
    return 1 + layer.strength * pow(saturate(max(sampleDistance - layer.start, 0) / DistanceCurveRange()) + 1e-6,
                                    layer.exponent);
}

float HeightProfile(FogLayer layer, float height)
{
    return saturate(exp((layer.upperHeight - height) * layer.upperFalloff)) *
           saturate(exp((height - layer.lowerHeight) * layer.lowerFalloff));
}

float HorizonShadow(FogLayer layer)
{
    return layer.shadowed * (1 - LightAboveHorizon());
}

float ShadowDensityScale(FogLayer layer)
{
    return lerp(1, layer.shadowDensity, HorizonShadow(layer));
}

// skyUpward = the ray's upward component on sky pixels, 0 elsewhere (exp(0) = 1, coa's non-sky scale): one
// float live through the march instead of coa's skyMask and upward (ps_3_0 temps).
float SkyDensityScale(FogLayer layer, float skyUpward)
{
    return exp(-skyUpward * layer.skyFalloff);
}

float LayerPhase(FogLayer layer, float cosAngle)
{
    return lerp(PhaseHG(layer.g, cosAngle), 1, layer.isotropic);
}

float2 LayerRayTerms(FogLayer layer, float cosToLight, float skyUpward)
{
    return float2(layer.density * SkyDensityScale(layer, skyUpward),
                  (1 - HorizonShadow(layer)) * LayerPhase(layer, cosToLight));
}

struct MarchRay
{
    float3 viewDirection;
    float3 directionWorld;
    float cameraHeight;
    float heightPerYard;
};

float DensityVariationAlongRay(MarchRay ray, float distanceAlongRay)
{
    return DensityVariation(CameraPositionWorld() + ray.directionWorld * distanceAlongRay);
}

float WeightedVariation(float variation, float weight)
{
    return kSamplesAuthoredNoise ? lerp(1, variation, weight) : variation;
}

float DensityProfile(FogLayer layer, MarchRay ray, float distanceAlongRay, float variation)
{
    float density = DistanceCurve(layer, distanceAlongRay) *
                    HeightProfile(layer, ray.cameraHeight + ray.heightPerYard * distanceAlongRay);
    return layer.densityVariation > 0 ? density * WeightedVariation(variation, layer.densityVariation) : density;
}

#include "vf_local_lights.hlsli"

// The four layers' weighted density variations for one step (coa: per layer inside AccumulateLayer, the step's
// own sample reused when a layer samples at the same distance). Gathered here in one [loop] with a single
// atlas sample site: inlined per unrolled layer the atlas fetches ran fxc past ps_3_0's 32 temps. A layer
// that samples where the previous one did reuses its value, which covers coa's reuse. Same values as coa.
float4 LayerVariations(MarchRay ray, float stepStart, float stepEnd, float jitter)
{
    float4 variations = 1;
    [branch] if (!DensityVariationEnabled())
        return variations;
    float lastDistance = -1;
    float variation = 1;
    [loop] for (int j = 0; j < kFogLayers; j++)
    {
        // Picked by branch, not gathered into float4s: fxc hoists loop-invariant gathers out of the step loop,
        // and four float4s held through the lit march's light loops do not fit.
        FogLayer layer = LoadFogLayer(j);
        float layerStart = max(stepStart, layer.start);
        float layerLength = max(min(stepEnd, layer.limit) - layerStart, 0);
        [branch] if (layer.densityVariation > 0 && layerLength > 0 && layer.density > 0)
        {
            float distance = layerStart + layerLength * jitter;
            [branch] if (distance != lastDistance)
                variation = DensityVariationAlongRay(ray, distance);
            lastDistance = distance;
            variations = j == int4(0, 1, 2, 3) ? WeightedVariation(variation, layer.densityVariation) : variations;
        }
    }
    return variations;
}

// One layer's weighted variation, for the looped (lit) layer path: inside its [loop] this is one sample site
// with one float live, where the float4 of LayerVariations would stay live beside the noise densities.
float LayerVariation(FogLayer layer, MarchRay ray, float stepStart, float stepEnd, float jitter)
{
    float layerStart = max(stepStart, layer.start);
    float layerLength = max(min(stepEnd, layer.limit) - layerStart, 0);
    float variation = 1;
    [branch] if (DensityVariationEnabled() && layer.densityVariation > 0 && layerLength > 0 && layer.density > 0)
        variation = WeightedVariation(DensityVariationAlongRay(ray, layerStart + layerLength * jitter),
                                      layer.densityVariation);
    return variation;
}

void AccumulateLayer(FogLayer layer, int layerIndex, float noiseDensity, float2 rayTerms, float stepStart,
                     float stepEnd, float jitter, MarchRay ray, float variation, float sun, inout float3 radiance,
                     inout float opticalDepth)
{
    float layerStart = max(stepStart, layer.start);
    float layerLength = max(min(stepEnd, layer.limit) - layerStart, 0);
    [branch] if (layerLength <= 0 || layer.density <= 0)
        return;
    float shadow = HorizonShadow(layer);
    float shadowDensityScale = ShadowDensityScale(layer);
    float sampleDistance = layerStart + layerLength * jitter;
    float sampleHeight = ray.cameraHeight + ray.heightPerYard * sampleDistance;
    float distanceCurve = DistanceCurve(layer, sampleDistance);
    float heightProfile = HeightProfile(layer, sampleHeight);
    float layerOpticalDepth = rayTerms.x * layerLength * distanceCurve * heightProfile * shadowDensityScale *
                              variation * noiseDensity;
    float3 emissive = lerp(layer.emissive, layer.shadowEmissive, shadow);
    [flatten] if (kSamplesAuthoredNoise && layerIndex < kNoisyLayers)
        emissive = lerp(emissive, LayerNoiseFade(layerIndex), 1 - noiseDensity);
    // sun = the sun's visibility here (vf_shadow.hlsli, 1 unshadowed): the sun term and, by vfogCanopy, the glow
    radiance += (layer.diffuse * (rayTerms.y * sun) + emissive * CanopyScale(sun)) * layerOpticalDepth;
    opticalDepth += layerOpticalDepth;
}

float3 StepSamplePosition(MarchRay ray, float stepStart, float stepEnd, float jitter)
{
    return CameraPositionWorld() + ray.directionWorld * (stepStart + (stepEnd - stepStart) * jitter);
}

// rayTerms: [0] = the four layers' ray densities, [1] = their phase terms (LayerRayTerms), hoisted out of the
// step loop by the unrolled path only. The looped (lit) path takes them per layer: eight floats live across
// the whole step loop would push the lit noisy march past ps_3_0's 32 temps.
void AccumulateLayers(float4 noiseDensities, float4 rayTerms[2], float cosToLight, float skyUpward, float stepStart,
                      float stepEnd, float jitter, MarchRay ray, float sun, inout float3 radiance,
                      inout float opticalDepth)
{
    [branch] if (kUnrollsLayers)
    {
        float4 variations = LayerVariations(ray, stepStart, stepEnd, jitter);
        [unroll] for (int j = 0; j < kFogLayers; j++)
            AccumulateLayer(LoadConstantFogLayer(j), j, noiseDensities[j], float2(rayTerms[0][j], rayTerms[1][j]),
                            stepStart, stepEnd, jitter, ray, variations[j], sun, radiance, opticalDepth);
    }
    else
    {
        [loop] for (int j = 0; j < kFogLayers; j++)
        {
            FogLayer layer = LoadFogLayer(j);
            float4 selector = j == int4(0, 1, 2, 3) ? 1 : 0;
            float noiseDensity = kSamplesAuthoredNoise ? dot(noiseDensities, selector) : 1;
            AccumulateLayer(layer, j, noiseDensity, LayerRayTerms(layer, cosToLight, skyUpward), stepStart,
                            stepEnd, jitter, ray, LayerVariation(layer, ray, stepStart, stepEnd, jitter), sun,
                            radiance, opticalDepth);
        }
    }
}

float LayerMarchEnd(FogLayer layer)
{
    return layer.density > 0 ? layer.limit : 0;
}

float LayersMarchEnd()
{
    return max(max(LayerMarchEnd(LoadConstantFogLayer(0)), LayerMarchEnd(LoadConstantFogLayer(1))),
               max(LayerMarchEnd(LoadConstantFogLayer(2)), LayerMarchEnd(LoadConstantFogLayer(3))));
}

float LayerMarchStart(FogLayer layer)
{
    return layer.density > 0 ? layer.start : 1e30;
}

float LayersMarchStart()
{
    return min(min(LayerMarchStart(LoadConstantFogLayer(0)), LayerMarchStart(LoadConstantFogLayer(1))),
               min(LayerMarchStart(LoadConstantFogLayer(2)), LayerMarchStart(LoadConstantFogLayer(3))));
}

// vfogTemporal 0 keeps coa's fixed mid-step sample, except with the shadowed sun: its on/off compares at fixed
// distances along every ray turn the shadow volumes into shells round the camera, scaled by each pixel's depth (a
// warped copy of the background in the fog, fixed to the screen). There the step is dithered in space alone, a
// static noise the upsample blurs, instead of the frame-animated jitter only the temporal filter averages.
float StepJitter(float2 lowResTexel)
{
    // cMarch.x: 1 = animated (vfogTemporal), 0.5 = spatial only (temporal off, shadowed sun), 0 = none.
    return JitterEnabled() ? frac(InterleavedGradientNoise(lowResTexel) + FrameIndex() * kGoldenRatioFraction * step(1, cMarch.x)) : 0.5;
}

float4 IntegrateFogAtDepth(float2 pixel, float depth, float jitter)
{
    float3 viewRay = ViewRayAtUnitDepth(pixel);
    float distancePerViewZ = length(viewRay);
    float3 viewDirection = viewRay / distancePerViewZ;
    float skyMask = IsSky(depth) ? 1 : 0;
    float viewZ = LinearDepth(depth);
    float horizonBlend = max(BeyondFarClip(depth) ? 1 : 0, smoothstep(HorizonBlendStart(), FarClip(), viewZ));
    viewZ = lerp(viewZ, MaxFogDistance(), horizonBlend);
    float marchLength = min(viewZ * distancePerViewZ, MaxFogDistance());
    [branch] if (marchLength <= LayersMarchStart())
        return 0;

    float3 cameraWorld = CameraPositionWorld();
    float3 directionWorld = ViewToWorldDirection(viewDirection);
    float cosToLight = dot(DirectionToLightView(), viewDirection);
    float upward = max(directionWorld.z, 0);
    float riseLevelledAtHorizon = lerp(directionWorld.z, upward, horizonBlend);
    float skyUpward = skyMask * upward;
    MarchRay ray = {viewDirection, directionWorld, cameraWorld.z, riseLevelledAtHorizon};
    ChordCoverage lightCoverage = {0, false, 0, LocalLightCount()};

    float4 layerTerms[2] = {(float4)0, (float4)0};
    [unroll] for (int j = 0; kUnrollsLayers && j < kFogLayers; j++)
    {
        float2 terms = LayerRayTerms(LoadConstantFogLayer(j), cosToLight, skyUpward);
        layerTerms[0][j] = terms.x;
        layerTerms[1][j] = terms.y;
    }
    float layersEnd = LayersMarchEnd();
    float3 inScatteredRadiance = 0;
    float transmittance = 1;
    const float stepFraction = 1.0 / STEPS;
    [loop] for (int s = 0; s < STEPS; s++)
    {
        float startFraction = s * stepFraction;
        float endFraction = startFraction + stepFraction;
        float stepStart = marchLength * startFraction * startFraction;
        [branch] if (kUnrollsLayers && stepStart >= layersEnd)
            break;
        float stepEnd = marchLength * endFraction * endFraction;
        float3 stepRadiance = 0;
        float stepOpticalDepth = 0;
        // Before the noise: its four densities are not live yet while the shadow taps run (ps_3_0 temps).
        float sun = SunVisibility(ray.viewDirection * (stepStart + (stepEnd - stepStart) * jitter), jitter);
        float4 noiseDensities = LayerNoiseDensities(StepSamplePosition(ray, stepStart, stepEnd, jitter));
        AccumulateLayers(noiseDensities, layerTerms, cosToLight, skyUpward, stepStart, stepEnd, jitter, ray, sun,
                         stepRadiance, stepOpticalDepth);
        [branch] if (kMarchesLocalLights && lightCoverage.nextEndpoint <= stepStart)
            lightCoverage = ChordCoverageFrom(lightCoverage, viewDirection, stepStart, marchLength);
        [branch] if (kMarchesLocalLights && StepMeetsLocalLights(lightCoverage, stepEnd))
            stepRadiance += LocalLightScattering(lightCoverage, LayerLightDensityScales(skyUpward), ray, stepStart,
                                                 stepEnd, noiseDensities * LayerVariations(ray, stepStart, stepEnd,
                                                                                           jitter));
        [branch] if (stepOpticalDepth > 0)
        {
            float stepOpacity = stepOpticalDepth < 1e-3
                ? stepOpticalDepth * (1 - 0.5 * stepOpticalDepth + stepOpticalDepth * stepOpticalDepth / 6)
                : 1 - exp(-stepOpticalDepth);
            inScatteredRadiance += transmittance * stepRadiance * (stepOpacity / stepOpticalDepth);
            transmittance *= 1 - stepOpacity;
        }
    }
    return float4(clamp(inScatteredRadiance, 0, 65504), 1 - transmittance);
}

float4 IntegrateFogAtPixel(float2 pixel, float jitter)
{
    return IntegrateFogAtDepth(pixel, SampleDepth(sDepth, pixel), jitter);
}
