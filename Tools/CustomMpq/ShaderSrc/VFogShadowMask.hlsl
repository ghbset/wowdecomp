// VFogShadowMask (ps_3_0, full res over the world viewport; Legion survey 2.7, shadowMask): the land's sun shadow
// (gfxTerrainShadow's WDL lit-height texture, vf_shadow.hlsli) resolved once from the scene depth after the opaque
// world and laid over it as a multiply (GxBlend_Mod), so terrain, WMO and M2 receivers all darken under a mountain.
// Our own shader; Legion's shadowmask ps (91 instructions, 12 taps) was read for its role only.
//
// The receivers already shaded with the engine's cascades (C). The output is the ratio of the light the surface
// should get to what it got, per channel: (ambient + diffuse * ndl * min(T, C)) / (ambient + diffuse * ndl * C),
// T the land's visibility. Never above 1, and 1 where a cascade already darkened the pixel (no double darkening).
// ndl from a normal rebuilt from the depth (the smaller-step neighbour per axis, so silhouettes keep their own side).
// TEXCOORD0 = world-viewport uv. s0 scene depth, s5/s6 cascades, s7 the land. c10 (toLight world, strength),
// c11 (ambient rgb, surface bias yd), c12 (diffuse rgb), c13 (stock fog start, 1 / span, on, 0).
#include "vf_common.hlsli"
#include "vf_shadow.hlsli"

float4 cMaskLight : register(c10);
float4 cMaskAmbient : register(c11);
float4 cMaskDiffuse : register(c12);
float4 cMaskFog : register(c13);

sampler2D sDepth : register(s0);

float3 ViewAt(float2 pixel)
{
    return ViewRayAtUnitDepth(pixel) * LinearDepth(SampleDepth(sDepth, pixel));
}

float3 Nearer(float3 p, float3 a, float3 b)
{
    return abs(a.z - p.z) < abs(b.z - p.z) ? a - p : p - b;
}

float4 main(float2 viewportUv : TEXCOORD0) : COLOR0
{
    float2 pixel = FullPixelFromUv(viewportUv);
    float depth = SampleDepth(sDepth, pixel);
    [branch] if (IsSky(depth) || cTerrainShadow.w <= 0)
        return 1;
    float3 p = ViewRayAtUnitDepth(pixel) * LinearDepth(depth);
    float3 world = mul(float4(p, 1), cViewToWorld).xyz;
    float lit = tex2Dlod(sTerrainShadow, float4(cTerrainShadow.xy - cTerrainShadow.z * world.yx, 0, 0)).x;
    float land = saturate(1 - (lit - world.z - cMaskAmbient.w) * cTerrainShadow.w);
    [branch] if (land >= 1)
        return 1;

    float3 dx = Nearer(p, ViewAt(pixel + float2(1, 0)), ViewAt(pixel - float2(1, 0)));
    float3 dy = Nearer(p, ViewAt(pixel + float2(0, 1)), ViewAt(pixel - float2(0, 1)));
    float3 n = cross(dx, dy);
    n *= dot(n, p) > 0 ? -1 : 1;    // toward the camera
    float ndl = saturate(dot(normalize(ViewToWorldDirection(n)), cMaskLight.xyz));

    float cascade = CascadeSunVisibility(p, InterleavedGradientNoise(pixel));
    float3 sun = cMaskDiffuse.rgb * ndl;
    float3 ratio = (cMaskAmbient.rgb + sun * min(land, cascade)) / max(cMaskAmbient.rgb + sun * cascade, 1.0 / 255);
    // The stock fog is in the colour when the volumetric fog did not push it out: fogged pixels keep their colour.
    float fog = saturate((length(p) - cMaskFog.x) * cMaskFog.y) * cMaskFog.z;
    return float4(lerp(1, saturate(ratio), cMaskLight.w * (1 - fog)), 1);
}
