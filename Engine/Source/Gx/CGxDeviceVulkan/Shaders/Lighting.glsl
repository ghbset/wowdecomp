// LIGHTING: the modern-lighting post passes as compute (Vulkan/Lighting.cpp), a port of the D3D11 device's
// CGxDeviceD3d11/Lighting/Lighting.hlsl: keep the two in step. Every pass binds the same set:
//   0 the scene depth (a depth copy, .r)   1 gxColor   2 gxColor2   3 the pass's output (storage)   4 the constants
// A pixel shader's SV_Position (pixel centre) is gl_GlobalInvocationID + 0.5 here (+ the viewport's origin
// for the passes that run over the world viewport). Matrices are the engine's row-major, row-vector ones:
// HLSL mul(v, M) is v * M with row_major.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D gxDepth;
layout(set = 0, binding = 1) uniform sampler2D gxColor;
layout(set = 0, binding = 2) uniform sampler2D gxColor2;

layout(std140, set = 0, binding = 4) uniform GxPost {
    layout(row_major) mat4 gxInvProj;  // NDC to view
    layout(row_major) mat4 gxInvView;  // view to world
    vec4 gxViewportPx;                 // the world viewport: x left, y top, z width, w height (pixels)
    vec4 gxDepthRange;                 // x min, y max, z 1 / (max - min)
    vec4 gxTarget;                     // this pass's output: x width, y height, z 1 / width, w 1 / height
    vec4 gxAo;                         // x radius (yards), y intensity, z fade start, w fade end
    vec4 gxAoProj;                     // x pixels per yard at clip w 1, y the depth step
    vec4 gxFogColor;                   // rgb the engine's fog colour, a scattering
    vec4 gxFogParams;                  // x start, y end, z density, w 1 = the stock fog is on
    vec4 gxSunColor;
    vec4 gxSunDir;                     // xyz toward the sun (world)
    vec4 gxHeightFog;                  // x density, y falloff, z base height, w most opacity
    vec4 gxCamera;                     // xyz camera (world), w time
    vec4 gxBloom;                      // x threshold, y knee, z intensity, w 1 = on
    vec4 gxTone;                       // x exposure, y mode (0 clamp, 1 neutral, 2 ACES), z lightingDebug
    vec4 gxSource;                     // the texture sampled: xy 1 / size; zw the blur direction
    vec4 gxProjW;                      // clip w = dot(vec4(view position, 1), gxProjW)
    vec4 gxPass;                       // Vulkan only: apply x 1 = SSAO, y 1 = fog; bloom x 0 prefilter, 1 down, 2 up
};

float GxDepthAt(ivec2 p) {
    return texelFetch(gxDepth, p, 0).r;
}

vec3 GxDec(vec3 c) {
    return pow(max(c, 0.0), vec3(2.2));
}

vec3 GxEnc(vec3 c) {
    return pow(max(c, 0.0), vec3(1.0 / 2.2));
}

vec3 GxViewPos(vec2 pixel, float depth) {
    vec2 ndc = vec2((pixel.x - gxViewportPx.x) / gxViewportPx.z * 2.0 - 1.0, 1.0 - (pixel.y - gxViewportPx.y) / gxViewportPx.w * 2.0);
    vec4 h = vec4(ndc, (depth - gxDepthRange.x) * gxDepthRange.z, 1.0) * gxInvProj;
    return h.xyz / h.w;
}

bool GxIsGeometry(float depth) {
    return depth <= gxDepthRange.y + 0.00001 && depth < 0.99999;
}

ivec2 GxClampPixel(vec2 p) {
    return ivec2(clamp(p, gxViewportPx.xy, gxViewportPx.xy + gxViewportPx.zw - 1.0));
}

float GxClipW(vec3 p) {
    return dot(vec4(p, 1.0), gxProjW);
}
