// Port of coa-vfog shaders/vf_density_variation.hlsli. N variants only (VF_NOISE); the others compile it out.
// s9: the 32^3 density noise as a slice atlas, 8 x 4 tiles of 34 x 34 (272 x 136), bilinear + wrap.
#ifndef VF_NOISE
#define VF_NOISE 0
#endif

float4 cDensityVariation : register(c78);
sampler2D sDensityNoise : register(s9);

float DensityNoise(float3 uv)
{
    return SampleNoiseAtlas(sDensityNoise, uv * 32, 32, 8, 34).r;
}

float DensityVariation(float3 worldPosition)
{
#if VF_NOISE
    float variation = 1;
    // No early return: fxc flattens those and hoists every atlas fetch of the step, which runs ps_3_0 out
    // of its 32 temps.
    [branch] if (cDensityVariation.x > 0)
    {
        float3 uv = (worldPosition - float3(cDensityVariation.zw, 0)) * (cDensityVariation.y / 32) + 0.5 / 32;
        float noise = DensityNoise(uv) * (4.0 / 3.0);
        uv = uv * 2 + float3(36.5, 16.5, 52.5) / 32;
        float centered = noise + DensityNoise(uv) * (2.0 / 3.0) - 1;
        variation = 1 + saturate(cDensityVariation.x) * centered;
    }
    return variation;
#else
    return 1;
#endif
}

bool DensityVariationEnabled()
{
    return VF_NOISE && cDensityVariation.x > 0;
}
