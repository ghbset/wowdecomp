// VFogWater: coa-vfog shaders/vw_surface.hlsli (@ 4e31ddf, GPLv3) with the moments rebuilt from the CPU wave textures
// (SampleMoments), R32F depth copies and no ripples.
static const float kPi = 3.14159265;
static const float kShallowTileAmplitude = 800;
static const float kMaxTanhArgument = 10;
static const float kAdtGridOrigin = 17066.666;
static const float kAdtTilesPerYard = 0.001875;
static const float kSlopeToAdtUv = 0.001;
static const float kMinFoamFade = 1e-4;
static const float kMinRayRise = 1e-4;
static const float kRippleSlopeScale = 3;
static const float kRippleFullDetailTexels = 2;
static const float kRippleDetailFadeTexels = 2;

struct WaterPixel
{
    float2 uv;
    float waterZ;
    float sceneZ;
    float3 position;
    float3 toCamera;
    float cameraDistance;
    float columnDepth;
    float2 footprintX;
    float2 footprintY;
};

struct WaveState
{
    float4 moments;
    float2 foam;
};

struct RippleSample
{
    float height;
    float2 slope;
    float coverage;
};

float2 CopyUv(float2 pixel)
{
    return (pixel - ViewportOrigin()) / ViewportSize();
}

// R32F copies (coa's r32f path; no packed rgba8 fallback). sWaterDepth is signed: abs() is the view Z.
float CopiedViewDepth(sampler2D copy, float2 uv)
{
    return abs(tex2Dlod(copy, float4(uv, 0, 0)).r);
}

float3 GammaToLinear(float3 colour)
{
    return pow(max(colour, 0), 2.2);
}

float3 LinearToGamma(float3 colour)
{
    return pow(saturate(colour), 1 / 2.2);
}

float2 PlaneFootprint(float3 ray, float3 rayStep, float viewZ)
{
    float rise = ray.z < 0 ? min(ray.z, -kMinRayRise) : max(ray.z, kMinRayRise);
    return viewZ * (rayStep.xy - ray.xy * rayStep.z / rise);
}

WaterPixel ReconstructWaterPixel(float2 pixel)
{
    WaterPixel w;
    w.uv = CopyUv(pixel);
    w.waterZ = CopiedViewDepth(sWaterDepth, w.uv);
    w.sceneZ = max(CopiedViewDepth(sSceneDepth, w.uv), w.waterZ);
    float3 worldRay = ViewToWorldDirection(ViewRayAtUnitDepth(pixel));
    w.position = CameraPositionWorld() + worldRay * w.waterZ;
    w.toCamera = normalize(-worldRay);
    w.cameraDistance = w.waterZ * length(worldRay);
    w.columnDepth = max(0, (w.sceneZ - w.waterZ) * -worldRay.z);
    float3 rayStepX = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(1, 0))) - worldRay;
    float3 rayStepY = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(0, 1))) - worldRay;
    w.footprintX = PlaneFootprint(worldRay, rayStepX, w.waterZ);
    w.footprintY = PlaneFootprint(worldRay, rayStepY, w.waterZ);
    return w;
}

float4 SampleTile(sampler2D tile, WaterPixel w, float inverseSize)
{
    return tex2Dgrad(tile, w.position.xy * inverseSize, w.footprintX * inverseSize, w.footprintY * inverseSize);
}

// coa's float4 moments (slope.xy, slope^2, wide slope^2) from the two CPU textures: slope from the Gr1616F tile,
// slope^2 = variance + slope^2 of the filtered mean (the variance itself is stored, so the mean's own square never
// meets an 8-bit copy of it), wide slope^2 decoded from its sqrt. kVarianceRange / kWideRange match
// VolumetricFogWaterWaves.cpp.
static const float kVarianceRange = 0.25;
static const float kWideRange = 1;

float4 SampleMoments(sampler2D slopes, sampler2D state, WaterPixel w, float inverseSize)
{
    float2 slope = SampleTile(slopes, w, inverseSize).xy;
    float2 spread = SampleTile(state, w, inverseSize).xy;
    spread *= spread;
    return float4(slope, spread.x * kVarianceRange + dot(slope, slope), spread.y * kWideRange);
}

float2 SampleFoamState(sampler2D state, WaterPixel w, float inverseSize)
{
    return tex2Dlod(state, float4(w.position.xy * inverseSize, 0, 0)).zw;
}

float Tanh(float x)
{
    float e = exp(2 * min(x, kMaxTanhArgument));
    return (e - 1) / (e + 1);
}

float ShallowTileWeight(float inverseTileSize, float columnDepth)
{
    float shallowAmplitude = kShallowTileAmplitude * inverseTileSize * inverseTileSize;
    return saturate(lerp(shallowAmplitude, 1, Tanh(4 * kPi * columnDepth * inverseTileSize)));
}

// Ripples are not ported (docs "Water", left): no wakes, the coverage is zero.
RippleSample SampleRipples(WaterPixel w)
{
    return (RippleSample)0;
}

WaveState SampleWaves(WaterPixel w, float2 rippleSlope)
{
    float4 inverseSize = cInverseTileSizes;
    float4 weight = float4(ShallowTileWeight(inverseSize.x, w.columnDepth),
                           ShallowTileWeight(inverseSize.y, w.columnDepth),
                           ShallowTileWeight(inverseSize.z, w.columnDepth),
                           ShallowTileWeight(inverseSize.w, w.columnDepth));
    WaveState waves;
    waves.moments = weight.x * SampleMoments(sSurface0, sFoamState0, w, inverseSize.x) +
                    weight.y * SampleMoments(sSurface1, sFoamState1, w, inverseSize.y) +
                    weight.z * SampleMoments(sSurface2, sFoamState2, w, inverseSize.z) +
                    weight.w * SampleMoments(sSurface3, sFoamState3, w, inverseSize.w);
    waves.moments.xy += rippleSlope;
    waves.foam = weight.x * SampleFoamState(sFoamState0, w, inverseSize.x) +
                 weight.y * SampleFoamState(sFoamState1, w, inverseSize.y) +
                 weight.z * SampleFoamState(sFoamState2, w, inverseSize.z) +
                 weight.w * SampleFoamState(sFoamState3, w, inverseSize.w);
    return waves;
}

float SlopeVariance(float4 moments)
{
    float2 meanSlope = moments.xy * InverseTileCount();
    return 0.5 * max(0, moments.z * InverseTileCount() - dot(meanSlope, meanSlope));
}

float CrestTilt(float4 moments)
{
    float coarseSlope = moments.w * InverseTileCount();
    return sqrt(coarseSlope / (coarseSlope + 1));
}

float2 AdtUv(float3 position, float2 slope)
{
    return (kAdtGridOrigin - position.yx) * kAdtTilesPerYard + slope * kSlopeToAdtUv;
}

float4 TintedFoam(int slot, float maskCoverage)
{
    float coverage = maskCoverage * MaskPresent(slot);
    return float4(lerp(MaskTintLow(slot), MaskTintHigh(slot), coverage), coverage);
}

float4 FoamLayer(sampler2D mask, int slot, float2 uv, float2 dx, float2 dy)
{
    return TintedFoam(slot, tex2Dgrad(mask, uv, dx, dy).r);
}

float FoamFade(float distance, float fadeDistance)
{
    float t = saturate(1 - distance / max(fadeDistance, kMinFoamFade));
    float eased = t * t * (3 - 2 * t);
    eased *= eased;
    return eased * eased;
}

float4 WaveFoam(float2 adtUv, float2 dx, float2 dy, float f)
{
    float3 scale = cWaveFoamScaling.xyz;
    float4 high = TintedFoam(kHighFoamSlot, tex2Dgrad(sWaveFoamMasks, adtUv * scale.x + cFoamScroll.xy, dx * scale.x,
                                                      dy * scale.x).r);
    float4 mid = TintedFoam(kMidFoamSlot, tex2Dgrad(sWaveFoamMasks, adtUv * scale.y + cFoamScroll.xy, dx * scale.y,
                                                    dy * scale.y).g);
    float4 low = TintedFoam(kLowFoamSlot, tex2Dgrad(sWaveFoamMasks, adtUv * scale.z + cFoamScroll.xy, dx * scale.z,
                                                    dy * scale.z).b);
    float rootF = sqrt(f);
    return saturate(low * rootF + mid * f * rootF + high * pow(f, 4.5)) * WaveFoamIntensity();
}

float4 FoamAlbedo(WaterPixel w, WaveState waves)
{
    float2 adtUv = AdtUv(w.position, waves.moments.xy);
    float2 dx = -w.footprintX.yx * kAdtTilesPerYard;
    float2 dy = -w.footprintY.yx * kAdtTilesPerYard;
    float f = max(waves.foam.x, 0);
    float shoreWeight = cShoreFoam.x * FoamFade(w.columnDepth * ShoreDistancePerDepth(), cShoreFoam.z);
    float depthWeight = cDepthFadeFoam.x * FoamFade(w.sceneZ - w.waterZ, cDepthFadeFoam.z);
    float4 foam = 0;
    [branch] if (f * WaveFoamIntensity() > 0)
        foam = WaveFoam(adtUv, dx, dy, f);
    [branch] if (shoreWeight > 0)
        foam += shoreWeight * FoamLayer(sShoreFoamMask, kShoreFoamSlot, adtUv * cShoreFoam.y + cFoamScroll.zw,
                                        dx * cShoreFoam.y, dy * cShoreFoam.y);
    [branch] if (depthWeight > 0)
        foam += depthWeight * FoamLayer(sDepthFoamMask, kDepthFoamSlot, adtUv * cDepthFadeFoam.y + cDepthFoamScroll.xy,
                                        dx * cDepthFadeFoam.y, dy * cDepthFadeFoam.y);
    return saturate(foam);
}
