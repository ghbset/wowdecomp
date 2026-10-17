// VFOG water waves: coa-vfog's Forever FFT ocean (src/water_spectrum.cpp, the CPU reference path its GPU FFT is
// tested against; shaders/vw_fft_*.hlsl) run on the CPU, because the Gx API has no RGBA16F/32F targets and no
// render-to-mip. Same spectra, tile parameters, loop clock, Hermitian fold, 1/256^2 normalisation, surface stencil,
// foam/oxygen persistence and box-filtered mips as coa; float instead of double. docs/VFOG_PORT.md "Water".
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <thread>

namespace VolumetricFog {
namespace {

using Complex = std::complex<float>;

// coa water_spectrum.h / .cpp
constexpr double kGravity = 9.80665;
constexpr double kForeverInverseGravity = 0.10197161883115768;
constexpr double kTwoPi = 6.2831854820251465;
constexpr double kLoopSeconds = 1024.0;
constexpr double kSmallestWaveNumber = 1.0e-3;
constexpr double kFlatWaveNumber = 1.0e-16;
constexpr double kLargestInversePeakLengthSquared = 1.0e30;
constexpr double kHashDotX = 12.9898;
constexpr double kHashDotY = 78.233;
constexpr double kHashScale = 43758.546875;
constexpr double kSmallestHash = 1.0e-16;
constexpr double kTowardsRadiusOffset = 0.13;
constexpr double kTowardsAngleOffset = 0.07;
constexpr double kAwayRadiusOffset = 0.29;
constexpr double kAwayAngleOffset = 0.19;
constexpr int kReferenceResolution = 256;          // coa kFftReferenceResolution: the normalisation of every resolution
constexpr float kWindDirection[2] = { 0.8f, 0.6f }; // coa water_renderer.cpp kWindDirection
constexpr double kMaxFoamStepSeconds = 0.1;        // coa kMaxFoamStepSeconds
// vw_surface.hlsli SampleMoments decodes with the same ranges.
constexpr float kVarianceRange = 0.25f;
constexpr float kWideRange = 1.0f;

double Fract(double v) {
    return v - std::floor(v);
}

double NoiseHash(double u, double v) {
    const double hash = Fract(std::sin(u * kHashDotX + v * kHashDotY) * kHashScale);
    return std::min(std::max(hash, kSmallestHash), 1.0);
}

std::complex<double> Gaussian(double u, double v, double radiusOffset, double angleOffset) {
    const double radius = std::sqrt(-2.0 * std::log(NoiseHash(u + radiusOffset, v + radiusOffset)));
    const double angle = kTwoPi * NoiseHash(u + angleOffset, v + angleOffset);
    return { radius * std::cos(angle), radius * std::sin(angle) };
}

uint16_t Half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + (1u << (shift - 1))) >> shift));
    }
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7BFFu);     // clamp to the largest finite half
    }
    const uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    return static_cast<uint16_t>(half + ((mantissa >> 12) & 1u));   // round half up (may carry into the exponent)
}

uint32_t Byte(float v) {
    return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// One tile's simulation (coa WaterReferenceDisplacement + WaterReferenceSurface + vw_fft_surface FOAM).
struct Tile {
    WaterFftTile params = {};
    WaveSettings settings = {};
    bool configured = false;
    double lastSeconds = -1.0;
    std::vector<Complex> h0Towards;     // coa WaterForeverAmplitude, cached per settings
    std::vector<float> h0AwayReal;
    std::vector<float> loopFrequency;   // coa WaterFrequencyIndex per texel
    std::vector<float> kx;              // per column / row wave numbers over |k|
    std::vector<Complex> heightAndX;
    std::vector<Complex> alongY;
    std::vector<float> disp;            // x, y, up per texel
    std::vector<float> foam;
    std::vector<float> oxygen;
    std::vector<float> moments;         // 4 per texel per level
    WaveMaps work;
    WaveMaps shown;
    bool shownValid = false;
};

Tile s_tiles[kWaterMaxTiles];
std::thread* s_batch = nullptr;     // never destroyed: a joinable std::thread at exit would std::terminate
std::atomic<bool> s_busy{ false };
uint32_t s_runningMask = 0;

double SpectrumAmplitude(double kx, double ky, double amplitude, double inversePeak2, double alignment, const double wind[2]) {
    const double squared = kx * kx + ky * ky;
    const double magnitude = std::sqrt(squared);
    if (magnitude < kSmallestWaveNumber) {
        return 0.0;
    }
    const double alongWind = (kx * wind[0] + ky * wind[1]) / magnitude;
    if (!(alongWind > 0.0)) {
        return 0.0;
    }
    const double power = amplitude / (squared * squared) * std::exp(-inversePeak2 / squared) * std::pow(alongWind, alignment);
    return std::sqrt(power) / std::sqrt(2.0);
}

void Configure(Tile& t) {
    const int n = t.settings.resolution;
    const size_t texels = static_cast<size_t>(n) * n;
    const double length = std::max(1.0, std::floor(static_cast<double>(t.params.size)));
    const double amplitude = static_cast<double>(t.params.amplitude) * t.settings.amplitudeScale;
    const double wind = static_cast<double>(t.params.windMultiplier) * t.settings.windSpeed;
    const double peakLength = wind * wind * kForeverInverseGravity;
    const double inversePeak2 = peakLength > 0.0 ? std::min(1.0 / (peakLength * peakLength), kLargestInversePeakLengthSquared) : kLargestInversePeakLengthSquared;
    const double windLength = std::hypot(kWindDirection[0], kWindDirection[1]);
    const double windDir[2] = { kWindDirection[0] / windLength, kWindDirection[1] / windLength };
    const float loopPerRootRadius = static_cast<float>(kLoopSeconds * std::sqrt(kGravity / (kTwoPi * length)));

    t.h0Towards.assign(texels, Complex());
    t.h0AwayReal.assign(texels, 0.0f);
    t.loopFrequency.assign(texels, 0.0f);
    t.kx.assign(n, 0.0f);
    for (int i = 0; i < n; i++) {
        t.kx[i] = static_cast<float>(kTwoPi * (i - 0.5 * n) / length);
    }
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            const size_t i = static_cast<size_t>(y) * n + x;
            const double u = static_cast<double>(x) / n;
            const double v = static_cast<double>(y) / n;
            const std::complex<double> towards = Gaussian(u, v, kTowardsRadiusOffset, kTowardsAngleOffset);
            const double away = Gaussian(u, v, kAwayRadiusOffset, kAwayAngleOffset).real();
            const double kx = t.kx[x];
            const double ky = t.kx[y];
            t.h0Towards[i] = Complex(towards * SpectrumAmplitude(kx, ky, amplitude, inversePeak2, t.params.windAlignment, windDir));
            t.h0AwayReal[i] = static_cast<float>(away * SpectrumAmplitude(-kx, -ky, amplitude, inversePeak2, t.params.windAlignment, windDir));
            const double cx = x - 0.5 * n;
            const double cy = y - 0.5 * n;
            const float rootRadius = static_cast<float>(std::sqrt(std::sqrt(cx * cx + cy * cy)));
            t.loopFrequency[i] = std::floor(rootRadius * loopPerRootRadius + 0.5f);
        }
    }
    t.heightAndX.assign(texels, Complex());
    t.alongY.assign(texels, Complex());
    t.disp.assign(texels * 3, 0.0f);
    t.foam.assign(texels, 0.0f);
    t.oxygen.assign(texels, 0.0f);
    t.lastSeconds = -1.0;
    t.configured = true;
}

// In-place inverse DFT of one line (sum x_n e^{+2 pi i n k / N}), coa's butterfly convention.
void TransformLine(Complex* line, int n, const Complex* twiddles) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(line[i], line[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int step = n / len;
        const int halfLen = len >> 1;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < halfLen; k++) {
                const Complex a = line[i + k];
                const Complex b = line[i + k + halfLen] * twiddles[k * step];
                line[i + k] = a + b;
                line[i + k + halfLen] = a - b;
            }
        }
    }
}

void Transform2D(std::vector<Complex>& grid, int n, const std::vector<Complex>& twiddles, std::vector<Complex>& column) {
    for (int y = 0; y < n; y++) {
        TransformLine(&grid[static_cast<size_t>(y) * n], n, twiddles.data());
    }
    column.resize(n);
    for (int x = 0; x < n; x++) {
        for (int y = 0; y < n; y++) {
            column[y] = grid[static_cast<size_t>(y) * n + x];
        }
        TransformLine(column.data(), n, twiddles.data());
        for (int y = 0; y < n; y++) {
            grid[static_cast<size_t>(y) * n + x] = column[y];
        }
    }
}

void Step(Tile& t, double seconds) {
    const int n = t.settings.resolution;
    const size_t texels = static_cast<size_t>(n) * n;
    const double loopFraction = Fract(seconds / kLoopSeconds);

    // Evolve + fold (coa WaterReferenceModes + WaterFoldModes): height and the choppy x/y modes, Hermitian halves.
    std::vector<Complex> height(texels);
    for (size_t i = 0; i < texels; i++) {
        const double phase = kTwoPi * Fract(t.loopFrequency[i] * loopFraction);
        const Complex rotation(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
        const Complex away(t.h0AwayReal[i], t.h0Towards[i].imag());
        height[i] = t.h0Towards[i] * rotation + std::conj(away) * std::conj(rotation);
    }
    const Complex unit(0.0f, 1.0f);
    for (int y = 0; y < n; y++) {
        const int my = (n - y) % n;
        for (int x = 0; x < n; x++) {
            const int mx = (n - x) % n;
            const size_t at = static_cast<size_t>(y) * n + x;
            const size_t mirror = static_cast<size_t>(my) * n + mx;
            auto direction = [&](int cx, int cy, float& dx, float& dy) {
                const float kx = t.kx[cx];
                const float ky = t.kx[cy];
                const float divisor = static_cast<float>(std::max(std::sqrt(static_cast<double>(kx) * kx + static_cast<double>(ky) * ky), kFlatWaveNumber));
                dx = kx / divisor;
                dy = ky / divisor;
            };
            float ax, ay, bx, by;
            direction(x, y, ax, ay);
            direction(mx, my, bx, by);
            const Complex h = height[at];
            const Complex hm = height[mirror];
            const Complex foldH = 0.5f * (h + std::conj(hm));
            const Complex foldX = 0.5f * (Complex(0.0f, -ax) * h + std::conj(Complex(0.0f, -bx) * hm));
            const Complex foldY = 0.5f * (Complex(0.0f, -ay) * h + std::conj(Complex(0.0f, -by) * hm));
            t.heightAndX[at] = foldH + unit * foldX;
            t.alongY[at] = foldY;
        }
    }

    std::vector<Complex> twiddles(n / 2);
    for (int k = 0; k < n / 2; k++) {
        const double angle = 6.283185307179586 * k / n;
        twiddles[k] = Complex(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
    }
    std::vector<Complex> column;
    Transform2D(t.heightAndX, n, twiddles, column);
    Transform2D(t.alongY, n, twiddles, column);

    // coa WaterAssembleDisplacement.
    const float area = static_cast<float>(kReferenceResolution) * kReferenceResolution;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            const size_t i = static_cast<size_t>(y) * n + x;
            const float scale = ((x + y) & 1 ? -1.0f : 1.0f) / area;
            t.disp[i * 3 + 0] = scale * t.heightAndX[i].imag();
            t.disp[i * 3 + 1] = scale * t.alongY[i].real();
            t.disp[i * 3 + 2] = -scale * t.heightAndX[i].real();
        }
    }

    // Surface moments + Jacobian (coa WaterReferenceSurface / vw_fft_surface) and the foam step (FoamStep).
    const double length = std::max(1.0, std::floor(static_cast<double>(t.params.size)));
    const double texelsPerUnit = n / length;
    const int wide = std::min(std::max(static_cast<int>(std::lround(2.0 * texelsPerUnit)), 1), n / 2);
    const float half = static_cast<float>(0.5 * texelsPerUnit);
    const float inverseWide = static_cast<float>(texelsPerUnit / (2.0 * wide));
    const float dt = t.lastSeconds < 0.0 ? 0.0f : static_cast<float>(std::clamp(seconds - t.lastSeconds, 0.0, kMaxFoamStepSeconds));
    t.lastSeconds = seconds;

    WaveMaps& m = t.work;
    m.resolution = static_cast<uint32_t>(n);
    m.levels = 0;
    size_t total = 0;
    for (uint32_t side = n; ; side >>= 1) {
        m.offset[m.levels++] = static_cast<uint32_t>(total);
        total += static_cast<size_t>(side) * side;
        if (side == 1) {
            break;
        }
    }
    m.slopes.resize(total);
    m.state.resize(total);
    t.moments.resize(total * 4);

    auto at = [&](int x, int y) -> const float* {
        x = (x % n + n) % n;
        y = (y % n + n) % n;
        return &t.disp[(static_cast<size_t>(y) * n + x) * 3];
    };
    auto foamStep = [dt](float previous, float jacobian, const float p[3]) {
        const float f = std::clamp(previous, 0.0f, 1.0f);
        const float injection = std::max(p[0] + 1.0f - jacobian, 0.0f);
        return std::clamp(f + dt * (p[1] * injection * (1.0f - f) - p[2] * f), 0.0f, 1.0f);
    };
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            const float* ax = at(x + 1, y);
            const float* bx = at(x - 1, y);
            const float* ay = at(x, y + 1);
            const float* by = at(x, y - 1);
            const float dXx = (ax[0] - bx[0]) * half, dYx = (ax[1] - bx[1]) * half, dUx = (ax[2] - bx[2]) * half;
            const float dXy = (ay[0] - by[0]) * half, dYy = (ay[1] - by[1]) * half, dUy = (ay[2] - by[2]) * half;
            const float wideX = (at(x + wide, y)[2] - at(x - wide, y)[2]) * inverseWide;
            const float wideY = (at(x, y + wide)[2] - at(x, y - wide)[2]) * inverseWide;
            const float jacobian = (1.0f + dXx) * (1.0f + dYy) - dXy * dYx;
            const size_t i = static_cast<size_t>(y) * n + x;
            t.foam[i] = foamStep(t.foam[i], jacobian, t.params.foam);
            t.oxygen[i] = foamStep(t.oxygen[i], jacobian, t.params.oxygen);
            float* mo = &t.moments[i * 4];
            mo[0] = dUx;
            mo[1] = dUy;
            mo[2] = dUx * dUx + dUy * dUy;
            mo[3] = wideX * wideX + wideY * wideY;
        }
    }
    // coa vw_fft_mip: box average of the moments level by level.
    for (uint32_t level = 1; level < m.levels; level++) {
        const uint32_t side = static_cast<uint32_t>(n) >> level;
        const float* src = &t.moments[static_cast<size_t>(m.offset[level - 1]) * 4];
        float* dst = &t.moments[static_cast<size_t>(m.offset[level]) * 4];
        const uint32_t srcSide = side * 2;
        for (uint32_t y = 0; y < side; y++) {
            for (uint32_t x = 0; x < side; x++) {
                for (int c = 0; c < 4; c++) {
                    dst[(y * side + x) * 4 + c] = 0.25f * (src[((2 * y) * srcSide + 2 * x) * 4 + c] + src[((2 * y) * srcSide + 2 * x + 1) * 4 + c]
                        + src[((2 * y + 1) * srcSide + 2 * x) * 4 + c] + src[((2 * y + 1) * srcSide + 2 * x + 1) * 4 + c]);
                }
            }
        }
    }
    for (uint32_t level = 0; level < m.levels; level++) {
        const uint32_t side = static_cast<uint32_t>(n) >> level;
        for (uint32_t i = 0; i < side * side; i++) {
            const size_t o = m.offset[level] + i;
            const float* mo = &t.moments[o * 4];
            const float variance = std::max(mo[2] - (mo[0] * mo[0] + mo[1] * mo[1]), 0.0f);
            m.slopes[o] = static_cast<uint32_t>(Half(mo[0])) | (static_cast<uint32_t>(Half(mo[1])) << 16);
            // The foam state is read at level 0 only (coa's foam maps have no mips).
            const float foam = level == 0 ? t.foam[i] : 0.0f;
            const float oxygen = level == 0 ? t.oxygen[i] : 0.0f;
            m.state[o] = (Byte(oxygen) << 24) | (Byte(std::sqrt(variance / kVarianceRange)) << 16)
                | (Byte(std::sqrt(mo[3] / kWideRange)) << 8) | Byte(foam);
        }
    }
}

}  // namespace

void WaveLaunch(const std::vector<WaterFftTile>& tiles, uint32_t tileMask, const WaveSettings& settings, double seconds) {
    if (s_busy.load() || !tileMask) {
        return;
    }
    if (s_batch) {
        s_batch->join();
        delete s_batch;
        s_batch = nullptr;
    }
    uint32_t running = 0;
    for (int i = 0; i < kWaterMaxTiles && i < static_cast<int>(tiles.size()); i++) {
        if (!(tileMask & (1u << i))) {
            continue;
        }
        Tile& t = s_tiles[i];
        if (!t.configured || std::memcmp(&t.params, &tiles[i], sizeof(WaterFftTile)) != 0 || t.settings.resolution != settings.resolution
            || t.settings.windSpeed != settings.windSpeed || t.settings.amplitudeScale != settings.amplitudeScale) {
            t.params = tiles[i];
            t.settings = settings;
            t.configured = false;
            t.shownValid = false;
        }
        running |= 1u << i;
    }
    if (!running) {
        return;
    }
    s_runningMask = running;
    s_busy.store(true);
    s_batch = new std::thread([running, seconds]() {
        std::thread workers[kWaterMaxTiles];
        for (int i = 0; i < kWaterMaxTiles; i++) {
            if (running & (1u << i)) {
                workers[i] = std::thread([i, seconds]() {
                    Tile& t = s_tiles[i];
                    if (!t.configured) {
                        Configure(t);
                    }
                    Step(t, seconds);
                });
            }
        }
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        s_busy.store(false);
    });
}

bool WaveCollect() {
    if (!s_batch || s_busy.load()) {
        return false;
    }
    s_batch->join();
    delete s_batch;
    s_batch = nullptr;
    for (int i = 0; i < kWaterMaxTiles; i++) {
        if (s_runningMask & (1u << i)) {
            std::swap(s_tiles[i].work, s_tiles[i].shown);
            s_tiles[i].shownValid = true;
        }
    }
    s_runningMask = 0;
    return true;
}

const WaveMaps* WaveMapsOf(int tile) {
    return tile >= 0 && tile < kWaterMaxTiles && s_tiles[tile].shownValid ? &s_tiles[tile].shown : nullptr;
}

void WaveReset() {
    if (s_batch) {
        s_batch->join();
        delete s_batch;
        s_batch = nullptr;
        s_busy.store(false);
    }
    s_runningMask = 0;
    for (Tile& t : s_tiles) {
        t.configured = false;
        t.shownValid = false;
    }
}

}  // namespace VolumetricFog
