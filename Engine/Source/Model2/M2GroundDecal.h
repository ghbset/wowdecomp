#ifndef ENGINE_SOURCE_MODEL2_M2GROUNDDECAL_H
#define ENGINE_SOURCE_MODEL2_M2GROUNDDECAL_H

// BENILLA: not in 12340. benilla's GroundQuad::ground_quad_hover (benilla-formats models/types.rs): a
// spell batch that is one flat, axis-aligned XY rectangle at model z in [-0.01, 1.0] is ground art
// (Consecration's disc, the paladin aura rings, Blizzard/Flamestrike floors). The stock client draws it
// as a flat plane that hills bury and dips leave floating; CM2Scene routes it through the stock
// projected-texture element (type 1, DrawBatchProj) instead, so it drapes terrain and WMO floors.
// Templated on the vertex type so Tools/GroundDecalCheck/check.cpp can test it without the engine.

#include <cmath>

namespace M2GroundDecal {

constexpr float kFlatEps = 0.01f;  // vertices further than this off the quad's plane disqualify it
constexpr float kHoverMax = 1.0f;  // GreaterHeal_Low_Base's mid-body glow at 1.389 must not drape

// p: the section's 4 model-space positions (.x .y .z).
template <class V>
bool IsGroundQuad(const V* const p[4]) {
    float hover = (p[0]->z + p[1]->z + p[2]->z + p[3]->z) * 0.25f;

    if (!(hover >= -kFlatEps && hover <= kHoverMax)) {
        return false;
    }

    float minX = p[0]->x, maxX = p[0]->x, minY = p[0]->y, maxY = p[0]->y;

    for (int i = 0; i < 4; i++) {
        if (std::fabs(p[i]->z - hover) > kFlatEps) {
            return false;
        }

        minX = std::fmin(minX, p[i]->x);
        maxX = std::fmax(maxX, p[i]->x);
        minY = std::fmin(minY, p[i]->y);
        maxY = std::fmax(maxY, p[i]->y);
    }

    float epsX = (maxX - minX) * 1e-3f + 1e-6f;
    float epsY = (maxY - minY) * 1e-3f + 1e-6f;

    if (maxX - minX <= epsX || maxY - minY <= epsY) {
        return false;  // zero-area
    }

    // Every vertex on its own corner of the XY bounding box.
    int filled = 0;

    for (int i = 0; i < 4; i++) {
        int sx = std::fabs(p[i]->x - minX) <= epsX ? 0 : std::fabs(p[i]->x - maxX) <= epsX ? 1 : -1;
        int sy = std::fabs(p[i]->y - minY) <= epsY ? 0 : std::fabs(p[i]->y - maxY) <= epsY ? 1 : -1;

        if (sx < 0 || sy < 0 || (filled & (1 << (sy * 2 + sx)))) {
            return false;
        }

        filled |= 1 << (sy * 2 + sx);
    }

    return true;
}

}  // namespace M2GroundDecal

#endif
