// VFogGrade (ps_3_0, full res over the world viewport): port of coa-vfog shaders/vp_grade.hlsl, the modern
// client's colour curve for the lights around the camera. TEXCOORD0 = world-viewport uv instead of VPOS.
// s0 a copy of the finished world viewport (after the glow). c0 world viewport px, c10.x strength,
// c11-c18 the 32-entry curve (4 entries a register) instead of coa's 32x1 R32F texture: the hat-weight sum below
// is exactly coa's lerp between the two neighbouring entries, without a float texture on three backends.
// Draw opaque, colour write RGB, no depth.
#include "vf_common.hlsli"

float4 cGrading : register(c10);
float4 cGradingCurve[8] : register(c11);

sampler2D sSceneBeforeGrading : register(s0);

static const float kCurveEntries = 32;

float GradedChannel(float value)
{
    float position = saturate(value) * (kCurveEntries - 1);
    float graded = 0;
    [unroll] for (int k = 0; k < 8; k++)
    {
        float4 weight = saturate(1 - abs(position - float4(4 * k, 4 * k + 1, 4 * k + 2, 4 * k + 3)));
        graded += dot(weight, cGradingCurve[k]);
    }
    return graded;
}

float4 main(float2 viewportUv : TEXCOORD0) : COLOR0
{
    float3 scene = tex2Dlod(sSceneBeforeGrading, float4(viewportUv, 0, 0)).rgb;
    float3 graded = float3(GradedChannel(scene.r), GradedChannel(scene.g), GradedChannel(scene.b));
    float4 colour = float4(lerp(scene, graded, cGrading.x), 1);
    return Dithered(colour, floor(viewportUv * ViewportSize()), float2(59, 173), cDither.y);
}
