// VFogWavePack (ps_3_0): one mip level of a tile's final maps from the moments (s0) as the CPU path stores them, so
// vw_surface.hlsli reads the same: c0.z = 0: slopes (RGBA16F, .xy = mean slope); 1: state (Argb8888, R = sqrt(variance /
// 0.25), G = sqrt(wide^2 / 1), B = foam, A = oxygen from s1 at level 0, 0 above). c0 = size, 1 / size, mode, level 0.
#include "vw_wave_common.hlsli"

float4 cGrid : register(c0);

sampler2D sMoments : register(s0);
sampler2D sFoam : register(s1);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 t = TexelOf(uv, cGrid.x);
    float4 m = FetchTexel(sMoments, t, cGrid.y);
    [branch] if (cGrid.z == 0)
        return float4(m.xy, 0, 0);
    float4 f = FetchTexel(sFoam, t, cGrid.y);
    float2 foam = cGrid.w > 0 ? saturate(f.xy + f.zw) : 0;
    return float4(sqrt(saturate(m.z / 0.25)), sqrt(saturate(m.w)), foam);
}
