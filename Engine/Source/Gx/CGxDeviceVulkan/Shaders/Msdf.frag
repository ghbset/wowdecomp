// The interface's MSDF font pixel shader (Gxu "UI_MSDF", GxuFontMsdf.cpp): the median of the
// atlas's three distance channels against the edge, 4-sample rotated-grid supersampling when the
// text is minified, the outline width and font pixel size from pixel register c0 (x, y). The same
// math as the D3D9 / D3D11 shaders (TesterWoWDev/wowdecomp's ps_2_a), then the alpha test.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "AlphaTest.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord0;
layout(location = 2) in vec2 vTexCoord1;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D tex0;

layout(set = 1, binding = 0, std140) uniform Regs
{
    vec4 p[16];
} regs;

float Median(vec3 v)
{
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

float EvalSample(float sd, float range, float bias, float width)
{
    float dist = sd - 0.5 + bias;
    float fade = clamp((sd - 0.02) * 20.0, 0.0, 1.0);
    return clamp(dist * range + width + 0.5, 0.0, 1.0) * fade;
}

void main()
{
    vec2 uv = vTexCoord0;
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);

    vec2 unitRange = vec2(4.0 / 512.0);
    vec2 screenTexSize = 1.0 / max(abs(dx) + abs(dy), vec2(1e-6));
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    float fontSize = regs.p[0].y > 1.0 ? regs.p[0].y : 12.0;
    float screenFontSize = fontSize * (screenPxRange / 4.0);
    float bias = screenFontSize < 14.0 ? (14.0 - screenFontSize) * 0.005 : 0.0;

    float isShadow = dot(vColor.rgb, vec3(1.0)) < 0.05 ? 1.0 : 0.0;
    bias += isShadow * 0.035;

    float maxTexelDelta = max(length(dx), length(dy)) * 512.0;
    float subpixelScale = clamp((maxTexelDelta - 1.15) * 0.8, 0.0, 1.0) * (1.0 - isShadow);

    vec2 off0 = (-0.375 * dx - 0.125 * dy) * subpixelScale;
    vec2 off1 = ( 0.125 * dx - 0.375 * dy) * subpixelScale;
    vec2 off2 = ( 0.375 * dx + 0.125 * dy) * subpixelScale;
    vec2 off3 = (-0.125 * dx + 0.375 * dy) * subpixelScale;

    float sd0 = Median(textureGrad(tex0, uv + off0, dx, dy).rgb);
    float sd1 = Median(textureGrad(tex0, uv + off1, dx, dy).rgb);
    float sd2 = Median(textureGrad(tex0, uv + off2, dx, dy).rgb);
    float sd3 = Median(textureGrad(tex0, uv + off3, dx, dy).rgb);

    float outlineWidth = regs.p[0].x;
    float maxMargin = outlineWidth > 1.5 ? 0.45 : 0.28;
    float effOutlineRange = max(screenPxRange, (outlineWidth + 0.5) / maxMargin);

    float fillAlpha = 0.25 * (EvalSample(sd0, screenPxRange, bias, 0.0) + EvalSample(sd1, screenPxRange, bias, 0.0)
                            + EvalSample(sd2, screenPxRange, bias, 0.0) + EvalSample(sd3, screenPxRange, bias, 0.0));
    float outlineAlpha = 0.25 * (EvalSample(sd0, effOutlineRange, bias, outlineWidth) + EvalSample(sd1, effOutlineRange, bias, outlineWidth)
                               + EvalSample(sd2, effOutlineRange, bias, outlineWidth) + EvalSample(sd3, effOutlineRange, bias, outlineWidth));

    float hasOutline = step(0.001, outlineWidth);
    vec3 rgb = mix(vColor.rgb, mix(vec3(0.0), vColor.rgb, fillAlpha), hasOutline);
    float alpha = mix(fillAlpha, outlineAlpha, hasOutline);

    vec4 c = vec4(rgb, vColor.a * alpha);
    AlphaTest(c.a);
    outColor = c;
}
