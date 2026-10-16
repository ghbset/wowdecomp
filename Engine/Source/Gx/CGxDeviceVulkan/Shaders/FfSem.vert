// The fixed-function vertex stage in the translated interface: what a translated pixel module pairs
// with when the engine binds no vertex shader (or one this backend has no program for). Inputs by
// semantic at the translated locations (position0 0, color0 2, texcoord0 4, texcoord1 5; the
// elements the vertex lacks come from the constant stream), texcoords 0 and 1 through their stages'
// fixed-function texture transforms, every varying of the interface written so any pixel module's
// reads are covered: colour 1 black, texcoord 2..7 zero, fog factor 1 (no fog).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"

layout(location = 0) in vec4 inPos;
layout(location = 2) in vec4 inColor;
layout(location = 4) in vec4 inTexCoord0;
layout(location = 5) in vec4 inTexCoord1;

layout(location = 0) out vec4 color0;
layout(location = 1) out vec4 color1;
layout(location = 2) out vec4 texcoord0;
layout(location = 3) out vec4 texcoord1;
layout(location = 4) out vec4 texcoord2;
layout(location = 5) out vec4 texcoord3;
layout(location = 6) out vec4 texcoord4;
layout(location = 7) out vec4 texcoord5;
layout(location = 8) out vec4 texcoord6;
layout(location = 9) out vec4 texcoord7;
layout(location = 10) out float fog0;

void main()
{
    gl_Position = ToVulkanClip(pc.xform * vec4(inPos.xyz, 1.0));
    color0      = inColor;
    color1      = vec4(0.0);
    texcoord0   = vec4(TexTransform(0u, inTexCoord0.xy), inTexCoord0.zw);
    texcoord1   = vec4(TexTransform(1u, inTexCoord1.xy), inTexCoord1.zw);
    texcoord2   = vec4(0.0);
    texcoord3   = vec4(0.0);
    texcoord4   = vec4(0.0);
    texcoord5   = vec4(0.0);
    texcoord6   = vec4(0.0);
    texcoord7   = vec4(0.0);
    fog0        = 1.0;
}
