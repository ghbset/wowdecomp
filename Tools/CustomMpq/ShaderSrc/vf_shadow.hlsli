// Shadowed sun in-scatter (SHADOW_PORT_PLAN S3): the fog's sun term is tested against the engine's own cascaded sun
// shadow maps (CShadowCache bands 1 and 2, CShadowCache::GetPublished), so shafts fall through the fog past trees and
// buildings. One compare per march step in the nearest band that covers the point, as the engine's receivers do it
// (Terrain2[_pcf]: uv = q.xy * 0.5 + 0.5, lit when ref q.z <= stored depth). Hardware compare (D3D9 hwPCF, D3D11
// SampleCmpLevelZero: build_shaders.py marks s5/s6 as compare slots) is one bilinear PCF tap; the R32F maps (Vulkan,
// D3D9 without hwPCF) take four point taps and interpolate the comparisons (northlight fogShadow), never the depths.
// Outside both bands, and with cShadowControl.x = 0 (vfogShadowedSun 0, shadows low, no maps), the sun is unshadowed.

float4 cShadowBands[6] : register(c56);  // band 1 then band 2: view-space position -> (u, v, ref) rows
float4 cShadowControl : register(c62);   // (on, hardware compare, canopy strength, map size in texels)

// VF_SHADOW_TAPS: 2 = the nearest of bands 1/2 with the filtered compare (default), 1 = one tap in band 2 only (the
// hardware compare, or one point compare on R32F; the march jitter and the temporal filter smooth it), 0 = unshadowed.
// ps_3_0's 32 temps: the lit noisy march (L + N) takes 1, the composites' full-res edge fallback 1, and the lit noisy
// composite 0 (it is at the limit without, as its authored noise, VFogComposite.hlsl).
#ifndef VF_SHADOW_TAPS
#if LOCAL_LIGHTS && VF_NOISE
#define VF_SHADOW_TAPS 1
#else
#define VF_SHADOW_TAPS 2
#endif
#endif

sampler2D sShadowNear : register(s5);
sampler2D sShadowFar : register(s6);

bool ShadowedSun()
{
    return cShadowControl.x > 0;
}

// The haze glow kept in shadow: 1 - canopy where the point is fully shadowed (dark under trees and buildings).
float CanopyScale(float sunVisibility)
{
    return 1 - cShadowControl.z * (1 - sunVisibility);
}

float ShadowCompare(sampler2D maps, float3 q)
{
    float2 uv = q.xy * 0.5 + 0.5;
    [branch] if (cShadowControl.y > 0)
        return tex2Dlod(maps, float4(uv, q.z, 0)).x;
    // One tap at a time (ps_3_0's 32 temps: the lit march is at the limit).
    float2 t = uv * cShadowControl.w - 0.5;
    float2 f = frac(t);
    t = (t - f + 0.5) / cShadowControl.w;
    float lit = 0;
    [unroll] for (int k = 0; k < 4; k++)
    {
        float2 corner = float2(k % 2, k / 2);
        float2 w = lerp(1 - f, f, corner);
        lit += (tex2Dlod(maps, float4(t + corner / cShadowControl.w, 0, 0)).x >= q.z ? 1 : 0) * w.x * w.y;
    }
    return lit;
}

float3 ShadowCoord(int band, float3 viewPosition)
{
    int r = band * 3;
    return float3(dot(viewPosition, cShadowBands[r].xyz), dot(viewPosition, cShadowBands[r + 1].xyz),
                  dot(viewPosition, cShadowBands[r + 2].xyz)) +
           float3(cShadowBands[r].w, cShadowBands[r + 1].w, cShadowBands[r + 2].w);
}

// edgeJitter (0..1) dithers the seam between the bands so it does not show as a line.
float SunVisibility(float3 viewPosition, float edgeJitter)
{
    [branch] if (!ShadowedSun() || VF_SHADOW_TAPS == 0)
        return 1;
#if VF_SHADOW_TAPS == 1
    float3 q = ShadowCoord(1, viewPosition);
    float edge = max(abs(q.x), abs(q.y));
    float stored = tex2Dlod(sShadowFar, float4(q.xy * 0.5 + 0.5, q.z, 0)).x;
    float lit = cShadowControl.y > 0 ? stored : (stored >= q.z ? 1 : 0);
    return lerp(1, lit, saturate((1 - edge) * 10));
#else
    float3 q = ShadowCoord(0, viewPosition);
    [branch] if (max(abs(q.x), abs(q.y)) < 0.85 + 0.1 * edgeJitter)
        return ShadowCompare(sShadowNear, q);
    q = ShadowCoord(1, viewPosition);
    float edge = max(abs(q.x), abs(q.y));
    [branch] if (edge >= 1)
        return 1;
    return lerp(1, ShadowCompare(sShadowFar, q), saturate((1 - edge) * 10));
#endif
}
