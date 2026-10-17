// VFogWaterDepth / VFogWaterMask (ps_3_0, world viewport sized R32F target): coa-vfog shaders/vw_linear_depth.hlsl
// (@ 4e31ddf, GPLv3), r32f path, TEXCOORD0 = target uv instead of VPOS. s0 = the scene depth (DeviceCopySceneDepth).
// MASK (docs/VFOG_PORT.md "Water", mask scheme): s1 = the previous copy (abs = its view Z); the output is the view Z
// where the depth moved closer since that copy (this water class drew the front surface there), else minus it.
#include "vf_common.hlsli"

sampler2D sDepth : register(s0);
#if MASK
sampler2D sPrevious : register(s1);
#endif

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float viewZ = LinearDepth(SampleDepth(sDepth, FullPixelFromUv(uv)));
#if MASK
    float previous = abs(tex2Dlod(sPrevious, float4(uv, 0, 0)).r);
    return viewZ < previous ? viewZ : -viewZ;
#else
    return viewZ;
#endif
}
