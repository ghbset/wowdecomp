// VFogShadowMask (ps_3_0, full res over the world viewport; Legion survey 2.7, shadowMask): the land's sun shadow
// (gfxTerrainShadow's WDL lit-height texture, vf_shadow.hlsli) resolved once from the scene depth after the opaque
// world and laid over it as a multiply (GxBlend_Mod), so terrain, WMO and M2 receivers all darken under a mountain.
// Our own shader; Legion's shadowmask ps (91 instructions, 12 taps) was read for its role only.
//
// The receivers already shaded with the engine's shadow maps (C = min(unit map, band 0/1/2), as they do) the stock
// way: lit colour x (1 - k (1 - C)), k = 0.3 (vfog38, shadow audit H1/F1). In the land's shadow (T = 0) the surface
// should get the ambient alone, so the output is lerp(A / stock, 1, T) per channel, stock = (A + D ndl)(1 - k (1 - C)):
// a tree's shadow inside a mountain's merges into it (A either way) instead of staying at 0.7 of lit. Never above 1.
// ndl from a normal rebuilt from the depth (the smaller-step neighbour per axis, so silhouettes keep their own side),
// against the receivers' own light (m_light1 after PickLightBody; the land texture keeps the true sun).
// TEXCOORD0 = world-viewport uv. s0 scene depth, s5/s6 bands 1/2, s7 the land, s8 band 0, s9 the unit map.
// c10 (shading toLight world, strength), c11 (ambient rgb, surface bias yd), c12 (diffuse rgb, k), c13 (stock fog
// start, 1 / span, on, 0), c65-c67 band 0 rows, c68-c70 unit rows, c71 (band 0 on, unit on).
#include "vf_common.hlsli"
#include "vf_shadow.hlsli"
#include "vf_gbuffer.hlsli"

float4 cMaskLight : register(c10);
float4 cMaskAmbient : register(c11);
float4 cMaskDiffuse : register(c12);
float4 cMaskFog : register(c13);
float4 cMaskRows[6] : register(c65);
float4 cMaskMaps : register(c71);

sampler2D sDepth : register(s0);
sampler2D sBand0 : register(s8);
sampler2D sUnit : register(s9);

float3 MaskCoord(int r, float3 p)
{
    return float3(dot(p, cMaskRows[r].xyz), dot(p, cMaskRows[r + 1].xyz), dot(p, cMaskRows[r + 2].xyz)) +
           float3(cMaskRows[r].w, cMaskRows[r + 1].w, cMaskRows[r + 2].w);
}

// What the receivers' shadow maps say: min(unit map, band 0 where it covers the point, else bands 1/2).
float ReceiverShadow(float3 p, float jitter)
{
    float c = 1;
    [branch] if (cMaskMaps.y > 0)
    {
        float3 q = MaskCoord(3, p);
        [branch] if (max(abs(q.x), abs(q.y)) < 1)
            c = ShadowCompare(sUnit, q);
    }
    float3 q0 = MaskCoord(0, p);
    [branch] if (cMaskMaps.x > 0 && max(abs(q0.x), abs(q0.y)) < 0.85 + 0.1 * jitter)
        return min(c, ShadowCompare(sBand0, q0));
    return min(c, CascadeSunVisibility(p, jitter));
}

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
    n = GBufferNormal(pixel, p, normalize(n));
    float ndl = saturate(dot(normalize(ViewToWorldDirection(n)), cMaskLight.xyz));

    float cascade = ReceiverShadow(p, InterleavedGradientNoise(pixel));
    float3 stock = (cMaskAmbient.rgb + cMaskDiffuse.rgb * ndl) * (1 - cMaskDiffuse.w * (1 - cascade));
    float3 ratio = lerp(saturate(cMaskAmbient.rgb / max(stock, 1.0 / 255)), 1, land);
    // The stock fog is in the colour when the volumetric fog did not push it out: fogged pixels keep their colour.
    float fog = saturate((length(p) - cMaskFog.x) * cMaskFog.y) * cMaskFog.z;
    return float4(lerp(1, saturate(ratio), cMaskLight.w * (1 - fog)), 1);
}
