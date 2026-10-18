// VFogWaveFft (ps_3_0, n x n RGBA16F): one radix-2 stage of the inverse DFT (CPU TransformLine, e^{+i}) along rows
// (c1 = 1,0) or columns (c1 = 0,1) of bit-reversed input, halved each stage so values never grow past h0 in half
// (log2 n stages each way = 1/n^2; the h0 prescale makes it the CPU's 1/256^2). c0 = n, 1/n, stage length, 1/length.
#include "vw_wave_common.hlsli"

float4 cGrid : register(c0);
float4 cAxis : register(c1);

sampler2D sSource : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 q = TexelOf(uv, cGrid.x);
    float i = dot(q, cAxis.xy);
    float p = i - cGrid.z * floor(i * cGrid.w);         // position in the block
    float h = 0.5 * cGrid.z;
    float first = i - p + (p >= h ? p - h : p);
    float2 other = q - cAxis.xy * i;
    float2 a = FetchComplex(sSource, other + cAxis.xy * first, cGrid.y);
    float2 b = FetchComplex(sSource, other + cAxis.xy * (first + h), cGrid.y);
    float2 w = Rotation(p * cGrid.w);
    return SplitComplex(0.5 * (a + float2(w.x * b.x - w.y * b.y, w.x * b.y + w.y * b.x)));
}
