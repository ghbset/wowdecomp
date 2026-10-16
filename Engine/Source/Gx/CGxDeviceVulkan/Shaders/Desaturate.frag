// The engine's "Desaturate" pixel shader (Shaders\Pixel\Desaturate.bls), the interface's
// desaturated image mode: the texture's luminance in every colour channel, times the vertex colour
// (a tinted desaturated icon keeps its tint), then the alpha test.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "AlphaTest.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord0;
layout(location = 2) in vec2 vTexCoord1;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D tex0;

void main()
{
    vec4  t   = texture(tex0, vTexCoord0);
    float lum = dot(t.rgb, vec3(0.299, 0.587, 0.114));
    vec4  c   = vec4(lum, lum, lum, t.a) * vColor;
    AlphaTest(c.a);
    outColor = c;
}
