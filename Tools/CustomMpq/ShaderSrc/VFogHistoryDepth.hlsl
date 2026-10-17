// VFogHistoryDepth (ps_3_0, low res): port of coa-vfog shaders/vf_history_depth.hlsl. Packs the low-res
// texel's view Z (16 bits over MaxFogDistance, r = high byte, g = low byte) and its depth class (b: 0 world,
// 0.5 beyond the far clip, 1 sky) for the next frame's temporal pass. TEXCOORD0 = low-res target uv.
#include "vf_common.hlsli"

sampler2D sDepth : register(s0);

float4 main(float2 targetUv : TEXCOORD0) : COLOR0
{
    float depth = SampleDepth(sDepth, LowResTexelToFullPixel(LowResTexelFromUv(targetUv)));
    float packed = floor(saturate(LinearDepth(depth) / MaxFogDistance()) * 65535 + 0.5);
    float highByte = floor(packed / 256);
    float lowByte = packed - highByte * 256;
    float depthClass = IsSky(depth) ? 1 : (BeyondFarClip(depth) ? 0.5 : 0);
    return float4(highByte / 255, lowByte / 255, depthClass, 1);
}
