// VFogComposite{Q}[L][N] (ps_3_0, full res over the world viewport): port of coa-vfog shaders/vf_composite.hlsl.
// Defines as VFogMarch. TEXCOORD0 = world-viewport uv instead of VPOS (vf_common.hlsli). Samplers:
// s0 scene depth, s1 fog (temporal history or the march), s3 scene copy (blend modes 1/3 only),
// s4 current march (silhouette fallback). Draw with GxBlend_Alpha, colour write RGB, no depth.
// As coa (renderer.cpp CompositeShader: lit wins over noisy), the lit composite's full-res fallback march has
// no authored noise; VFogComposite{Q}LN keeps the density variation. A lit noisy fallback does not fit ps_3_0.
#if LOCAL_LIGHTS
#define VF_AUTHORED_NOISE 0
#endif
// The full-res fallback's shadowed sun (vf_shadow.hlsli): one tap, none in the lit noisy composite (ps_3_0 temps).
#if LOCAL_LIGHTS && VF_NOISE
#define VF_SHADOW_TAPS 0
#else
#define VF_SHADOW_TAPS 1
#endif
// SPLIT: VFogCompositeSplit{Q}[L][N], coa vf_composite_split.hlsl: drawn over the stencil-marked near (c80.x = 0) or
// far (c80.x = 1) samples of an MSAA silhouette (VFogSilhouette), fogged at that side's depth; c80.y = 1: the pixel's own
// side was already drawn by the plain composite, so it is clipped here.
#if SPLIT
#include "vf_sample_split.hlsli"
float4 cSampleSide : register(c80);
#else
#include "vf_composite.hlsli"
#endif

float4 main(float2 targetUv : TEXCOORD0) : COLOR0
{
    float2 pixel = FullPixelFromUv(targetUv);
#if SPLIT
    DepthNeighbourhood n = NeighbourhoodOf(pixel);
    const bool farSamples = cSampleSide.x > 0.5;
    const bool ownSide = (n.own > SplitDepth(n)) == farSamples;
    [branch] if (ownSide && cSampleSide.y > 0.5)
    {
        clip(-1);
        return 0;
    }
    float depth = ownSide ? n.own : (farSamples ? n.farthest : n.nearest);
#else
    float depth = SampleDepth(sDepth, pixel);
#endif
    return Dithered(CompositeAtDepth(pixel, depth), pixel, float2(211, 97), FrameIndex());
}
