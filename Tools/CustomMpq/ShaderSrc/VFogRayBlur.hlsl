// VFogRayBlur (ps_3_0, ray target): port of coa-vfog shaders/vf_ray_blur.hlsl, a radial blur toward the sun.
// TEXCOORD0 = ray target uv instead of VPOS. c9 = (sun uv, step fraction toward the sun, 1 / tap weight sum).
float4 cRadialBlur : register(c9);

sampler2D sSource : register(s0);

static const int kTaps = 8;
static const float kDecayPerTap = 0.9;

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 tapStep = (cRadialBlur.xy - uv) * cRadialBlur.z;
    float3 sum = 0;
    float tapWeight = 1;
    [unroll] for (int k = 0; k < kTaps; k++)
    {
        sum += tex2Dlod(sSource, float4(uv + tapStep * k, 0, 0)).rgb * tapWeight;
        tapWeight *= kDecayPerTap;
    }
    return float4(sum * cRadialBlur.w, 1);
}
