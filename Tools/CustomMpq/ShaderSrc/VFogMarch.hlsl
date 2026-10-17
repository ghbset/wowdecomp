// VFogMarch{Q}[L][N] (ps_3_0, low res): port of coa-vfog shaders/vf_march.hlsl. build_shaders.py defines
// STEPS (16/24/32), LOCAL_LIGHTS (0/1) and VF_NOISE (0/1) per variant. Output: premultiplied
// (in-scattered radiance, opacity). TEXCOORD0 = low-res target uv instead of VPOS (vf_common.hlsli).
#include "vf_integrate.hlsli"

float4 main(float2 targetUv : TEXCOORD0) : COLOR0
{
    float2 lowResTexel = LowResTexelFromUv(targetUv);
    return Dithered(IntegrateFogAtPixel(LowResTexelToFullPixel(lowResTexel), StepJitter(lowResTexel)), lowResTexel,
                    float2(0, 0), FrameIndex());
}
