// VFogWaveMip (ps_3_0, RGBA16F level l from level l - 1, s0): coa vw_fft_mip's box filter of the moments, the variance
// kept central (mean of the children's variance + their means' spread) so half storage never subtracts E[s^2] - |E[s]|^2.
// c0 = size of this level, 1 / size of the source level.
#include "vw_wave_common.hlsli"

float4 cGrid : register(c0);

sampler2D sSource : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 c = 2 * TexelOf(uv, cGrid.x);
    float4 a = FetchTexel(sSource, c, cGrid.y);
    float4 b = FetchTexel(sSource, c + float2(1, 0), cGrid.y);
    float4 d = FetchTexel(sSource, c + float2(0, 1), cGrid.y);
    float4 e = FetchTexel(sSource, c + float2(1, 1), cGrid.y);
    float2 mean = 0.25 * (a.xy + b.xy + d.xy + e.xy);
    float2 da = a.xy - mean, db = b.xy - mean, dd = d.xy - mean, de = e.xy - mean;
    float variance = 0.25 * ((a.z + dot(da, da)) + (b.z + dot(db, db)) + (d.z + dot(dd, dd)) + (e.z + dot(de, de)));
    return float4(mean, variance, 0.25 * (a.w + b.w + d.w + e.w));
}
