// VFOG WP1: noise volumes as 2D slice atlases (port of coa-vfog src/noise_volume.cpp @ 4e31ddf).
// Layout (docs/VFOG_PORT.md "Noise atlases"): slice z sits in grid tile (z % cols, z / cols); each tile is
// the slice plus a 1-texel wrap border, so bilinear filtering inside a tile equals wrap addressing.
// Texels are A8R8G8B8 as uint32 0xAARRGGBB (B, G, R, A in memory on little-endian), like D3DFMT_A8R8G8B8.
#include "WoW/Source/WorldClient/VolumetricFogModel.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace VolumetricFog {

namespace {

constexpr uint32_t kDensityNoiseSize = 32;
constexpr uint32_t kAuthoredNoiseSize = 64;

constexpr uint32_t kAuthoredNoiseBaseCells = 4;
constexpr int kAuthoredNoiseDetailLayers = 3;
constexpr float kAuthoredNoiseDetailGain = 0.5f;
constexpr float kByteCentre = 127.5f;
constexpr int kEdgeGradientCount = 12;
constexpr float kEdgeGradients[kEdgeGradientCount][3] = {
    { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 },
    { 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 },
};

// noise_volume.cpp:20-30
uint8_t DensityNoiseValue(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t value = x + kDensityNoiseSize * (y + kDensityNoiseSize * z);
    value = ((value ^ (value >> 7)) * 4051u + 12743u) & 0x7FFFu;
    value ^= value >> 9;
    value = (value * 109u + 583u) & 0x7FFFu;
    value ^= value >> 6;
    value = (value * 29317u + 9419u) & 0x7FFFu;
    value ^= value >> 7;
    return static_cast<uint8_t>(value >> 7);
}

uint32_t LatticeHash(uint32_t x, uint32_t y, uint32_t z, uint32_t detailLayer) {
    uint32_t h = (x * 0x8DA6B343u) ^ (y * 0xD8163841u) ^ (z * 0xCB1AB31Fu) ^ (detailLayer * 0x165667B1u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

float QuinticFade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

constexpr uint32_t kFinestLatticePeriod = kAuthoredNoiseBaseCells << (kAuthoredNoiseDetailLayers - 1);

class GradientLattice {
public:
    GradientLattice(uint32_t period, uint32_t detailLayer) : m_period(period), m_gradients(period * period * period) {
        for (uint32_t z = 0; z < period; ++z) {
            for (uint32_t y = 0; y < period; ++y) {
                for (uint32_t x = 0; x < period; ++x) {
                    this->m_gradients[x + period * (y + period * z)] =
                        kEdgeGradients[LatticeHash(x, y, z, detailLayer) % kEdgeGradientCount];
                }
            }
        }
    }

    uint32_t Period() const {
        return this->m_period;
    }

    const float* const* Row(uint32_t y, uint32_t z) const {
        return &this->m_gradients[this->m_period * (y % this->m_period + this->m_period * (z % this->m_period))];
    }

private:
    uint32_t m_period;
    std::vector<const float*> m_gradients;
};

struct LatticeCoordinate {
    uint32_t cell;
    uint32_t nextCell;
    float offset;
    float fade;
};

std::vector<LatticeCoordinate> TexelLatticeCoordinates(uint32_t period) {
    std::vector<LatticeCoordinate> coordinates(kAuthoredNoiseSize);
    for (uint32_t texel = 0; texel < kAuthoredNoiseSize; ++texel) {
        const float position = (texel + 0.5f) / kAuthoredNoiseSize * period;
        const float floored = std::floor(position);
        const float offset = position - floored;
        const uint32_t cell = static_cast<uint32_t>(floored) % period;
        coordinates[texel] = { cell, (cell + 1) % period, offset, QuinticFade(offset) };
    }
    return coordinates;
}

struct RowGradientTerms {
    float slope = 0.0f;
    float intercept = 0.0f;
};

void AddGradientNoiseRow(const GradientLattice& lattice, const std::vector<LatticeCoordinate>& coordinates, uint32_t y,
                         uint32_t z, float amplitude, float* row) {
    const uint32_t period = lattice.Period();
    const LatticeCoordinate& along = coordinates[y];
    const LatticeCoordinate& across = coordinates[z];
    RowGradientTerms terms[kFinestLatticePeriod];
    for (uint32_t corner = 0; corner < 4; ++corner) {
        const uint32_t stepY = corner & 1u;
        const uint32_t stepZ = corner >> 1;
        const float weight = (stepY ? along.fade : 1.0f - along.fade) * (stepZ ? across.fade : 1.0f - across.fade);
        const float offsetY = along.offset - static_cast<float>(stepY);
        const float offsetZ = across.offset - static_cast<float>(stepZ);
        const float* const* gradients = lattice.Row(along.cell + stepY, across.cell + stepZ);
        for (uint32_t x = 0; x < period; ++x) {
            const float* g = gradients[x];
            terms[x].slope += weight * g[0];
            terms[x].intercept += weight * (g[1] * offsetY + g[2] * offsetZ);
        }
    }
    for (uint32_t x = 0; x < kAuthoredNoiseSize; ++x) {
        const LatticeCoordinate& c = coordinates[x];
        const RowGradientTerms& left = terms[c.cell];
        const RowGradientTerms& right = terms[c.nextCell];
        const float leftValue = left.slope * c.offset + left.intercept;
        const float rightValue = right.slope * (c.offset - 1.0f) + right.intercept;
        row[x] += amplitude * (leftValue + (rightValue - leftValue) * c.fade);
    }
}

std::vector<float> TileableGradientDetail() {
    std::vector<float> values(kAuthoredNoiseSize * kAuthoredNoiseSize * kAuthoredNoiseSize, 0.0f);
    float amplitude = 1.0f;
    for (uint32_t layer = 0; layer < kAuthoredNoiseDetailLayers; ++layer) {
        const GradientLattice lattice(kAuthoredNoiseBaseCells << layer, layer);
        const std::vector<LatticeCoordinate> coordinates = TexelLatticeCoordinates(lattice.Period());
        for (uint32_t z = 0; z < kAuthoredNoiseSize; ++z) {
            for (uint32_t y = 0; y < kAuthoredNoiseSize; ++y) {
                AddGradientNoiseRow(lattice, coordinates, y, z, amplitude,
                                    &values[kAuthoredNoiseSize * (y + kAuthoredNoiseSize * z)]);
            }
        }
        amplitude *= kAuthoredNoiseDetailGain;
    }
    return values;
}

uint8_t NearestByte(float nonNegative) {
    return static_cast<uint8_t>(std::clamp(static_cast<int>(nonNegative + 0.5f), 0, 255));
}

std::vector<uint8_t> QuantizedAroundMedian(const std::vector<float>& values) {
    std::vector<float> sorted = values;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float median = sorted[sorted.size() / 2];
    float extent = 0.0f;
    for (float value : values) {
        extent = std::max(extent, std::fabs(value - median));
    }
    std::vector<uint8_t> bytes(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        const float centred = extent > 0.0f ? (values[i] - median) / extent : 0.0f;
        bytes[i] = NearestByte(kByteCentre + kByteCentre * centred);
    }
    return bytes;
}

// The 2D atlas of an n^3 volume: cols x (n / cols) tiles of (n + 2)^2 texels. texel(x, y, z) -> A8R8G8B8.
template <typename Texel>
std::vector<uint32_t> SliceAtlas(uint32_t n, uint32_t cols, Texel texel) {
    const uint32_t tile = n + 2;
    const uint32_t width = cols * tile;
    const uint32_t height = (n / cols) * tile;
    std::vector<uint32_t> pixels(width * height);
    for (uint32_t z = 0; z < n; ++z) {
        const uint32_t originX = (z % cols) * tile;
        const uint32_t originY = (z / cols) * tile;
        for (uint32_t j = 0; j < tile; ++j) {
            const uint32_t y = (j + n - 1) % n;     // border texel = wrapped neighbour of the same slice
            for (uint32_t i = 0; i < tile; ++i) {
                const uint32_t x = (i + n - 1) % n;
                pixels[(originY + j) * width + originX + i] = texel(x, y, z);
            }
        }
    }
    return pixels;
}

uint32_t GreyArgb(uint8_t value) {
    return 0xFF000000u | (value * 0x00010101u);
}

}  // namespace

// coa CreateDensityNoise: grey A8R8G8B8 (value in R, G, B; alpha 255), 32^3 -> 8x4 tiles of 34^2 = 272x136.
const NoiseAtlas& DensityNoiseAtlas() {
    static const std::vector<uint32_t> pixels =
        SliceAtlas(kDensityNoiseSize, 8, [](uint32_t x, uint32_t y, uint32_t z) { return GreyArgb(DensityNoiseValue(x, y, z)); });
    static const NoiseAtlas atlas = { 8 * (kDensityNoiseSize + 2), 4 * (kDensityNoiseSize + 2),
                                      reinterpret_cast<const uint8_t*>(pixels.data()) };
    return atlas;
}

// coa CreateAuthoredNoise: 3-octave tileable gradient noise, quantised around its median; the value goes in
// every channel (coa's L8 replicates into RGB with alpha 1; here alpha carries it too). 64^3 -> 528x528.
const NoiseAtlas& AuthoredNoiseAtlas() {
    static const std::vector<uint8_t> texels = QuantizedAroundMedian(TileableGradientDetail());
    static const std::vector<uint32_t> pixels =
        SliceAtlas(kAuthoredNoiseSize, 8, [](uint32_t x, uint32_t y, uint32_t z) {
            return texels[x + kAuthoredNoiseSize * (y + kAuthoredNoiseSize * z)] * 0x01010101u;
        });
    static const NoiseAtlas atlas = { 8 * (kAuthoredNoiseSize + 2), 8 * (kAuthoredNoiseSize + 2),
                                      reinterpret_cast<const uint8_t*>(pixels.data()) };
    return atlas;
}

}  // namespace VolumetricFog
