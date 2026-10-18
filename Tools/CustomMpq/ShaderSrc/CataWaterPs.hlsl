// CataWaterAbove / CataWaterBelow (ps_3_0): Cataclysm 4.3.4's ProcWaterAbove (p = 2: one-tap screen-space
// reflection with the fog colour as the fallback, foam on, no ripples) and, with BELOW, ProcWaterBelow for a submerged
// camera. waterStyle 1, docs/BACKLOG_PORT.md "Cata water". Ported from wxl-water src/Shaders.hpp kPixel (@ 17cf9af)
// minus the far sea, caustics, sun / land shadows, the last-frame depth reprojection (the copy here is this frame's,
// taken before the first water draw) and the debug views; Cata decoded in water-research-W1-cata.md section 8.
// Pairs with the CataWater vertex shader.
sampler2D sRamp   : register(s0);    // stock procedural depth-colour ramp (Cata s2)
sampler2D sNormal : register(s9);    // XTextures\ocean\newoceanbump.blp (Cata s5)
sampler2D sFoam   : register(s10);   // XTextures\foam\waterchop7bw.blp (Cata s7)
sampler2D sScene  : register(s12);   // the world viewport before the water (Cata s0 / s3)
sampler2D sDepth  : register(s13);   // the raw scene depth before the water, R32F, whole window
// Free for the follow-up pieces: s11 ripple height field (piece B), s14 planar reflection (piece D).

float4 gFogColor : register(c4);     // stock
row_major float4x4 gVP : register(c16);   // camera-relative world -> clip (the reflection lookup)
float4 gDepthTex : register(c20);    // xy = 1 / depth texture size, z w = (A, B): view depth = B / (d - A)
float4 gScreen   : register(c21);    // world viewport in window pixels (x, y, w, h)
float4 gP        : register(c22);    // (P6 normal strength, P9 diffuse, P10 specular, P14 normal to world)
float4 gMisc     : register(c23);    // x = deepest world depth (past it: sky), y = thickness fog strength (0 = off until piece C fills c24/c25)
float4 gFogA     : register(c24);    // Cata fog A: (-1/(end-start), end/(end-start), exponent, P7 * 0.35)
float4 gFogAColor: register(c25);    // its colour
float4 gLightW   : register(c26);    // world direction toward the light
float4 gDiffuse  : register(c27);    // sun diffuse colour
float4 gSpecular : register(c28);    // sun specular colour

// The viewport uv of a clip position, and the linear view depth of the scene there.
float2 ScreenUv(float2 clipXy, float w) { return float2(clipXy.x / w * 0.5 + 0.5, 0.5 - clipXy.y / w * 0.5); }
float SceneZ(float2 uv)
{
    float d = tex2D(sDepth, (gScreen.xy + uv * gScreen.zw) * gDepthTex.xy).r;
    return d > gMisc.x ? 1.0e5 : gDepthTex.w / min(d - gDepthTex.z, -1.0e-8);
}

float4 main(float4 col : COLOR0, float4 coarse : TEXCOORD0, float4 fine : TEXCOORD1, float4 ramp : TEXCOORD2,
            float4 world4 : TEXCOORD3, float4 vnrm : TEXCOORD4, float4 clip : TEXCOORD5, float4 foamuv : TEXCOORD7) : COLOR0
{
    float3 world = world4.xyz;
    // --- normals (Cata 8.1) -------------------------------------------------------------------------
    float  dist = length(world);
    float3 V    = world / max(dist, 1e-4);   // camera -> pixel
    float2 nA = tex2D(sNormal, coarse.xy).xy + tex2D(sNormal, coarse.zw).xy - 1.0;
    float2 nB = (tex2D(sNormal, fine.xy).xy + tex2D(sNormal, fine.zw).xy - 1.0) * 1.4;
    float2 n  = lerp(nB, 2.0 * nA, saturate(dist * 0.001 + 0.2)) * gP.x;
    float2 N2 = n * rsqrt(dot(n, n) + 1.0);
    // piece B: N2 += the ripple slope (W1 8.2, wxl-water RippleSlope) here.

    // --- shading (Cata 8.3) -------------------------------------------------------------------------
    float3 Nw  = normalize(float3(gP.w * N2 + vnrm.xy, vnrm.z));
#ifdef BELOW
    float4 alb = tex2D(sRamp, ramp.xy);
#else
    float4 alb = tex2D(sRamp, ramp.xy + abs(N2 * 0.1));
#endif
    float3 tint = alb.rgb * col.rgb;
    float  Zw  = ramp.w;
    float2 suv = ScreenUv(clip.xy, Zw);
    float3 L   = normalize(gLightW.xyz);
#ifdef BELOW
    // Seen from underneath (Cata ProcWaterBelow): the frame above refracted through the wave normal, the ramp
    // tint over it and a weak glint. No shoreline, no reflection, opaque.
    float3 Vb  = V * float3(1.0, 1.0, -1.0);   // Cata mirrors the view, not the normal
    float  f   = 1.0 - saturate(dot(-Vb, Nw));
    float  f2  = f * f;
    float2 uvd = suv + Nw.xy * 0.1;            // a fixed offset: there is no thickness to scale it by
    float3 refr = tex2D(sScene, (Zw - SceneZ(uvd) >= 0.0) ? suv : uvd).rgb;
    float3 c = lerp(refr, tint, alb.a * (1.0 - 0.1 * f2));
    c += saturate(dot(L, Nw)) * gP.y * gDiffuse.rgb * tint;
    float3 H = normalize(V * float3(-1.0, -1.0, 1.0) + L + float3(0.0, 0.0, 1e-4));
    c += 0.4 * gP.z * alb.a * f2 * f2 * (saturate(Nw.z - 0.8) + pow(saturate(dot(H, Nw)), 50.0)) * gSpecular.rgb;
    c = lerp(gFogColor.rgb, c, ramp.z);
    return float4(c, 1.0);
#else
    float  f   = 1.0 - saturate(dot(-V, Nw));
    float  f2  = f * f;
    float  th  = SceneZ(suv) - Zw;
    // Something in front of the water at this pixel fails the depth test, so a negative thickness is a unit
    // standing in the water: read it as open water.
    if (th < -0.1) th = 3.0;

    // 1 soft shoreline alpha
    float a = saturate(th / ((0.5 * f2 + 0.2) * 1.2));
    a = a * a * (3.0 - 2.0 * a);

    // 2 reflection, Cata p = 2 (W1 8.4): one tap 128 yd along the grazing ray (normal flattened to 0.3) in the
    // frame copy, faded at steep rays and at the screen edge, the fog colour where it misses.
    // piece D: the planar reflection (p = 1) replaces `fallback` with its target where it covers this water.
    float3 fallback = gFogColor.rgb;
    float3 Rr = reflect(V, normalize(Nw * float3(0.3, 0.3, 1.0)));
    float4 pc = float4(world + 128.0 * Rr, 1.0);
    pc = mul(pc, gVP);
    float  ws = saturate((0.75 - Rr.z * Rr.z) * (4.0 / 3.0)) * step(0.05, pc.w);
    float2 ndc = pc.xy / max(pc.w, 0.05);
    ws *= saturate(1.0 - ndc.x * ndc.x) * saturate(1.0 - ndc.y * ndc.y);
    float3 refl = lerp(fallback, tex2D(sScene, ScreenUv(ndc, 1.0)).rgb, ws);
    refl = lerp(refl, tint, 0.6);

    // 3 refraction, refusing offsets that land on something in front of the water
    float2 off = Nw.xy * 0.1 * saturate((1.2 - f2) * th / max(Zw, 1e-3) * 100.0);
    float2 uvd = suv + off;
    float  Zd  = SceneZ(uvd);
    bool   keep = Zw - Zd >= 0.0;
    float3 refr = tex2D(sScene, keep ? suv : uvd).rgb;
    // Cata AboveFog folded in: the ground under the water fades into fog A with the thickness at the texel read.
    float  thR  = keep ? th : Zd - Zw;
    float  kFog = saturate(pow(saturate(thR * gFogA.x + gFogA.y), gFogA.z));
    refr = lerp(refr, lerp(gFogAColor.rgb, refr, kFog), gMisc.y);
    float3 under = lerp(refr, tint, alb.a * (1.0 - 0.1 * f2));
    float3 c = lerp(under, refl, 0.5 * f2 + 0.1);

    // 4 shoreline foam: the chop texture swirled by the normal, in the band where the water is thin, out to 150 yd.
    float3 foam = tex2D(sFoam, foamuv.xy - Nw.xy).rgb + tex2D(sFoam, foamuv.zw + Nw.yx * 0.2).rgb;
    float  fs   = saturate((1.0 - V.z) * th / ((0.5 * f2 + 0.2) * 3.75));
    float  fw   = (1.0 - fs * fs * (3.0 - 2.0 * fs)) * gFogA.w * (1.1 - saturate(dist * 0.006667)) * Nw.z;
    // A band narrower than a pixel lands on a pixel one frame and misses it the next: fade it out from 3 px to 1.
    fw *= saturate(((0.5 * f2 + 0.2) * 3.75 / max(1.0 - V.z, 1e-3) / max(fwidth(th), 1e-4) - 1.0) * 0.5);
    c += fw * foam;

    // 5 lighting
    c += saturate(dot(L, Nw)) * gP.y * gDiffuse.rgb * tint;
    float3 H = normalize(-V + L + float3(0.0, 0.0, 1e-4));
    c += gP.z * f2 * (saturate(Nw.z - 0.8) + pow(saturate(dot(H, Nw)), 50.0)) * gSpecular.rgb;

    // 6 fog
    c = lerp(gFogColor.rgb, c, ramp.z);
    return float4(c, a);
#endif
}
