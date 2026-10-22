// LIGHTING: a post pass's result back into a multisampled scene (Vulkan/Lighting.cpp), drawn in the scene's
// own pass over the world viewport with Present.vert's triangle. The pass resolves every instance into the
// single-sample scene, so a copy into that alone would be lost: every sample gets the post pass's pixel,
// and the draws after it (liquids, transparents, the UI) keep their MSAA.
#version 450

layout(location = 0) out vec4 outColor;

// RGBA8 holding B, G, R, A (what the compute passes write): swizzled back to colour
layout(set = 0, binding = 1) uniform sampler2D gxColor;

void main()
{
    outColor = texelFetch(gxColor, ivec2(gl_FragCoord.xy), 0).bgra;
}
