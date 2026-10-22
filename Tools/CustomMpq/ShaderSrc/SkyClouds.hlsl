// SkyClouds (ps_3_0): wxl-light's sky and volumetric clouds (/root/wxl-light src/CloudShaders.hpp, CloudField.hpp,
// Atmosphere.hpp), drawn by WorldClient/SkyClouds.cpp in the stock cloud sheet's slot of DayNight::RenderSky, a
// screen quad at depth 1 (only pixels nothing has drawn on). The maths and the tuned values are wxl-light's; the
// differences: the 64^3 noise volume is a 2D atlas of its mip levels (no volume textures in Gx), the hardware
// depth test replaces the linear depth read, there is no wxl-water sea under the horizon (the liquids draw over
// the sky later), the registers are c0-c12, and the output is for GxBlend_Alpha (wxl-light blended ONE, SRCALPHA).
// s0 noise atlas, s3 the game's moon picture, s4 the game's cloud sheet (alpha = DNClouds density).

sampler2D sNoise : register(s0);
sampler2D sMoon  : register(s3);
sampler2D sStock : register(s4);
float4 gR   : register(c0);    // the view rays: dir = right * ndc.x + up * ndc.y + forward
float4 gU   : register(c1);    // w = 1: sStock is live, the game's pattern says where the cloud is
float4 gF   : register(c2);    // w = 1: sMoon is bound
float4 gLit : register(c3);    // rgb = the light's colour on cloud, w = coverage
float4 gAmb : register(c4);    // rgb = the cloud's own shade, w = opacity
float4 gDr  : register(c5);    // xy = drift, zw = the target's size in pixels
float4 gSun : register(c6);    // xyz = toward the sun or moon, w = its disc's radius in radians (0: none drawn)
float4 gBody : register(c7);   // rgb = the disc's colour (display), w = how soft its edge is
float4 gAtH : register(c8);    // rgb = the sky at eye level (linear), w = how much of the sky is ours
float4 gAtT : register(c9);    // rgb = the band of sky over eye level (linear)
float4 gAtG : register(c10);   // rgb = the light's glow (linear); w, gAtS.w, gAtZ.w = the twilight band opposite the light
float4 gAtS : register(c11);   // xyz = toward the sun or moon
float4 gAtZ : register(c12);   // rgb = the sky straight up (linear)

// ---- Atmosphere.hpp
float3 SkyAt(float3 d, float kb, float kg)
{
    float e = saturate(d.z);
    float3 base = lerp(lerp(gAtH.rgb, gAtT.rgb, 1.0 - exp(-e * 5.0)), gAtZ.rgb, smoothstep(0.25, 1.0, e));
    float c = clamp(dot(d, gAtS.xyz), -1.0, 1.0);
    float low = 0.3 + 0.7 * exp(-e * 5.0);
    float tight = 0.4375 / pow(max(1.5625 - 1.5 * c, 1.0e-4), 1.5) * 0.08;   // Henyey-Greenstein, g = 0.75
    float wide = pow(saturate(c * 0.5 + 0.5), 4.0);
    float3 belt = float3(gAtG.w, gAtS.w, gAtZ.w) * (pow(saturate(0.5 - 0.5 * c), 2.0) * exp(-e * 4.0) * (1.0 - exp(-e * 30.0)));
    return (base + belt) * kb + gAtG.rgb * ((0.5 * wide + 0.6 * tight) * low * kg);
}
float3 ToDisplay(float3 x)
{
    x = max(x, 0.0);
    return pow(saturate(x / (1.0 + max(x - 0.8, 0.0) * 1.2)), 1.0 / 2.2);
}

// ---- the noise volume: SkyClouds.cpp NoiseAtlas. Level L (64 >> L texels a side) is a grid of min(n, 8) columns
// of (n + 2)^2 tiles with wrap borders; the levels are stacked down a 528 x 720 atlas.
static const float2 kAtlas = float2(528.0, 720.0);
float4 Tex0(float2 uv) { return tex2Dlod(sNoise, float4(uv, saturate(uv - 2))); }   // vf_common Tex2DLevel0
float4 NoiseLevel(float3 uvw, float L)
{
    float n = exp2(6.0 - L);
    float cols = min(n, 8.0);
    float tile = n + 2.0;
    float y0 = L < 0.5 ? 0.0 : (L < 1.5 ? 528.0 : (L < 2.5 ? 664.0 : (L < 3.5 ? 700.0 : (L < 4.5 ? 710.0 : 716.0))));
    float3 p = uvw * n;
    float2 inTile = frac(p.xy / n) * n + float2(1.0, 1.0 + y0);
    float z = p.z - 0.5;
    float f = frac(z);
    float s = z - f;
    s -= n * floor(s / n);
    float row = floor(s / cols);
    float4 a = Tex0((float2(s - row * cols, row) * tile + inTile) / kAtlas);
    s = s + 1.0 < n ? s + 1.0 : 0.0;
    row = floor(s / cols);
    float4 b = Tex0((float2(s - row * cols, row) * tile + inTile) / kAtlas);
    return lerp(a, b, f);
}
// tex3Dlod(sNoise, float4(uvw, lod)) with wrap and trilinear mips (levels 0-5)
float4 Noise(float3 uvw, float lod)
{
    lod = clamp(lod, 0.0, 5.0);
    float l0 = floor(lod);
    float t = lod - l0;
    float4 a = NoiseLevel(uvw, l0);
    [branch] if (t > 0.01)
        a = lerp(a, NoiseLevel(uvw, min(l0 + 1.0, 5.0)), t);
    return a;
}

// ---- CloudField.hpp
static const float R = 60.0, BASE = 1.0, THICK = 0.7;   // a small planet: the layer bends down to the horizon
static const float SCALE = 0.26, DETAIL = 1.63, WEATHER = 0.075, SIGMA = 10.0;
static float gCloudLod = 0.0;
float Remap(float v, float a, float b, float c, float d) { return c + (v - a) / max(b - a, 1.0e-4) * (d - c); }
float Height(float3 p) { return (length(p + float3(0.0, 0.0, R)) - R - BASE) / THICK; }
// DNClouds::WorldToTexture (0x007EF920): the sheet's dome is a unit sphere centred cos(45 deg) under the camera.
float2 StockUv(float3 p)
{
    float3 d  = normalize(p);
    float  c  = 0.70710678;
    float  t  = -c * d.z + sqrt(c * c * d.z * d.z + 1.0 - c * c);
    float  th = min(acos(clamp(t * d.z + c, -1.0, 1.0)), 0.78539816);
    float  l  = length(d.xy);
    return 0.5 + (l < 1.0e-5 ? float2(0.0, 0.0) : d.xy / l) * (th * 0.63661977);
}
float Weather(float3 p, float3 at, float cov, float stock)
{
    [branch] if (stock > 0.5)
    {
        float s = tex2Dlod(sStock, float4(StockUv(at), 0.0, 0.0)).a;
        return max(s * s * (3.0 - 2.0 * s), smoothstep(0.7, 1.0, cov));
    }
    float w = NoiseLevel(float3(p.xy * WEATHER, 0.37), 0.0).r;
    float shut = 0.30 * smoothstep(0.7, 1.0, cov);
    return smoothstep(0.50 - 0.35 * cov - shut, 0.80 - 0.35 * cov - shut, w);
}
float Shape(float3 p, float hf, float wth, float cov)
{
    float4 n = Noise(p * SCALE, gCloudLod);
    float wf = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float base = saturate(Remap(n.r, wf * 0.55 - 0.55, 1.0, 0.0, 1.0));
    float prof = smoothstep(0.0, 0.12, hf) * smoothstep(1.0, 0.35, hf);   // a flat base, a rounded top
    float cover = saturate(cov * 0.55 + wth * 0.5 + 0.35 * smoothstep(0.7, 1.0, cov)) * wth;
    return saturate(Remap(base * prof, 1.0 - cover, 1.0, 0.0, 1.0));
}

// ---- CloudShaders.hpp
float HG(float c, float g) { float d = max(1.0 + g * g - 2.0 * g * c, 1e-4); return (1.0 - g * g) / (d * sqrt(d)); }
float Density(float3 p, float hf, float wth)
{
    float d = Shape(p, hf, wth, gLit.w);
    [branch] if (d > 0.0)
    {
        float3 h = Noise(p * DETAIL, gCloudLod + 2.65).gba;
        float hfreq = h.x * 0.625 + h.y * 0.25 + h.z * 0.125;
        d = saturate(Remap(d, (1.0 - hfreq) * 0.45 * (1.0 - d * 0.5), 1.0, 0.0, 1.0));
    }
    return d;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 dir = normalize(gR.xyz * ndc.x + gU.xyz * ndc.y + gF.xyz);
    float3 ours = ToDisplay(SkyAt(dir, 1.0, 1.0));
    float2 px = uv * gDr.zw;
    ours += (frac(52.9829189 * frac(px.x * 0.06711056 + px.y * 0.00583715)) - 0.5) / 255.0;
    float L0 = 0.0, L1 = 0.0, L2 = 0.0, stay = 1.0;
    // Under the horizon: still sky (seen from a height), with no cloud in it; the sea draws over it later.
    [branch] if (dir.z <= 0.0)
    {
        L0 = ours.r * gAtH.w; L1 = ours.g * gAtH.w; L2 = ours.b * gAtH.w;
        stay = 1.0 - gAtH.w;
    }
    else
    {
        float  T = 1.0;
        float3 L = 0.0;
        [branch] if (gLit.w > 0.0)
        {
            float b  = R * dir.z;
            float t0 = -b + sqrt(b * b + 2.0 * R * BASE + BASE * BASE);
            float t1 = -b + sqrt(b * b + 2.0 * R * (BASE + THICK) + (BASE + THICK) * (BASE + THICK));
            float ds = (t1 - t0) / 20.0;
            gCloudLod = max(log2(ds / 0.06) + 0.5, 0.0);
            float jit = frac(52.9829189 * frac(px.x * 0.06711056 + px.y * 0.00583715));
            float3 sun = gSun.xyz;
            float cosT = dot(dir, sun);
            float ph = min(0.55 * HG(cosT, -0.15) + 0.45 * HG(cosT, 0.6), 2.2);
            float3 off = float3(gDr.xy, 0.0);
            [loop] for (int i = 0; i < 20; ++i)
            {
                [branch] if (T > 0.02)
                {
                    float3 p   = dir * (t0 + ds * (float(i) + jit));
                    float  hf  = Height(p);
                    float3 q   = p + off;
                    float  wth = Weather(q, p, gLit.w, gU.w);
                    [branch] if (wth > 0.0)
                    {
                        float dn = Density(q, hf, wth);
                        [branch] if (dn > 0.0)
                        {
                            float od = Shape(q + sun * 0.06, Height(p + sun * 0.06), wth, gLit.w) * 0.06
                                     + Shape(q + sun * 0.14, Height(p + sun * 0.14), wth, gLit.w) * 0.14
                                     + Shape(q + sun * 0.30, Height(p + sun * 0.30), wth, gLit.w) * 0.30;
                            od *= SIGMA * 1.6;
                            float E = max(exp(-od), 0.7 * exp(-0.25 * od));
                            float3 col = min(gAmb.rgb * (0.82 + 0.18 * saturate(hf)) + gLit.rgb * (E * ph), 1.0);
                            float dT = exp(-dn * SIGMA * ds);
                            L += col * (T * (1.0 - dT));
                            T *= dT;
                        }
                    }
                }
            }
        }
        float k = smoothstep(0.0, 0.10, dir.z) * gAmb.w;
        float through = 1.0 - (1.0 - T) * k;
        float3 body = 0.0;
        float cover = 0.0;
        [branch] if (gSun.w > 0.0)
        {
            float ang = acos(clamp(dot(dir, gSun.xyz), -1.0, 1.0));
            cover = 1.0 - smoothstep(gSun.w * (1.0 - gBody.w), gSun.w * (1.0 + gBody.w), ang);
            body = gBody.rgb * cover;
            [branch] if (gF.w > 0.5 && gAtH.w < 1.0 && ang < 2.0 * gSun.w)
            {
                float3 sr = normalize(cross(float3(0.0, 0.0, 1.0), gSun.xyz));
                float3 su = cross(gSun.xyz, sr);
                float2 muv = 0.5 + float2(dot(dir, sr), -dot(dir, su)) / (2.1 * sin(gSun.w));
                float4 m = tex2Dlod(sMoon, float4(saturate(muv), 0.0, 0.0));
                float inside = step(abs(muv.x - 0.5), 0.5) * step(abs(muv.y - 0.5), 0.5);
                body = lerp(m.rgb * (m.a * inside) * gBody.rgb * 1.15, body, gAtH.w);
                cover = lerp(m.a * inside, cover, gAtH.w);
            }
            body += gBody.rgb * ((1.0 - gAtH.w) * 0.22 * exp(-ang / (3.0 * gSun.w)));
        }
        stay = through * (1.0 - gAtH.w) * (1.0 - cover);
        float3 add = L * k + (ours * gAtH.w + body) * through;
        L0 = add.r; L1 = add.g; L2 = add.b;
    }
    // wxl-light's out = (add, stay) for blend ONE, SRCALPHA: as an alpha blend, a = 1 - stay and rgb = add / a.
    // Where add is more than a (the moon's halo over the night sky) a grows to it: add + dst * (1 - add) there, not
    // add + dst, a few levels darker under the halo.
    float3 addc = float3(L0, L1, L2);
    float a = max(1.0 - stay, max(addc.r, max(addc.g, addc.b)));
    return float4(a > 1.0e-4 ? saturate(addc / a) : float3(0.0, 0.0, 0.0), a);
}
