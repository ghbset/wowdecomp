// clang++ -std=c++17 -I. Tools/GroundDecalCheck/check.cpp -o /tmp/gdc && /tmp/gdc   (from the repo root)
// Checks M2GroundDecal::IsGroundQuad against benilla's GroundQuad cases (models/types.rs).
#include <cassert>
#include <cstdio>
#include "Engine/Source/Model2/M2GroundDecal.h"

struct V { float x, y, z; };

static bool Test(V a, V b, V c, V d) {
    const V* p[4] = { &a, &b, &c, &d };
    return M2GroundDecal::IsGroundQuad(p);
}

int main() {
    assert(Test({-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, {1, 1, 0}));            // ground disc
    assert(Test({1, 1, 0.5f}, {-1, -1, 0.5f}, {1, -1, 0.5f}, {-1, 1, 0.5f}));  // hovering, any order
    assert(!Test({-1, -1, 1.389f}, {1, -1, 1.389f}, {-1, 1, 1.389f}, {1, 1, 1.389f}));  // mid-body glow
    assert(!Test({-1, -1, 0}, {1, -1, 0.2f}, {-1, 1, 0}, {1, 1, 0}));          // not flat
    assert(!Test({-1, 0, -1}, {1, 0, -1}, {-1, 0, 1}, {1, 0, 1}));             // vertical card
    assert(!Test({-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, {-1, 1, 0}));            // two on one corner
    assert(!Test({0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}));               // 45-degree diamond
    assert(!Test({-1, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {1, 0, 0}));               // zero area
    std::puts("ground decal check: ok");
    return 0;
}
