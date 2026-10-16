// The present pass's vertex stage: one triangle covering the swapchain image, from gl_VertexIndex
// alone (no vertex buffer), with the scene's texture coordinate at each corner.
#version 450

layout(location = 0) out vec2 vTexCoord;

void main()
{
    // (0,0), (2,0), (0,2) in texture space: the triangle's hypotenuse lies outside the image, so the
    // image is covered exactly once.
    vTexCoord   = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vTexCoord * 2.0 - 1.0, 0.0, 1.0);
}
