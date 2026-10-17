// VFogRippleStep (ps_3_0, into the 256^2 / 512^2 Gr1616F ripple map): port of coa-vfog shaders/vw_ripple_step.hlsl
// (@ 4e31ddf, GPLv3). One 30 Hz step of the height field (r = height, g = previous height) around the camera target,
// shifted with the window, plus up to 48 unit footprints / splashes. TEXCOORD0 = target uv instead of VPOS.
// c97.x (new): 0 clears the map instead (coa ClearMaps; the Gx clear is viewport-dependent across backends).
float4 cRippleStep : register(c0);
float4 cRippleSegments[48] : register(c1);
float4 cRippleShapes[48] : register(c49);
float4 cRippleKeep : register(c97);

sampler2D sRippleState : register(s0);

static const int kMaxDisturbances = 48;
static const float kDamping = 0.97;
static const float kEdgeRampPerUv = 16;
static const float kNeighbourWeight = 0.5;
static const float kFootprintCore = 0.20;
static const float kFootprintRim = 0.79;

float2 WindowShift()
{
    return cRippleStep.xy;
}

float InverseTexels()
{
    return cRippleStep.z;
}

float DisturbanceCount()
{
    return cRippleStep.w;
}

float2 State(float2 uv)
{
    return tex2Dlod(sRippleState, float4(uv, 0, 0)).rg;
}

float EdgeRamp(float2 uv)
{
    float2 inward = min(uv, 1 - uv) * kEdgeRampPerUv;
    return saturate(min(inward.x, inward.y));
}

float2 Propagate(float2 uv)
{
    float texel = InverseTexels();
    float2 centre = State(uv);
    float left = State(uv - float2(texel, 0)).r;
    float down = State(uv - float2(0, texel)).r;
    float right = State(uv + float2(texel, 0)).r;
    float up = State(uv + float2(0, texel)).r;
    float next = kDamping * EdgeRamp(uv) * (kNeighbourWeight * (left + down + right + up) - centre.g);
    return float2(next, centre.r);
}

float SegmentDistance(float2 p, float2 from, float2 to)
{
    float2 along = to - from;
    float lengthSquared = dot(along, along);
    float t = lengthSquared > 0 ? saturate(dot(p - from, along) / lengthSquared) : 0;
    return length(p - from - along * t);
}

float Footprint(float distance, float inverseRadius)
{
    return 1 - smoothstep(kFootprintCore, kFootprintRim, distance * inverseRadius);
}

float2 Disturb(float2 state, float2 texelCentre, float4 segment, float4 shape)
{
    float weight = Footprint(SegmentDistance(texelCentre, segment.xy, segment.zw), shape.x);
    float held = weight * shape.z;
    float height = lerp(state.x, shape.y, held) + shape.y * (weight - held);
    return float2(height, lerp(state.y, height, held));
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 texel = floor(uv / InverseTexels());
    float2 state = 0;
    [branch] if (cRippleKeep.x > 0)
        state = Propagate((texel + WindowShift() + 0.5) * InverseTexels());
    [unroll] for (int i = 0; i < kMaxDisturbances; i++)
    {
        [branch] if (i < DisturbanceCount())
            state = Disturb(state, texel + 0.5, cRippleSegments[i], cRippleShapes[i]);
    }
    return float4(state, 0, 0);
}
