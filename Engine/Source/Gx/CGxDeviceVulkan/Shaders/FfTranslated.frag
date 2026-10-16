// The fixed-function pixel stage in the translated interface: what a translated vertex module pairs
// with when the engine binds no pixel shader and the vertex record names no generated companion.
// D3D9's texture stages 0 and 1 over the vertex module's colour 0 and texcoords 0 and 1 (the ops
// from the push constants, as Ff.frag), then the alpha test. Reads color0, texcoord0 and texcoord1
// only, so it pairs with any vertex module that writes them.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "Stage.glsl"
#include "AlphaTest.glsl"

layout(location = 0) in vec4 color0;
layout(location = 2) in vec4 texcoord0;
layout(location = 3) in vec4 texcoord1;

layout(location = 0) out vec4 oC0;

layout(set = 0, binding = 0) uniform sampler2D s0;
layout(set = 0, binding = 1) uniform sampler2D s1;

void main()
{
    vec4 cur = Stages(clamp(color0, 0.0, 1.0), texture(s0, texcoord0.xy), texture(s1, texcoord1.xy));
    AlphaTest(cur.a);
    oC0 = cur;
}
