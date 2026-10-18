// VFogWave* (GPU waves, docs/VFOG_PORT.md "Waves"): shared by the off-screen passes of VolumetricFogWaterWavesGpu.cpp.
// Every pass draws a full n x n (or mip-sized) RGBA16F / Argb8888 target with DrawQuad: TEXCOORD0 = (i + 0.5) / size at
// pixel centres on every backend, so the texel index is floor(uv * size); point-sampled sources, wrap addressing.
// Tools/CustomMpq/check_gpu_waves.py replays this maths with float16 stores and must pass after any change here.
static const float kWaveTwoPi = 6.28318548;
static const float kSmallestNormalHalf = 6.10351563e-5;
static const float kCoarseHalfStepsPerOctave = 512;
static const float kLargestResidualFraction = 0.99951171875;

float2 TexelOf(float2 uv, float size)
{
    return floor(uv * size);
}

float4 FetchTexel(sampler2D source, float2 texel, float inverseSize)
{
    return tex2Dlod(source, float4((texel + 0.5) * inverseSize, 0, 0));
}

// coa vw_fft_surface SplitForHalfStorage: x = coarse (9 significant bits, exact in half), y = the residual.
float2 SplitUnsigned(float value)
{
    float step = exp2(floor(log2(max(value, kSmallestNormalHalf)))) / kCoarseHalfStepsPerOctave;
    float coarse = floor(value / step) * step;
    return float2(coarse, min(value - coarse, step * kLargestResidualFraction));
}

// One complex value in an RGBA16F texel at ~2^-20 relative precision: (re hi, im hi, re lo, im lo); read as xy + zw.
float4 SplitComplex(float2 value)
{
    float2 re = SplitUnsigned(abs(value.x)) * sign(value.x);
    float2 im = SplitUnsigned(abs(value.y)) * sign(value.y);
    return float4(re.x, im.x, re.y, im.y);
}

float2 FetchComplex(sampler2D source, float2 texel, float inverseSize)
{
    float4 t = FetchTexel(source, texel, inverseSize);
    return t.xy + t.zw;
}

// 2 pi f with f taken to [-0.5, 0.5) first (better sincos accuracy; same angle).
float2 Rotation(float fraction)
{
    float f = fraction - (fraction >= 0.5 ? 1 : 0);
    float s, c;
    sincos(kWaveTwoPi * f, s, c);
    return float2(c, s);
}
