// VFogSilhouette (ps_3_0, world viewport, D3D9/D3D11 only): coa-vfog shaders/vf_silhouette_mask.hlsl (@ 4e31ddf).
// Writes no colour; oDepth = the middle depth of a silhouette's neighbourhood, so the depth test against the
// multisampled depth splits its samples: far ones pass (stencil INVERT -> 0xFF), near ones fail (REPLACE -> 1).
// Elsewhere clipped (stencil stays 0). oDepth is why Vulkan never loads it (docs/VFOG_PORT.md section 3, MSAA).
#include "vf_sample_split.hlsli"

struct SilhouetteSplit
{
    float4 colour : COLOR0;
    float depth : DEPTH;
};

SilhouetteSplit main(float2 uv : TEXCOORD0)
{
    DepthNeighbourhood n = NeighbourhoodOf(FullPixelFromUv(uv));
    clip(SilhouetteWithin(n) ? 1 : -1);
    SilhouetteSplit output;
    output.colour = 0;
    output.depth = SplitDepth(n);
    return output;
}
