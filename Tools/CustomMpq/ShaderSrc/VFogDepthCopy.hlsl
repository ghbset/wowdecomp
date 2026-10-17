// VFogDepthCopy (ps_3_0, R32F target the size of the scene depth): wxl-vfog 0.6.3 KeepDepthBehind's depth copy.
// s0 = the scene depth (DeviceCopySceneDepth), point sampled; TEXCOORD0 = uv across the target at texel centres.
// vfogFadeStencil (docs/VFOG_PORT.md section 1, fading doodads): the depth behind the fading models, and with them.
sampler2D sDepth : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    return tex2Dlod(sDepth, float4(uv, 0, 0)).r;
}
