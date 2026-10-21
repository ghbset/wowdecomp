// LegionSsao* (ps_3_0; Legion survey 2.1 SSAO, FEATURE_BACKLOG #5; docs/LEGION_PORT.md): screen-space ambient occlusion
// from the scene depth after the opaque world, before liquids, the fog composite and the transparents. Our own HLSL.
// Legion 7.3.5's ssao ps (16 hemisphere taps, 4x4 random rotation, view normals) was read for its shape only; the
// depth-derivative normals (the nearer neighbour per axis), the depth-weighted blur and the multiply over the colour
// follow northlight effects.hlsl AO / AOBlur (MIT, (c) 2026 lepola).
//   PASS 0 LegionSsao          AO into the AO target (world viewport / c1.x): s0 depth. Out = the factor (rgb).
//   PASS 1 LegionSsaoBox       4x4 box over the AO (4 bilinear taps): s1 AO. Cancels the 4x4 rotation pattern exactly.
//   PASS 2 LegionSsaoBilateral the same 4x4 box, each texel weighted by its depth's distance from the centre's: s0, s1.
//   PASS 3 LegionSsaoApply     full res over the world viewport, drawn GxBlend_Mod: s1 the (blurred) AO, bilinear.
// The 4x4 box spans AO texels k-2..k+1, so its result is shifted half a texel: the apply reads at + c10.x texels.
// c0-c3, c8, c99 as vf_common.hlsli (c8 = the AO target). c10 = (radius yd, fade start yd, 1 / fade length, strength)
// (apply: c10.x = the read shift), c11 = (stock fog start, 1 / span, on, 0).
#include "vf_common.hlsli"
#include "vf_gbuffer.hlsli"

float4 cSsao : register(c10);
float4 cSsaoFog : register(c11);

sampler2D sDepth : register(s0);
sampler2D sAo : register(s1);

// View z of a depth sample; sky, the horizon band and past the far clip are far away (they occlude nothing).
float ViewZ(float depth)
{
    return depth > DeepestWorldDepth() ? 1e6 : DepthPerInverseViewZ() / (depth - DepthAtInfiniteViewZ());
}

float3 ViewAt(float2 pixel)
{
    return ViewRayAtUnitDepth(pixel) * ViewZ(SampleDepth(sDepth, pixel));
}

float3 Nearer(float3 p, float3 a, float3 b)
{
    return abs(a.z - p.z) < abs(b.z - p.z) ? a - p : p - b;
}

// 4x4 ordered (Bayer) index 0..15 of an AO texel: Legion's 4x4 randomRotation texture without the texture.
float Bayer4(float2 t)
{
    float2 a = fmod(t, 2);
    float2 b = fmod(floor(t * 0.5), 2);
    return 4 * fmod(2 * a.x + 3 * a.y, 4) + fmod(2 * b.x + 3 * b.y, 4);
}

float2 TexelDepthPixel(float2 texel)
{
    return LowResTexelToFullPixel(clamp(texel, 0, LowResSize() - 1));
}

#if PASS == 0
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 texel = LowResTexelFromUv(uv);
    float2 pixel = LowResTexelToFullPixel(texel);
    float depth = SampleDepth(sDepth, pixel);
    [branch] if (depth > DeepestWorldDepth())
        return 1;
    float3 p = ViewRayAtUnitDepth(pixel) * ViewZ(depth);
    float dist = length(p);
    // SSAODistance fade, and none where the stock fog is in the colour (the fog is not occluded).
    float fade = max(saturate((dist - cSsao.y) * cSsao.z), saturate((dist - cSsaoFog.x) * cSsaoFog.y) * cSsaoFog.z);
    [branch] if (fade >= 1)
        return 1;

    float3 n = normalize(cross(Nearer(p, ViewAt(pixel + float2(1, 0)), ViewAt(pixel - float2(1, 0))),
                               Nearer(p, ViewAt(pixel + float2(0, 1)), ViewAt(pixel - float2(0, 1)))));
    n *= dot(n, p) > 0 ? -1 : 1;    // toward the camera
    n = GBufferNormal(pixel, p, n);
    float3 t = normalize(cross(n, abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0)));
    float3 b = cross(n, t);

    // 16 hemisphere taps: a golden-angle spiral, turned per texel by the 4x4 index (one 22.5 degree step each),
    // elevation and length stratified (longer taps last, as Legion's kernel), cosine weighted.
    float2 rot;
    sincos(Bayer4(texel) * 0.39269908, rot.y, rot.x);
    const float2 kGolden = float2(-0.73736887, 0.67549029);     // cos, sin of 2.39996 rad
    float radius = cSsao.x;
    float bias = 0.03 + 0.002 * p.z;
    float elevation = 0.5;
    float occlusion = 0;
    [loop] for (float i = 0; i < 16; i++)
    {
        float k = (i + 1) / 16;
        float h = sqrt(elevation);                  // sin of the polar angle
        float3 d = (t * (rot.x * h) + b * (rot.y * h) + n * sqrt(1 - elevation)) * (radius * lerp(0.1, 1, k * k));
        float3 q = p + d;
        float2 tap = clamp(NdcToPixel(q.xy / max(q.z, 0.05) * ViewToNdcScale() + ViewToNdcOffset()),
                           ViewportOrigin() + 0.5, ViewportLastPixelCentre());
        float z = ViewZ(SampleDepth(sDepth, tap));
        // Behind the tap's surface = occluded; an occluder far in front (a near silhouette) fades out by distance.
        occlusion += (z < q.z - bias ? 1 : 0) * saturate(radius / max(abs(p.z - z), 1e-3));
        rot = float2(rot.x * kGolden.x - rot.y * kGolden.y, rot.x * kGolden.y + rot.y * kGolden.x);
        elevation = frac(elevation + 0.61803399);
    }
    return saturate(1 - occlusion * (1.0 / 16) * cSsao.w * (1 - fade));
}
#elif PASS == 1
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 s = LowResTexelSize();
    return 0.25 * (tex2Dlod(sAo, float4(uv + float2(-1.5, -1.5) * s, 0, 0)) + tex2Dlod(sAo, float4(uv + float2(0.5, -1.5) * s, 0, 0)) +
                   tex2Dlod(sAo, float4(uv + float2(-1.5, 0.5) * s, 0, 0)) + tex2Dlod(sAo, float4(uv + float2(0.5, 0.5) * s, 0, 0)));
}
#elif PASS == 2
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 texel = LowResTexelFromUv(uv);
    float z = ViewZ(SampleDepth(sDepth, TexelDepthPixel(texel)));
    [branch] if (z > 1e5)
        return 1;
    float tolerance = 1 / (0.02 * z + 0.05);
    float sum = 0;
    float weight = 0;
    [loop] for (float i = 0; i < 16; i++)
    {
        float2 o = float2(fmod(i, 4), floor(i * 0.25)) - 2;
        float w = exp2(-abs(ViewZ(SampleDepth(sDepth, TexelDepthPixel(texel + o))) - z) * tolerance);
        sum += tex2Dlod(sAo, float4((texel + o + 0.5) * LowResTexelSize(), 0, 0)).r * w;
        weight += w;
    }
    return sum / weight;    // the centre's own texel is in the box: weight >= 1
}
#else
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 texel = FullPixelToLowResTexel(FullPixelFromUv(uv)) + cSsao.x;
    return float4(tex2Dlod(sAo, float4((texel + 0.5) * LowResTexelSize(), 0, 0)).rgb, 1);
}
#endif
