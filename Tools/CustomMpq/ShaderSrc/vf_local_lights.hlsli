// Port of coa-vfog shaders/vf_local_lights.hlsli. The lights come from pixel constants c100..c131 instead of
// coa's 32x1 RGBA32F texture on s8 (docs/VFOG_PORT.md 5): four registers a light,
//   +0 view-space position (camera-relative, yards) + cutoff radius, +1 colour (linear, x intensity),
//   +2 attenuation constant / linear / quadratic, +3 spare.
float4 cLocalLightControl : register(c53);
static const int kMaxLocalLights = 8;
static const int kRegistersPerLight = 4;
float4 cLocalLights[kMaxLocalLights * kRegistersPerLight] : register(c100);

static const float kFirstQuadratureFraction = 0.2113248654;
static const float kQuadratureFractionSpacing = 0.5773502692;
static const float kBeyondEveryLight = 1e30;

struct ChordCoverage
{
    float nextEndpoint;
    bool covered;
    float firstLight;
    float lightEnd;
};

struct LocalLight
{
    float3 position;
    float radius;
    float projected;
    float2 chord;
};

float LocalLightCount()
{
    return cLocalLightControl.x;
}

float LocalLightReachLimit()
{
    return cLocalLightControl.y;
}

float LocalLightPhaseG()
{
    return cLocalLightControl.z;
}

// The light index is dynamic; a select chain over literal registers keeps every constant index static
// (no relative addressing for the translators to handle).
float4 LocalLightRegister(int light, int registerInLight)
{
    float4 value = cLocalLights[registerInLight];
    [unroll] for (int i = 1; i < kMaxLocalLights; ++i)
        value = light == i ? cLocalLights[i * kRegistersPerLight + registerInLight] : value;
    return value;
}

float3 LocalLightColour(int light)
{
    return LocalLightRegister(light, 1).rgb;
}

float3 LocalLightAttenuation(int light)
{
    return LocalLightRegister(light, 2).xyz;
}

LocalLight LoadLocalLight(int index, float3 viewDirection)
{
    float4 positionRadius = LocalLightRegister(index, 0);
    LocalLight light;
    light.position = positionRadius.xyz;
    light.radius = positionRadius.w;
    light.projected = dot(light.position, viewDirection);
    float perpendicularSquared = max(dot(light.position, light.position) - light.projected * light.projected, 0);
    float discriminant = light.radius * light.radius - perpendicularSquared;
    float halfChord = sqrt(max(discriminant, 0));
    light.chord = discriminant > 0 ? float2(light.projected - halfChord, light.projected + halfChord)
                                   : float2(kBeyondEveryLight, -kBeyondEveryLight);
    return light;
}

float QuadratureFraction(int sample)
{
    return kFirstQuadratureFraction + sample * kQuadratureFractionSpacing;
}

ChordCoverage ChordCoverageFrom(ChordCoverage previous, float3 viewDirection, float distanceAlongRay,
                                float marchLength)
{
    ChordCoverage coverage;
    coverage.nextEndpoint = kBeyondEveryLight;
    coverage.covered = false;
    coverage.firstLight = previous.lightEnd;
    coverage.lightEnd = previous.firstLight;
    [loop] for (int index = previous.firstLight; index < previous.lightEnd; ++index)
    {
        float2 chord = LoadLocalLight(index, viewDirection).chord;
        float endpoint = chord.x > distanceAlongRay ? chord.x : chord.y;
        coverage.nextEndpoint = endpoint > distanceAlongRay ? min(coverage.nextEndpoint, endpoint)
                                                            : coverage.nextEndpoint;
        coverage.covered = coverage.covered || (chord.x <= distanceAlongRay && chord.y > distanceAlongRay);
        [flatten] if (chord.y > distanceAlongRay && chord.x < marchLength)
        {
            coverage.firstLight = min(coverage.firstLight, index);
            coverage.lightEnd = index + 1;
        }
    }
    return coverage;
}

bool StepMeetsLocalLights(ChordCoverage coverage, float stepEnd)
{
    return coverage.covered || coverage.nextEndpoint < stepEnd;
}

float LayerLightDensityScale(FogLayer layer, float skyUpward)
{
    return layer.density * SkyDensityScale(layer, skyUpward) * ShadowDensityScale(layer);
}

float4 LayerLightDensityScales(float skyUpward)
{
    return float4(LayerLightDensityScale(LoadConstantFogLayer(0), skyUpward),
                  LayerLightDensityScale(LoadConstantFogLayer(1), skyUpward),
                  LayerLightDensityScale(LoadConstantFogLayer(2), skyUpward),
                  LayerLightDensityScale(LoadConstantFogLayer(3), skyUpward));
}

bool LayerLitInStep(FogLayer layer, float stepStart, float stepEnd)
{
    float layerStart = max(stepStart, layer.start);
    return min(stepEnd, layer.limit) - layerStart > 0 && layer.density > 0 &&
           (LocalLightReachLimit() <= 0 || layerStart < LocalLightReachLimit());
}

bool4 LayersLitInStep(float stepStart, float stepEnd)
{
    return bool4(LayerLitInStep(LoadConstantFogLayer(0), stepStart, stepEnd),
                 LayerLitInStep(LoadConstantFogLayer(1), stepStart, stepEnd),
                 LayerLitInStep(LoadConstantFogLayer(2), stepStart, stepEnd),
                 LayerLitInStep(LoadConstantFogLayer(3), stepStart, stepEnd));
}

bool LayerCoversInterval(FogLayer layer, float2 interval)
{
    return layer.start <= interval.x && layer.limit >= interval.y;
}

bool4 LayersCoverInterval(float2 interval)
{
    return bool4(LayerCoversInterval(LoadConstantFogLayer(0), interval),
                 LayerCoversInterval(LoadConstantFogLayer(1), interval),
                 LayerCoversInterval(LoadConstantFogLayer(2), interval),
                 LayerCoversInterval(LoadConstantFogLayer(3), interval));
}

float LayerStart(int layer)
{
    return layer == 0 ? LoadConstantFogLayer(0).start
         : layer == 1 ? LoadConstantFogLayer(1).start
         : layer == 2 ? LoadConstantFogLayer(2).start : LoadConstantFogLayer(3).start;
}

float LayerLimit(int layer)
{
    return layer == 0 ? LoadConstantFogLayer(0).limit
         : layer == 1 ? LoadConstantFogLayer(1).limit
         : layer == 2 ? LoadConstantFogLayer(2).limit : LoadConstantFogLayer(3).limit;
}

float4 LayerSelector(int layer)
{
    return layer == int4(0, 1, 2, 3) ? 1 : 0;
}

float2 LayerIntervalWithin(float2 interval, int layer)
{
    return float2(max(interval.x, LayerStart(layer)), min(interval.y, LayerLimit(layer)));
}

// Deviation from coa: no density-variation fetch per quadrature sample. LocalLightScattering's layer
// densities arrive multiplied by the step's per-layer variations (LayerVariations, the values the layers
// themselves used this step) instead: with the atlas noise (two fetches a sample, coa's one tex3D) the lit
// noisy march does not fit ps_3_0's 32 temps.
float4 DensityProfiles(MarchRay ray, float distanceAlongRay)
{
    float variation = 1;
    return float4(DensityProfile(LoadConstantFogLayer(0), ray, distanceAlongRay, variation),
                  DensityProfile(LoadConstantFogLayer(1), ray, distanceAlongRay, variation),
                  DensityProfile(LoadConstantFogLayer(2), ray, distanceAlongRay, variation),
                  DensityProfile(LoadConstantFogLayer(3), ray, distanceAlongRay, variation));
}

// layerWeights = coa's layerMask * densityScales, folded once by the caller so the light loops carry one
// float4 fewer (ps_3_0 temps). A piece or step whose weights are all zero is skipped; its integral is zero.
float SampleDensity(MarchRay ray, float distanceAlongRay, float4 layerWeights)
{
    return dot(DensityProfiles(ray, distanceAlongRay), layerWeights);
}

float LightSampleScattering(LocalLight light, float3 attenuation, float3 viewDirection, float distanceAlongRay,
                            float density)
{
    float3 toLight = light.position - viewDirection * distanceAlongRay;
    float distanceSquared = dot(toLight, toLight);
    float inverseDistance = rsqrt(max(distanceSquared, 1e-6));
    float lightDistance = max(distanceSquared, 1e-6) * inverseDistance;
    float falloff = rcp(max(attenuation.x + attenuation.y * lightDistance + attenuation.z * distanceSquared, 1));
    float boundaryFade = saturate((light.radius - lightDistance) * 4 / max(light.radius, 1e-3));
    float cosAngle = (light.projected - distanceAlongRay) * inverseDistance;
    return falloff * boundaryFade * PhaseHG(LocalLightPhaseG(), cosAngle) * density;
}

// Takes the light by index and loads it after both density samples, not before (coa passed the loaded light
// in): the light and its attenuation are ten floats that would otherwise stay live across the noise fetches,
// which ran the lit noisy march past ps_3_0's 32 temps. Same sum in the same order.
float IntervalScattering(int index, float2 interval, float4 layerWeights, MarchRay ray)
{
    float first = lerp(interval.x, interval.y, QuadratureFraction(0));
    float second = lerp(interval.x, interval.y, QuadratureFraction(1));
    float atFirst = 0;
    float atSecond = 0;
    [loop] for (int sample = 0; sample < 2; ++sample)
    {
        float density = SampleDensity(ray, sample == 0 ? first : second, layerWeights);
        [flatten] if (sample == 0)
            atFirst = density;
        else
            atSecond = density;
    }
    LocalLight light = LoadLocalLight(index, ray.viewDirection);
    float3 attenuation = LocalLightAttenuation(index);
    float quadrature = LightSampleScattering(light, attenuation, ray.viewDirection, first, atFirst) +
                       LightSampleScattering(light, attenuation, ray.viewDirection, second, atSecond);
    return 0.5 * quadrature * (interval.y - interval.x);
}

float2 PieceInterval(int piece, float2 interval)
{
    return piece == 0 ? interval : LayerIntervalWithin(interval, piece - 1);
}

float4 PieceLayers(int piece, float2 interval, float2 pieceInterval, float4 litLayers)
{
    return piece == 0 ? (LayersCoverInterval(interval) ? litLayers : 0)
                      : (any(pieceInterval != interval) ? litLayers * LayerSelector(piece - 1) : 0);
}

float LightOwnIntervalsScattering(int index, float2 interval, bool wholeStep, bool layerBoundaryInStep,
                                  float4 litWeights, MarchRay ray)
{
    float scattering = 0;
    int lastPiece = layerBoundaryInStep ? kFogLayers : 0;
    [loop] for (int piece = wholeStep ? 1 : 0; piece <= lastPiece; ++piece)
    {
        float2 pieceInterval = PieceInterval(piece, interval);
        float4 pieceWeights = PieceLayers(piece, interval, pieceInterval, litWeights);
        [branch] if (pieceInterval.y > pieceInterval.x && any(pieceWeights != 0))
            scattering += IntervalScattering(index, pieceInterval, pieceWeights, ray);
    }
    return scattering;
}

bool ChordCoversStep(float2 chord, float2 step)
{
    return chord.x <= step.x && chord.y >= step.y;
}

float3 WholeStepLightScattering(ChordCoverage coverage, float4 stepWeights, MarchRay ray, float2 step)
{
    float first = lerp(step.x, step.y, QuadratureFraction(0));
    float second = lerp(step.x, step.y, QuadratureFraction(1));
    float atFirst = 0;
    float atSecond = 0;
    [loop] for (int sample = 0; sample < 2; ++sample)
    {
        float density = SampleDensity(ray, sample == 0 ? first : second, stepWeights);
        [flatten] if (sample == 0)
            atFirst = density;
        else
            atSecond = density;
    }
    float3 scattering = 0;
    [loop] for (int index = coverage.firstLight; index < coverage.lightEnd; ++index)
    {
        LocalLight light = LoadLocalLight(index, ray.viewDirection);
        [branch] if (ChordCoversStep(light.chord, step))
        {
            float3 attenuation = LocalLightAttenuation(index);
            scattering += LocalLightColour(index) *
                          (LightSampleScattering(light, attenuation, ray.viewDirection, first, atFirst) +
                           LightSampleScattering(light, attenuation, ray.viewDirection, second, atSecond));
        }
    }
    return 0.5 * (step.y - step.x) * scattering;
}

float3 LocalLightScattering(ChordCoverage coverage, float4 densityScales, MarchRay ray, float stepStart,
                            float stepEnd, float4 layerNoiseDensities)
{
    float2 step = float2(stepStart, stepEnd);
    float4 litLayers = LayersLitInStep(stepStart, stepEnd) ? layerNoiseDensities : 0;
    bool4 layersCoverStep = LayersCoverInterval(step);
    bool layerBoundaryInStep = any(layersCoverStep ? 0 : litLayers);
    float4 litWeights = litLayers * densityScales;
    bool lightCoversStep = coverage.covered;
    float3 scattering = 0;
    [branch] if (coverage.nextEndpoint < stepEnd || layerBoundaryInStep)
    {
        lightCoversStep = false;
        [loop] for (int index = coverage.firstLight; index < coverage.lightEnd; ++index)
        {
            LocalLight light = LoadLocalLight(index, ray.viewDirection);
            float2 interval = float2(max(stepStart, light.chord.x), min(stepEnd, light.chord.y));
            bool wholeStep = ChordCoversStep(light.chord, step);
            lightCoversStep = lightCoversStep || wholeStep;
            float amount = 0;
            [branch] if (interval.y > interval.x && (!wholeStep || layerBoundaryInStep))
                amount = LightOwnIntervalsScattering(index, interval, wholeStep, layerBoundaryInStep, litWeights,
                                                     ray);
            [branch] if (amount != 0)
                scattering += LocalLightColour(index) * amount;
        }
    }
    float4 stepWeights = LayersCoverInterval(step) ? litWeights : 0;    // recomputed: not live across the loop
    [branch] if (lightCoversStep && any(stepWeights != 0))
        scattering += WholeStepLightScattering(coverage, stepWeights, ray, step);
    return scattering;
}
