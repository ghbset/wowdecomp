// Soft particles (wxl-light Soft.cpp a35 + tools/rxrewrite.py rewrite_soft_pixel; docs/BACKLOG_PORT.md): the stock
// particle / ribbon pixel shader (Combiners_Mod permutations 0 and 8: no shadow) plus a fade where the particle
// lies within a fade distance in front of the scene behind it. SoftParticles.cpp binds it in place of the stock
// one for blended particle and ribbon draws only.
//
// The stock vertex shaders (CDiffuse_T1 / Color_T1) are kept. Their fog factor is clamp(pow(max(z * c30.x +
// c30.y, 0), c30.z), 0, 1) of the view depth z; SoftParticles.cpp sets c30 = (-1 / D, 1, 1) for the draw, so FOG
// carries 1 - z / D, and the stock fog is worked out here from the real c30 (cFog).
//   c2  = the engine's (fog colour rgb, alpha ref): Combiners_Mod's own register
//   c40 = (1 / fade yards, rgb fade, alpha fade, 0)
//   c41 = (neutral rgb, D)
//   c42 = (1 / depth width, 1 / depth height, depth at infinite view z, depth per inverse view z)
//   c43 = (real c30.x, c30.y, c30.z, deepest world depth: beyond it is horizon or sky)
//   s12 = the scene depth (DeviceCopySceneDepth, .r = window depth), point sampled
sampler2D sTex : register(s0);
sampler2D sDepth : register(s12);
float4 cFogColorRef : register(c2);
float4 cSoft : register(c40);
float4 cNeutral : register(c41);
float4 cDepth : register(c42);
float4 cFog : register(c43);

float4 main(float4 col : COLOR0, float2 uv : TEXCOORD0, float fog : FOG, float2 vpos : VPOS) : COLOR
{
    float4 t = tex2D(sTex, uv);
#if ALPHAKILL
    clip(col.a * t.a - cFogColorRef.w);   // Combiners_Mod:8 texkill
#endif
    float4 c = float4(t.rgb * col.rgb, t.a * col.a);

    float z = (1.0 - fog) * cNeutral.w;
    float f = saturate(pow(max(z * cFog.x + cFog.y, 0.0), cFog.z));
    c.rgb = lerp(cFogColorRef.rgb, c.rgb, f);

    float d = tex2Dlod(sDepth, float4((vpos + 0.5) * cDepth.xy, 0, 0)).r;
    float sceneZ = d > cFog.w ? 1e30 : cDepth.w / (d - cDepth.z);
    float k = 1.0 - saturate((sceneZ - z) * cSoft.x);   // 1 at the surface, 0 a fade distance in front
    c.rgb = lerp(c.rgb, cNeutral.rgb, k * cSoft.y);
    c.a *= 1.0 - k * cSoft.z;
    return c;
}
