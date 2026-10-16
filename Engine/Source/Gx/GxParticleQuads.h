#ifndef GX_PARTICLE_QUADS_H
#define GX_PARTICLE_QUADS_H

#include <cstdint>

struct CGxBuf;

// GPUPARTICLES: one M2 particle billboard quad as the CPU hands it to the Vulkan backend, which expands it on the
// GPU (CGxDeviceVulkan/Shaders/ParticleQuads.comp) into the four vertices CParticleEmitter2::BuildVertex writes:
// corner i = (cx[i] * axisX + cy[i] * axisY) + center with (cx, cy) the stock s_cornerOffsets, texcoord i =
// (u0 | u1, v0 | v1) by the stock s_cornerTexCoords. 56 bytes, against 96 (PCT) or 144 (PNCT) for four vertices.
struct GxParticleQuad {
    float center[3];
    uint32_t color;   // the vertex colour exactly as BuildVertex stores it (CImVector, R/B swapped on GxCF_rgba)
    float axisX[3];
    float u0;
    float axisY[3];
    float v0;
    float u1;
    float v1;
};

static_assert(sizeof(GxParticleQuad) == 56, "ParticleQuads.comp reads 14 words per quad");

// GPUPARTICLES: true while the current device is the Vulkan one and its expansion pipeline is up.
bool GxVkParticleQuadsReady();

// GPUPARTICLES: the vertex stream buf of the next draws holds GxParticleQuad records, not vertices (the draw's
// vertex count is still 4 per quad); normal is written into every vertex of a format with a normal. Null records
// ends it. Main thread, around the draw.
void GxVkParticleQuadsBind(const CGxBuf* records, const float* normal);

#endif
