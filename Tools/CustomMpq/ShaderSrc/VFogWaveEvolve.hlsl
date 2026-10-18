// VFogWaveEvolve (ps_3_0, n x n RGBA16F): coa vw_fft_evolve + WaterFoldModes as VolumetricFogWaterWaves.cpp Step:
// the h0 texel (prescaled towards.re, towards.im, away.re: coarse in s0 + residual in s1; loop frequency mod 2048 in
// s0.w) rotated by its loop frequency, folded Hermitian with the
// mirror mode, written at the bit-reversed position so the butterflies (VFogWaveFft) need no reorder pass.
// c0 = n, 1/n, loop frequency per root radius, loop fraction; c1.x = 0: heightAndX, 1: alongY (one complex per pass).
#include "vw_wave_common.hlsli"

float4 cGrid : register(c0);
float4 cPair : register(c1);

sampler2D sH0 : register(s0);
sampler2D sH0Low : register(s1);

float Reverse(float index)     // 8-bit reversal; for n = 128 halve it (bit 7 of the index is 0)
{
    float r = 0;
    [unroll] for (int b = 0; b < 8; b++)
    {
        float h = floor(index * 0.5);
        r = r * 2 + (index - 2 * h);
        index = h;
    }
    return r;
}

// height = towards * rot + conj(away) * conj(rot), away = (away.re, towards.im) (the CPU's h0AwayReal pairing)
float2 Height(float2 texel, out float2 direction)
{
    float4 h0 = FetchTexel(sH0, texel, cGrid.y);
    h0.xyz += FetchTexel(sH0Low, texel, cGrid.y).xyz;
    float2 c = texel - 0.5 * cGrid.x;
    float r2 = dot(c, c);
    // The CPU's integer frequency exactly: its low part from h0.w + the multiple of 2048 nearest this estimate (a GPU
    // sqrt an ulp off would flip the CPU's floor(x + 0.5) near ties; the estimate only has to be within 1024).
    float estimate = sqrt(sqrt(r2)) * cGrid.z + 0.5;
    float frequency = h0.w + 2048 * floor((estimate - h0.w) / 2048 + 0.5);
    float2 rot = Rotation(frac(frequency * cGrid.w));
    float l = sqrt(r2);
    direction = c / (l > 0 ? l : 1);
    return float2(h0.x * rot.x - h0.y * rot.y + h0.z * rot.x - h0.y * rot.y,
                  h0.x * rot.y + h0.y * rot.x - h0.z * rot.y - h0.y * rot.x);
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 q = TexelOf(uv, cGrid.x);
    float2 at = float2(Reverse(q.x), Reverse(q.y)) * (cGrid.x / 256);
    float2 mirror = float2(at.x > 0 ? cGrid.x - at.x : 0, at.y > 0 ? cGrid.x - at.y : 0);
    float2 a, b;
    float2 h = Height(at, a);
    float2 m = Height(mirror, b);
    float2 foldH = 0.5 * float2(h.x + m.x, h.y - m.y);
    float2 dir = cPair.x > 0 ? float2(a.y, b.y) : float2(a.x, b.x);
    float2 fold = 0.5 * float2(dir.x * h.y + dir.y * m.y, -dir.x * h.x + dir.y * m.x);    // -i a h + conj(-i b m)
    return SplitComplex(cPair.x > 0 ? fold : float2(foldH.x - fold.y, foldH.y + fold.x));
}
