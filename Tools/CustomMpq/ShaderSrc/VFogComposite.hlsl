// VFogComposite{Q}[L][N] (ps_3_0, full res over the world viewport): port of coa-vfog shaders/vf_composite.hlsl.
// Defines as VFogMarch. TEXCOORD0 = world-viewport uv instead of VPOS (vf_common.hlsli). Samplers:
// s0 scene depth, s1 fog (temporal history or the march), s3 scene copy (blend modes 1/3 only),
// s4 current march (silhouette fallback). Draw with GxBlend_Alpha, colour write RGB, no depth.
// As coa (renderer.cpp CompositeShader: lit wins over noisy), the lit composite's full-res fallback march has
// no authored noise; VFogComposite{Q}LN keeps the density variation. A lit noisy fallback does not fit ps_3_0.
#if LOCAL_LIGHTS
#define VF_AUTHORED_NOISE 0
#endif
#include "vf_composite.hlsli"

float4 main(float2 targetUv : TEXCOORD0) : COLOR0
{
    float2 pixel = FullPixelFromUv(targetUv);
    return CompositeAtDepth(pixel, SampleDepth(sDepth, pixel));
}
