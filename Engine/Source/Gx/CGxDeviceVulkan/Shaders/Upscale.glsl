// FSR: the inputs and the output pass of the world upscaler (Vulkan/Upscale.cpp). One set for both:
//   0 the scene depth copy   1 a colour (the scene, or FSR's output)   3 out: colour (rgba16f, or rgba8 into the scene)
//   4 the constants          5 out: FSR depth (r32f)                    6 out: motion (rg16f)
// Matrices are the engine's row-major, row-vector ones (Lighting.glsl).

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gxDepth;
layout(set = 0, binding = 1) uniform sampler2D gxColor;

layout(std140, set = 0, binding = 4) uniform GxUpscale {
    layout(row_major) mat4 gxInvProj;  // NDC (as drawn, jittered) to this frame's view space
    layout(row_major) mat4 gxReproj;   // this frame's view space to last frame's clip, unjittered
    vec4 gxRender;                     // the world as drawn: x left, y top, z width, w height (scene pixels)
    vec4 gxDepthRange;                 // x min, y max, z 1 / (max - min), w the near plane
    vec4 gxJitter;                     // xy the jitter (NDC), z 1 with a previous frame
    vec4 gxProjW;                      // clip w = dot(vec4(view position, 1), gxProjW)
    vec4 gxNative;                     // the world viewport at full size: x left, y top, z width, w height
    vec4 gxSource;                     // out: xy 1 / gxColor's size, z 1 = no FSR this frame: gxColor is the scene,
                                       // its gxRender part stretched bilinearly
};
