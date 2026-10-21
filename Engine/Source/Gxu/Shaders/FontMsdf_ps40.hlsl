// MSDF font pixel shader for CGxDeviceD3d11 (ps_4_0). The same math as the D3D9 ps_2_a shader in
// GxuFontMsdf.cpp (TesterWoWDev/wowdecomp), in the register contract of /root/d3d11-port/shader-xlate:
// the full SM3 varying layout so it links with the translated "UI" vertex shader, c0 in cb0 at b0,
// the device's alpha reference in cb1 at b1. Regenerate GxuFontMsdfShaders.inc with Compile.py.
cbuffer cb0 : register(b0) { float4 c[1]; };
cbuffer cb1 : register(b1) { int4 i[16]; float4 dev_alphaRef; float4 dev_clipPlane[6]; };
Texture2D<float4> t0 : register(t0); SamplerState s0 : register(s0);

struct PSIn {
    float4 pos : SV_Position;
    float4 col0 : COLOR0;
    float4 col1 : COLOR1;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float4 tc4 : TEXCOORD4;
    float4 tc5 : TEXCOORD5;
    float4 tc6 : TEXCOORD6;
    float4 tc7 : TEXCOORD7;
    float fog : FOG0;
};

float median3(float3 v) {
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

float evalSample(float sd, float range, float bias, float width) {
    float dist = sd - 0.5f + bias;
    float fade = saturate((sd - 0.02f) * 20.0f);
    return saturate(dist * range + width + 0.5f) * fade;
}

float4 main(PSIn I) : SV_Target0 {
    float4 color = I.col0;
    float2 texcoord = I.tc0.xy;
    float2 dx = ddx(texcoord);
    float2 dy = ddy(texcoord);

    float2 unitRange = float2(4.0f / 512.0f, 4.0f / 512.0f);
    float2 screenTexSize = 1.0f / max(abs(dx) + abs(dy), 1e-6f);
    float screenPxRange = max(0.5f * dot(unitRange, screenTexSize), 1.0f);

    float fontSize = (c[0].y > 1.0f) ? c[0].y : 12.0f;
    float screenFontSize = fontSize * (screenPxRange / 4.0f);
    float bias = (screenFontSize < 14.0f) ? (14.0f - screenFontSize) * 0.005f : 0.0f;

    float isShadow = (dot(color.rgb, float3(1.0f, 1.0f, 1.0f)) < 0.05f) ? 1.0f : 0.0f;
    bias += isShadow * 0.035f;

    float maxTexelDelta = max(length(dx), length(dy)) * 512.0f;
    float subpixelScale = saturate((maxTexelDelta - 1.15f) * 0.8f) * (1.0f - isShadow);

    float2 off0 = (-0.375f * dx - 0.125f * dy) * subpixelScale;
    float2 off1 = ( 0.125f * dx - 0.375f * dy) * subpixelScale;
    float2 off2 = ( 0.375f * dx + 0.125f * dy) * subpixelScale;
    float2 off3 = (-0.125f * dx + 0.375f * dy) * subpixelScale;

    // SampleGrad with the pixel's own derivatives: the offsets come from ddx/ddy, which must not
    // sit in divergent flow, and the atlas has no mips anyway.
    float sd0 = median3(t0.SampleGrad(s0, texcoord + off0, dx, dy).rgb);
    float sd1 = median3(t0.SampleGrad(s0, texcoord + off1, dx, dy).rgb);
    float sd2 = median3(t0.SampleGrad(s0, texcoord + off2, dx, dy).rgb);
    float sd3 = median3(t0.SampleGrad(s0, texcoord + off3, dx, dy).rgb);

    float outlineWidth = c[0].x;
    float maxMargin = (outlineWidth > 1.5f) ? 0.45f : 0.28f;
    float effOutlineRange = max(screenPxRange, (outlineWidth + 0.5f) / maxMargin);

    float fillAlpha = 0.25f * (evalSample(sd0, screenPxRange, bias, 0.0f) + evalSample(sd1, screenPxRange, bias, 0.0f)
                             + evalSample(sd2, screenPxRange, bias, 0.0f) + evalSample(sd3, screenPxRange, bias, 0.0f));
    float outlineAlpha = 0.25f * (evalSample(sd0, effOutlineRange, bias, outlineWidth) + evalSample(sd1, effOutlineRange, bias, outlineWidth)
                                + evalSample(sd2, effOutlineRange, bias, outlineWidth) + evalSample(sd3, effOutlineRange, bias, outlineWidth));

    float hasOutline = step(0.001f, outlineWidth);
    float3 rgb = lerp(color.rgb, lerp(float3(0.0f, 0.0f, 0.0f), color.rgb, fillAlpha), hasOutline);
    float alpha = lerp(fillAlpha, outlineAlpha, hasOutline);

    float4 o = float4(rgb, color.a * alpha);
    if (dev_alphaRef.x > 0 && o.w < dev_alphaRef.x) discard;  // D3D9 ALPHAFUNC GREATEREQUAL, as the translated UI shader
    return o;
}
