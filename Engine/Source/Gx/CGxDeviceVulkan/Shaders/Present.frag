// The present pass's fragment stage: the scene's texel at this pixel, through the gamma ramp when
// the ramp applies (D3D9's SetGammaRamp, which the engine uses in fullscreen).
#version 450

layout(location = 0) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D scene;

// The ramp as 256 entries of (r, g, b, 0) in 0..1, and params.x = 1 when it applies.
layout(set = 0, binding = 1) uniform Gamma
{
    vec4 lut[256];
    vec4 params;
} gamma;

void main()
{
    vec4 c = texture(scene, vTexCoord);
    if (gamma.params.x > 0.5)
    {
        ivec3 i = ivec3(clamp(c.rgb * 255.0 + 0.5, 0.0, 255.0));
        c.rgb   = vec3(gamma.lut[i.r].r, gamma.lut[i.g].g, gamma.lut[i.b].b);
    }
    outColor = vec4(c.rgb, 1.0);
}
