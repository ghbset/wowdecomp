// The fixed-function vertex program: the position through the device's World * View * ProjNative,
// the vertex colour forwarded, texture coordinate sets 0 and 1 through their stages' texture
// transforms, as the engine's fixed-function pipeline would (no lighting, no fog), or, for a stage
// with camera-space-position texgen (the projected textures), from the position through the
// matrix the device folds into pixel rows 7..13 (CGxDeviceVulkanDraw.cpp).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;       // D3DCOLOR, fetched as r, g, b, a; opaque white when the vertex has none
layout(location = 2) in vec2 inTexCoord0;   // texcoord set 0; (0, 0) when the vertex has none
layout(location = 3) in vec2 inTexCoord1;   // texcoord set 1; (0, 0) when the vertex has none

layout(set = 1, binding = 0, std140) uniform Regs
{
    vec4 p[16];
} regs;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTexCoord0;
layout(location = 2) out vec2 vTexCoord1;

// Stage `s`'s coordinate: texgen mode regs.p[7][s] (0 off, 1 on, 2 projected), u, v, q rows from 8 + 3s.
vec2 StageCoord(uint s, vec2 uv)
{
    float mode = regs.p[7][s];
    if (mode < 0.5)
        return TexTransform(s, uv);
    vec4 pos = vec4(inPos, 1.0);
    uint b = 8u + 3u * s;
    vec2 r = vec2(dot(pos, regs.p[b]), dot(pos, regs.p[b + 1u]));
    return mode > 1.5 ? r / dot(pos, regs.p[b + 2u]) : r;
}

void main()
{
    gl_Position = ToVulkanClip(pc.xform * vec4(inPos, 1.0));
    vColor      = inColor;
    vTexCoord0  = StageCoord(0u, inTexCoord0);
    vTexCoord1  = StageCoord(1u, inTexCoord1);
}
