// VFogCausticsAbove / VFogCausticsUnder (ps_3_0, full-screen over the world viewport, blended GxBlend_Mod2x =
// 2 src dest, so 0.5 leaves the pixel as it is): wxl-water (branch caustics @ 17cf9af) Shaders.hpp Caustic and
// kCausticUnder, and the "3a caustics" term of its surface shader. docs/BACKLOG_PORT.md "Caustics".
// ABOVE (UNDER 0): s0 = the bed (the modern water's pre-water view Z copy), s1 = the copy with the water (|r|), over
//   the shaded water; the caustic scales the water pixel by 1 + c (wxl-water lit the refraction; this pass sees the
//   shaded surface, so the reflected share, Schlick at the flat surface, is taken out of c).
// UNDER 1: s0 = the scene depth (DeviceCopySceneDepth, decoded through c3 / c99), plus the underwater tint.
// The sun shadow: vf_shadow.hlsli (the engine's bands 1 and 2, s5 / s6, c56-c62) in place of wxl-water's SunVis.
#include "vf_common.hlsli"
#include "vf_shadow.hlsli"

sampler2D sDepth : register(s0);
#if !UNDER
sampler2D sWater : register(s1);
#endif
float4 cCausticSun : register(c9);      // xyz world toward the sun, w = UNDER: the surface's height above the camera (yd)
float4 cCausticColour : register(c10);  // rgb = the sun's colour x waterCaustics (0 = no caustic), w = time (s)
float4 cCausticTile : register(c11);    // xy = camera world xy wrapped to the 3 yd tile, z = fog end, w = 1 / (end - start)
float4 cCausticTint : register(c12);    // UNDER: rgb = colour multiplier, w = vignette strength (0 = none)

// Sunlight focused by the waves onto the ground under shallow water. No texture: three rounds of a tileable
// interference pattern (after Dave Hoskins' "tileable water caustic"), period 1 in p. wxl-water verbatim.
float Caustic(float2 p, float t)
{
    float2 q = frac(p) * 6.2831853 - 250.0;
    float2 i = q;
    float c = 1.0;
    for (int n = 0; n < 3; n++)
    {
        float tt = t * (1.0 - 3.5 / float(n + 1));
        i = q + float2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
        c += 1.0 / length(float2(q.x * 0.005 / sin(i.x + tt), q.y * 0.005 / cos(i.y + tt)));
    }
    c = 1.17 - pow(c / 3.0, 1.4);
    return pow(abs(c), 8.0);
}

float CausticOn()
{
    return dot(cCausticColour.rgb, 1) > 1e-4 ? 1 : 0;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 pixel = FullPixelFromUv(uv);
    float3 ray = ViewRayAtUnitDepth(pixel);
    float3 L = cCausticSun.xyz;
    float c = 0;
#if UNDER
    float3 V = ray * LinearDepth(SampleDepth(sDepth, pixel));
    float3 P = ViewToWorldDirection(V);                 // camera-relative world
    // The face normal from the next pixels' depth (wxl-water: ddx / ddy, which the Vulkan translator has no rule for).
    float3 Px = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(1, 0)) * LinearDepth(SampleDepth(sDepth, pixel + float2(1, 0))));
    float3 Py = ViewToWorldDirection(ViewRayAtUnitDepth(pixel + float2(0, 1)) * LinearDepth(SampleDepth(sDepth, pixel + float2(0, 1))));
    float3 N = normalize(cross(Px - P, Py - P));
    N = dot(N, P) > 0.0 ? -N : N;                       // toward the camera
    float h = cCausticSun.w - P.z;                      // water above this point; < 0: seen through the surface
    float dist = length(P);
    // Nothing at the surface, full from half a yard down, gone by 25; fades with the water's own fog; only on faces
    // the sun can reach, and not with the sun at the horizon.
    float w = CausticOn() * saturate(h * 2.0) * saturate(1.0 - h / 25.0) * saturate((cCausticTile.z - dist) * cCausticTile.w)
            * saturate(L.z * 2.5) * saturate(dot(N, L) * 1.5 + 0.15);
    [branch] if (w > 0.003)
    {
        float2 cp = (P.xy + cCausticTile.xy - L.xy * (h * 0.5 / max(L.z, 0.3))) / 3.0;
        c = Caustic(cp, cCausticColour.w * 0.6) * w * 2.0 * SunVisibility(V, 0.5);
    }
    // Cata FFXUnderWaterLow, frame * (1 - screenmask * k), with a procedural mask: dark toward the corners.
    float2 ndc = PixelToNdc(pixel);
    float3 o = 0.5 * (1.0 + cCausticColour.rgb * c) * cCausticTint.rgb * (1.0 - cCausticTint.w * smoothstep(0.2, 2.0, dot(ndc, ndc)));
#else
    float Zb = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
    float Zw = abs(tex2Dlod(sWater, float4(uv, 0, 0)).r);
    float th = Zb - Zw;                                 // water in front of the bed along the view (wxl-water thR)
    float3 W = ViewToWorldDirection(ray * Zw);          // the surface point, camera-relative
    float dist = length(W);
    // Nothing at the waterline, full from about half a yard of water, gone by 7 yd down, by 90 yd away, and with the
    // sun at the horizon. The pattern is anchored to the world at the ground point and slides along the sun with depth.
    float cw = CausticOn() * saturate(th * 2.0) * saturate(1.0 - th / 7.0) * saturate(1.0 - dist / 90.0) * saturate(L.z * 2.5);
    [branch] if (cw > 0.003)
    {
        float3 V = ray * Zb;
        float3 G = ViewToWorldDirection(V);
        float2 cp = (G.xy + cCausticTile.xy - L.xy * (th * 0.5 / max(L.z, 0.3))) / 3.0;
        float f = 1.0 - abs(W.z) / max(dist, 1e-3);
        float fresnel = 0.02 + 0.98 * f * f * f * f * f;
        c = Caustic(cp, cCausticColour.w * 0.6) * cw * 2.0 * SunVisibility(V, 0.5) * (1.0 - fresnel);
    }
    float3 o = 0.5 * (1.0 + cCausticColour.rgb * c);
#endif
    clip(dot(abs(o - 0.5), 1) - 1e-5);                 // unchanged pixels: no blend (8-bit 0.5 is 128/255)
    return float4(o, 1);
}
