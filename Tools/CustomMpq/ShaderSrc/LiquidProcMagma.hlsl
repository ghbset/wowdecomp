// Cata 4.3.4 ProcMagma (lava, slime, green lava), backlog #10, docs/BACKLOG_PORT.md. Ported from wxl-water
// (Shaders.hpp kLavaPixel/kLavaVertex), checked against wowdecomp15595 CMaterialMagma::SetupRender.
// Pairs with the STOCK vsLiquidMagma: CMaterialMagma::Render sets its tex transform to identity, so TEXCOORD0
// is the raw vertex uv and FOG the stock fog factor. Cata's VS work (the six 2x3 texture matrices and the
// noise A/B lerp field) is done here per pixel: the matrices are linear, so it is the same result.
//   c4      fog colour (stock Liquid PSConstants)
//   c6..c11 M0..M5 as (row0.xy, row1.xy): M0 lerp field, M1/M2 noise uv A/B, M3..M5 layers 0..2
//   c12     (M0 row3.xy, M1 row3.xy)   c13 (M2, M3)   c14 (M4, M5)   -- the scroll translations
//   c15     (base colour rgb = LiquidType Int[2], noise depth: frac(t / 600 s))
sampler2D sNoise  : register(s0);   // XTextures\FX\noise.blob 128x128x16 as a 4x4 atlas of 130^2 tiles (520^2)
sampler2D sLayer0 : register(s1);   // Magma0 / Slime0 / LavaGreen0
sampler2D sLayer1 : register(s2);
sampler2D sLayer2 : register(s3);
float4 cFogColor : register(c4);
float4 cM0 : register(c6);
float4 cM1 : register(c7);
float4 cM2 : register(c8);
float4 cM3 : register(c9);
float4 cM4 : register(c10);
float4 cM5 : register(c11);
float4 cT01 : register(c12);
float4 cT23 : register(c13);
float4 cT45 : register(c14);
float4 cBase : register(c15);

float2 Mul(float4 a, float2 t, float2 b) { return t.x * a.xy + t.y * a.zw + b; }
float2 WrapPi(float2 x) { return frac(x * 0.15915494 + 0.5) * 6.28318531 - 3.14159265; }

// The volume tap: bilinear inside the slice tile (its 1-texel border holds the wrapped neighbour), then the
// lerp between the two slices around depth w; trilinear with wrap on all axes, like Cata's tex3D.
float3 Slice(float slice, float2 inTile)
{
    float row = floor(slice * 0.25);
    return tex2Dlod(sNoise, float4((float2(slice - row * 4.0, row) * 130.0 + inTile) / 520.0, 0.0, 0.0)).rgb;
}

float3 Noise(float2 uv)
{
    float2 inTile = frac(uv) * 128.0 + 1.0;
    float z = cBase.w * 16.0 - 0.5;
    float f = frac(z);
    float s = z - f;
    s -= 16.0 * floor(s / 16.0);
    float s1 = s + 1.0 < 16.0 ? s + 1.0 : 0.0;
    return lerp(Slice(s, inTile), Slice(s1, inTile), f);
}

float4 main(float2 tc : TEXCOORD0, float fog : FOG) : COLOR
{
    float2 l  = Mul(cM0, tc, cT01.xy);
    float2 a1 = WrapPi(l), a2 = WrapPi(2.0 * l), a3 = WrapPi(3.0 * l);
    float  w  = 0.5 + 0.075 * ((cos(a1.x) + sin(a1.y)) + (cos(a2.x) + sin(a2.y)) + (cos(a3.x) + sin(a3.y)));
    float3 n  = lerp(Noise(Mul(cM1, tc, cT01.zw)), Noise(Mul(cM2, tc, cT23.xy)), w);
    float3 c  = lerp(cBase.rgb, tex2D(sLayer0, Mul(cM3, tc, cT23.zw)).rgb, n.x);
    float4 t1 = tex2D(sLayer1, Mul(cM4, tc, cT45.xy));
    c = lerp(c, t1.rgb, n.y * t1.a);
    float4 t2 = tex2D(sLayer2, Mul(cM5, tc, cT45.zw));
    c = lerp(c, t2.rgb, n.z * t2.a);
    return float4(lerp(cFogColor.rgb, c, fog), 1.0);
}
