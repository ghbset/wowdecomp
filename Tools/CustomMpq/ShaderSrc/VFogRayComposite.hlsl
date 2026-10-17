// VFogRayComposite (ps_3_0, full res over the world viewport): port of coa-vfog shaders/vf_ray_composite.hlsl,
// coa's god rays after the world. TEXCOORD0 = world-viewport uv instead of VPOS. s2 blurred rays (bilinear),
// s3 a copy of the world viewport taken just before this pass. Draw opaque, colour write RGB, no depth.
#define VF_GOD_RAYS 1
#include "vf_composite.hlsli"

float4 main(float2 viewportUv : TEXCOORD0) : COLOR0
{
    float3 scene = tex2Dlod(sSceneBeforeFog, float4(viewportUv, 0, 0)).rgb;
    return float4(BlendGodRays(scene, DisplaySpaceGodRays(viewportUv)), 1);
}
