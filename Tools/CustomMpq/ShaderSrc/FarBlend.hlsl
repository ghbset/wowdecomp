// FarBlend (ps_3_0): the far-terrain blend band. Drawn over the real terrain chunks near the view
// distance with the stock `Terrain` vertex shader (permutation 0), so the inputs are its outputs:
// color0 = sun light x 0.5, texcoord0 = the far-ground atlas uv (c13/c18), texcoord1 = the ground-plane
// offset from the camera in yards (c14/c19 with c23 = camera), fog = the terrain's own fog factor.
// Paints the far land's colour, lit and fogged exactly as the far pass paints it (stock Terrain1 with
// the atlas on both stages), at alpha = smoothstep over the band, so by the cut-off the real terrain
// IS the far land's colour and there is no line. docs/FARFIELD_FOG.md.
sampler2D sAtlas : register(s0);
float4 cFog  : register(c2);   // the terrain's fog colour (as Terrain1)
float4 cBand : register(c3);   // x = band start (yd), y = 1 / band width, z = strength

float4 main(float4 col : COLOR0, float2 uv : TEXCOORD0, float2 d : TEXCOORD1, float fog : FOG) : COLOR0
{
    float3 c = tex2D(sAtlas, uv).rgb * col.rgb * 2.0;
    c = lerp(cFog.rgb, c, fog);
    float t = saturate((length(d) - cBand.x) * cBand.y);
    return float4(c, t * t * (3.0 - 2.0 * t) * cBand.z);
}
