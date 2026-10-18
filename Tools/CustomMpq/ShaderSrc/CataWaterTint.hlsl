// CataWaterTint (ps_3_0, over the world viewport): Cataclysm 4.3.4 FFXUnderWaterLow, frame * (1 - screenmask * k), with
// the procedural mask wxl-water uses in place of Blizzard's texture (dark toward the corners, untouched in the
// middle) and a colour multiplier from the zone's underwater fog. waterStyle 1 with the camera in water,
// docs/BACKLOG_PORT.md "Cata water". Ported from wxl-water src/Shaders.hpp kCausticUnder (@ 17cf9af), without the
// caustics. s0 = a copy of the world viewport; TEXCOORD0 = uv across it.
sampler2D sScene : register(s0);
float4 gTint : register(c0);         // rgb = colour multiplier, w = corner darkening

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float2 ndc = uv * 2.0 - 1.0;
    float  v = smoothstep(0.2, 2.0, dot(ndc, ndc));
    return float4(tex2D(sScene, uv).rgb * gTint.rgb * (1.0 - gTint.w * v), 1.0);
}
