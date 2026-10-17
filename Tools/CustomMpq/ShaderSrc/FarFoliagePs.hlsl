// FarFoliage (ps_3_0): see FarFoliageVs.hlsl. Albedo with coverage alpha (s0) and baked normals (s1), two
// views mixed; lit like the terrain (min(ambient + diffuse x N.L, 1)), times the ground tint and its baked
// shadow, fogged with the terrain's fog colour. Cut out by coverage and by a card-anchored dissolve against
// the hand-over fade (never by a noise that moves with the camera or the mip).
sampler2D sAlb : register(s0);
sampler2D sNrm : register(s1);
float4 cAmb    : register(c3);   // terrain ambient (c25)
float4 cDif    : register(c4);   // terrain diffuse (c26)
float4 cFogCol : register(c5);
float4 cLook   : register(c6);   // x: ground tint strength, y: brightness in baked shadow

float4 main(float4 tint : COLOR0, float4 sun : COLOR1, float4 uv : TEXCOORD0, float4 aux : TEXCOORD1, float fog : FOG) : COLOR0
{
    float4 alb = lerp(tex2D(sAlb, uv.xy), tex2D(sAlb, uv.zw), aux.z);
    float3 n   = lerp(tex2D(sNrm, uv.xy).rgb, tex2D(sNrm, uv.zw).rgb, aux.z) * 2.0 - 1.0;

    float2 cell = floor(aux.xy);
    float3 h3 = frac(float3(cell.x, cell.y, cell.x) * 0.1031);
    h3 += dot(h3, h3.yzx + 33.33);
    float h = frac((h3.x + h3.y) * h3.z);
    clip(min(alb.a - 0.5, aux.w - h));

    float ndl = saturate(dot(normalize(n), sun.xyz * 2.0 - 1.0));
    float3 light = min(cAmb.rgb + cDif.rgb * ndl, 1.0);
    float3 ground = lerp(float3(1.0, 1.0, 1.0), tint.rgb * 2.0, cLook.x) * lerp(cLook.y, 1.0, tint.a);
    float3 c = alb.rgb * ground * light;
    return float4(lerp(cFogCol.rgb, c, fog), 1.0);
}
