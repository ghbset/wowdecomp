// VFogWaveSurface (ps_3_0, n x n RGBA16F): coa vw_fft_surface as VolumetricFogWaterWaves.cpp Step. Displacement =
// (-1)^(x+y) 2^e (heightAndX.im, alongY.re, -heightAndX.re) (s0, s1: the transformed pairs); central differences.
// c0.w = 0: moments level 0 = (mean slope xy, central variance 0, wide slope^2); 1: the foam/oxygen step from s2
// (coa FoamStep, dt clamped by the caller) stored split for half (coa SplitForHalfStorage: foam, oxygen, residuals).
// c0 = n, 1/n, 2^e, mode; c1 = half texels per unit, wide radius, 1 / wide span, dt; c2 = foam bias, rate, decay,
// keep (0 = start from no foam); c3 = oxygen bias, rate, decay.
#include "vw_wave_common.hlsli"

float4 cGrid : register(c0);
float4 cStencil : register(c1);
float4 cFoam : register(c2);
float4 cOxygen : register(c3);

sampler2D sHeightAndX : register(s0);
sampler2D sAlongY : register(s1);
sampler2D sPreviousFoam : register(s2);

float3 Displacement(float2 texel)
{
    float2 hx = FetchComplex(sHeightAndX, texel, cGrid.y);
    float2 ay = FetchComplex(sAlongY, texel, cGrid.y);
    float scale = (1 - 4 * frac(0.5 * (texel.x + texel.y))) * cGrid.z;
    return scale * float3(hx.y, ay.x, -hx.x);
}

float FoamStep(float previous, float jacobian, float3 p)
{
    float f = saturate(previous);
    float injection = max(p.x + 1 - jacobian, 0);
    return saturate(f + cStencil.w * (p.y * injection * (1 - f) - p.z * f));
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 t = TexelOf(uv, cGrid.x);
    float3 alongX = (Displacement(t + float2(1, 0)) - Displacement(t - float2(1, 0))) * cStencil.x;
    float3 alongY = (Displacement(t + float2(0, 1)) - Displacement(t - float2(0, 1))) * cStencil.x;
    [branch] if (cGrid.w == 0)
    {
        float wideX = (Displacement(t + float2(cStencil.y, 0)).z - Displacement(t - float2(cStencil.y, 0)).z) * cStencil.z;
        float wideY = (Displacement(t + float2(0, cStencil.y)).z - Displacement(t - float2(0, cStencil.y)).z) * cStencil.z;
        return float4(alongX.z, alongY.z, 0, wideX * wideX + wideY * wideY);
    }
    float jacobian = (1 + alongX.x) * (1 + alongY.y) - alongY.x * alongX.y;
    float4 stored = FetchTexel(sPreviousFoam, t, cGrid.y);
    float2 previous = saturate(stored.xy + stored.zw) * cFoam.w;
    float2 foam = SplitUnsigned(FoamStep(previous.x, jacobian, cFoam.xyz));
    float2 oxygen = SplitUnsigned(FoamStep(previous.y, jacobian, cOxygen.xyz));
    return float4(foam.x, oxygen.x, foam.y, oxygen.y);
}
