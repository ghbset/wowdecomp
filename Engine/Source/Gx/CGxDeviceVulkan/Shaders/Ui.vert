// The engine's "UI" vertex shader (Shaders\Vertex\UI.bls): the position as four dot products
// against the vertex constant registers c0..c3, which the interface fills with its
// ViewProjNativeTranspose; the colour and the first texture coordinate forwarded. The second
// texture coordinate set, which the shader does not write, is zero for a fixed-function stage 1.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTexCoord0;
layout(location = 2) out vec2 vTexCoord1;

void main()
{
    vec4 v = vec4(inPos, 1.0);
    // Column i of xform is register c_i.
    vec4 clip = vec4(dot(pc.xform[0], v), dot(pc.xform[1], v), dot(pc.xform[2], v), dot(pc.xform[3], v));
    gl_Position = ToVulkanClip(clip);
    vColor      = inColor;
    vTexCoord0  = inTexCoord0;
    vTexCoord1  = vec2(0.0);
}
