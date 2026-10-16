// The engine's "UI" pixel shader (Shaders\Pixel\UI.bls): the texture times the vertex colour, then
// the alpha test. The interface, the fonts and the software cursor draw through it.
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
    // Without a texture at stage 0 the bound image is the backend's 1x1 white, so the product is
    // the vertex colour, as D3D's shader reads a null texture as white.
    vec4 c = texture(tex0, vTexCoord0) * vColor;
    AlphaTest(c.a);
    outColor = c;
}
