// MSDF font pixel shader for the D3D9 devices (ps_2_a), by TesterWoWDev/wowdecomp. The bytecode
// s_msdfPs2aBytecode in GxuFontMsdf.cpp is tester's precompiled build of this source (fxc ps_2_a).
// FontMsdf_ps40.hlsl and ../../Gx/CGxDeviceVulkan/Shaders/Msdf.frag carry the same math.
sampler2D s0 : register(s0);
float4 u_outlineParams : register(c0);

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

float evalSample(float sd, float range, float bias, float width) {
    float dist = sd - 0.5f + bias;
    float fade = saturate((sd - 0.02f) * 20.0f);
    return saturate(dist * range + width + 0.5f) * fade;
}

float4 main(float4 color : COLOR0, float2 texcoord : TEXCOORD0) : COLOR0 {
    float2 dx = ddx(texcoord);
    float2 dy = ddy(texcoord);

    float2 unitRange = float2(4.0f / 512.0f, 4.0f / 512.0f);
    float2 screenTexSize = 1.0f / max(abs(dx) + abs(dy), 1e-6f);
    float screenPxRange = max(0.5f * dot(unitRange, screenTexSize), 1.0f);

    float fontSize = (u_outlineParams.y > 1.0f) ? u_outlineParams.y : 12.0f;
    float screenFontSize = fontSize * (screenPxRange / 4.0f);

    // Distance bias to maintain stroke weight and prevent thinning/breakup at small sizes
    float bias = (screenFontSize < 14.0f) ? (14.0f - screenFontSize) * 0.005f : 0.0f;

    // Detect if this is a shadow pass (shadowColor is black, r+g+b near 0)
    // Boost shadow distance slightly to give rich, dark, high-contrast drop shadows
    float isShadow = (dot(color.rgb, float3(1.0f, 1.0f, 1.0f)) < 0.05f) ? 1.0f : 0.0f;
    bias += isShadow * 0.035f;

    // Measure minification (how many atlas texels per screen pixel)
    // In 2D UI at native scale, maxTexelDelta is ~1.0 -> subpixelScale is 0 (crisp, solid 100% contrast).
    // Shadows never use subpixel offsets (keeps drop shadows 100% solid black).
    // In 3D far-away text, maxTexelDelta is > 1.5 -> subpixelScale ramps to 1.0 (4-sample RGSS).
    float maxTexelDelta = max(length(dx), length(dy)) * 512.0f;
    float subpixelScale = saturate((maxTexelDelta - 1.15f) * 0.8f) * (1.0f - isShadow);

    // 4-point Rotated Grid Supersampling (RGSS) offsets scaled by minification factor
    // Offsets: (-3/8, -1/8), (1/8, -3/8), (3/8, 1/8), (-1/8, 3/8)
    float2 off0 = (-0.375f * dx - 0.125f * dy) * subpixelScale;
    float2 off1 = ( 0.125f * dx - 0.375f * dy) * subpixelScale;
    float2 off2 = ( 0.375f * dx + 0.125f * dy) * subpixelScale;
    float2 off3 = (-0.125f * dx + 0.375f * dy) * subpixelScale;

    float3 samp0 = tex2D(s0, texcoord + off0).rgb;
    float3 samp1 = tex2D(s0, texcoord + off1).rgb;
    float3 samp2 = tex2D(s0, texcoord + off2).rgb;
    float3 samp3 = tex2D(s0, texcoord + off3).rgb;

    float sd0 = median(samp0.r, samp0.g, samp0.b);
    float sd1 = median(samp1.r, samp1.g, samp1.b);
    float sd2 = median(samp2.r, samp2.g, samp2.b);
    float sd3 = median(samp3.r, samp3.g, samp3.b);

    float outlineWidth = u_outlineParams.x;
    float maxMargin = (outlineWidth > 1.5f) ? 0.45f : 0.28f;
    float effOutlineRange = max(screenPxRange, (outlineWidth + 0.5f) / maxMargin);

    float a0 = evalSample(sd0, screenPxRange, bias, 0.0f);
    float a1 = evalSample(sd1, screenPxRange, bias, 0.0f);
    float a2 = evalSample(sd2, screenPxRange, bias, 0.0f);
    float a3 = evalSample(sd3, screenPxRange, bias, 0.0f);
    float fillAlpha = 0.25f * (a0 + a1 + a2 + a3);

    float o0 = evalSample(sd0, effOutlineRange, bias, outlineWidth);
    float o1 = evalSample(sd1, effOutlineRange, bias, outlineWidth);
    float o2 = evalSample(sd2, effOutlineRange, bias, outlineWidth);
    float o3 = evalSample(sd3, effOutlineRange, bias, outlineWidth);
    float outlineAlpha = 0.25f * (o0 + o1 + o2 + o3);

    float hasOutline = step(0.001f, outlineWidth);
    float3 rgb = lerp(color.rgb, lerp(float3(0.0f, 0.0f, 0.0f), color.rgb, fillAlpha), hasOutline);
    float alpha = lerp(fillAlpha, outlineAlpha, hasOutline);

    return float4(rgb, color.a * alpha);
}
