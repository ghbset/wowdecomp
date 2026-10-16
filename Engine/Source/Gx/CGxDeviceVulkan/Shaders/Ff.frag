// The fixed-function fragment program: D3D9's texture stages 0 and 1 as the engine configures them
// (GxRs_ColorOp0 / GxRs_AlphaOp0 over the texture and the vertex colour, then GxRs_ColorOp1 /
// GxRs_AlphaOp1 over the second texture and that result -- the minimap's circular mask, the
// terrain's alpha maps), then the alpha test. Without a texture at stage 0 the vertex colour
// passes through. Then D3D9's linear vertex fog (D3DFOG_LINEAR on view z, as CGxDeviceD3d sets
// it) under GxRs_Fog: the colour and enable in the constant slice's p[15], the -1/(end-start) and
// end/(end-start) of GxRs_FogStart / GxRs_FogEnd in p[14]; view z is the clip w (1 / FragCoord.w).
#version 450
#extension GL_GOOGLE_include_directive : require

#include "Push.glsl"
#include "Stage.glsl"
#include "AlphaTest.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord0;
layout(location = 2) in vec2 vTexCoord1;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;

layout(set = 1, binding = 0, std140) uniform Regs
{
    vec4 p[16];
} regs;

void main()
{
    // A stage without a texture binds the backend's 1x1 white, which Stages() does not read.
    vec4 cur = Stages(vColor, texture(tex0, vTexCoord0), texture(tex1, vTexCoord1));
    AlphaTest(cur.a);
    if (regs.p[15].w > 0.5)
    {
        float f = clamp((1.0 / gl_FragCoord.w) * regs.p[14].x + regs.p[14].y, 0.0, 1.0);
        cur.rgb = mix(regs.p[15].rgb, cur.rgb, f);
    }
    outColor = cur;
}
