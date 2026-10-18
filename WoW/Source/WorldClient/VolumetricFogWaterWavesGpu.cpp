// VFOG water waves on the GPU (vfogWaterGpuWaves, default): coa-vfog's GPU FFT ocean (src/water_fft.cpp,
// shaders/vw_fft_*.hlsl) as off-screen Gx passes, matching the CPU path (VolumetricFogWaterWaves.cpp, the fallback)
// texel for texel within half precision: Tools/CustomMpq/check_gpu_waves.py replays both. docs/VFOG_PORT.md "Waves".
//
// Gx has RGBA16F (no 32F, no MRT), so each FFT texel holds one complex value split hi + lo across its four halves
// (~2^-20 relative), heightAndX and alongY run as two chains, and every butterfly stage halves (no overflow).
// Per tile per step, n = 128 / 256:
//   2 x (VFogWaveEvolve + 2 log2 n VFogWaveFft)   h0 -> evolve + fold (bit-reversed) -> row then column stages
//   VFogWaveSurface x 2                           moments level 0 (scratch), foam/oxygen step (per-tile ping-pong)
//   VFogWaveMip x log2 n                          box-filtered moments into per-level scratch targets
//   VFogWavePack x 2 (log2 n + 1)                 every level of the tile's final slopes (RGBA16F) and state
//                                                 (Argb8888) maps, rendered into their mips (GxRenderTargetSet level)
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"

namespace VolumetricFog {
namespace {

constexpr double kLoopSeconds = 1024.0;
constexpr double kMaxFoamStepSeconds = 0.1;     // coa kMaxFoamStepSeconds, as the CPU path
constexpr int kMaxLevels = 9;                   // 256 -> 1

struct GpuTile {
    WaterFftTile params = {};
    WaveSettings settings = {};
    bool configured = false;
    WaveSpectrum spectrum;
    CGxTex* h0 = nullptr;
    CGxTex* h0Low = nullptr;
    CGxTex* foam[2] = {};
    int foamCurrent = 0;
    bool foamValid = false;
    double lastSeconds = -1.0;
    CGxTex* slopes = nullptr;
    CGxTex* state = nullptr;
    bool mapsValid = false;
};

struct Gpu {
    CGxShader* evolve = nullptr;
    CGxShader* fft = nullptr;
    CGxShader* surface = nullptr;
    CGxShader* mip = nullptr;
    CGxShader* pack = nullptr;
    bool unsupported = false;
    int resolution = 0;
    CGxTex* chain[3] = {};          // FFT ping-pong: [0] heightAndX, [2] alongY, [1] scratch
    CGxTex* moments[kMaxLevels] = {};
    GpuTile tiles[kWaterMaxTiles];
};

Gpu s_gpu;

void Destroy(CGxTex*& tex) {
    if (tex) {
        GxTexDestroy(tex);
        tex = nullptr;
    }
}

void H0Callback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& stride, const void*& texels) {
    // userArg = the tile's spectrum vector (h0 or h0Low)
    const std::vector<uint64_t>* data = static_cast<const std::vector<uint64_t>*>(userArg);
    if (command == GxTex_Latch && mipLevel == 0) {
        texels = data->data();
        stride = width * 8;
    }
}

CGxTex* Create(uint32_t side, EGxTexFormat format, EGxTexFilter filter, bool target, void* arg = nullptr, GxTexUserFunc func = nullptr) {
    // Wrap (the surface stencil wraps); a mip filter on a render target = a full chain rendered level by level (Gx.h).
    CGxTex* tex = nullptr;
    CGxTexFlags flags(filter, true, true, 0, 0, target, 1, 0, 0, 0);
    g_theGxDevicePtr->TexCreate(GxTex_2d, side, side, 0, format, format, flags, arg, func ? func : GxTexNullUserFunc, "VFogWaterGpuWaves", tex);
    return tex;
}

void DestroyTile(GpuTile& t) {
    Destroy(t.h0);
    Destroy(t.h0Low);
    Destroy(t.foam[0]);
    Destroy(t.foam[1]);
    Destroy(t.slopes);
    Destroy(t.state);
    t.configured = false;
    t.foamValid = false;
    t.mapsValid = false;
}

void DestroyAll() {
    for (CGxTex*& tex : s_gpu.chain) {
        Destroy(tex);
    }
    for (CGxTex*& tex : s_gpu.moments) {
        Destroy(tex);
    }
    for (GpuTile& t : s_gpu.tiles) {
        DestroyTile(t);
    }
    s_gpu.resolution = 0;
}

CGxShader* Load(CGxShader*& slot, const char* name) {
    if (!slot) {
        g_theGxDevicePtr->ShaderCreate(&slot, GxSh_Pixel, "Shaders\\Pixel", name, 1);
    }
    return slot && slot->Valid() ? slot : nullptr;
}

bool Prepare(int n) {
    CGxDevice* device = g_theGxDevicePtr;
    if (s_gpu.unsupported) {
        return false;
    }
    if (!device->Caps().m_rttAbgr16161616F || !device->Caps().m_rttFormat[GxTex_Argb8888] || !Load(s_gpu.evolve, "VFogWaveEvolve")
        || !Load(s_gpu.fft, "VFogWaveFft") || !Load(s_gpu.surface, "VFogWaveSurface") || !Load(s_gpu.mip, "VFogWaveMip") || !Load(s_gpu.pack, "VFogWavePack")) {
        s_gpu.unsupported = true;
        return false;
    }
    if (s_gpu.resolution == n) {
        return true;
    }
    DestroyAll();
    bool ok = true;
    for (CGxTex*& tex : s_gpu.chain) {
        ok = ok && (tex = Create(n, GxTex_Abgr16161616F, GxTex_Nearest, true));
    }
    for (int level = 0; (n >> level) >= 1; level++) {
        ok = ok && (s_gpu.moments[level] = Create(n >> level, GxTex_Abgr16161616F, GxTex_Nearest, true));
    }
    if (!ok) {
        DestroyAll();
        s_gpu.unsupported = true;
        return false;
    }
    s_gpu.resolution = n;
    return true;
}

bool PrepareTile(GpuTile& t, const WaterFftTile& params, const WaveSettings& settings) {
    if (t.configured && std::memcmp(&t.params, &params, sizeof(WaterFftTile)) == 0 && t.settings.resolution == settings.resolution
        && t.settings.windSpeed == settings.windSpeed && t.settings.amplitudeScale == settings.amplitudeScale) {
        return true;
    }
    DestroyTile(t);
    t.params = params;
    t.settings = settings;
    ComputeWaveSpectrum(params, settings, t.spectrum);
    const uint32_t n = static_cast<uint32_t>(settings.resolution);
    t.h0 = Create(n, GxTex_Abgr16161616F, GxTex_Nearest, false, &t.spectrum.h0, H0Callback);
    t.h0Low = Create(n, GxTex_Abgr16161616F, GxTex_Nearest, false, &t.spectrum.h0Low, H0Callback);
    t.foam[0] = Create(n, GxTex_Abgr16161616F, GxTex_Nearest, true);
    t.foam[1] = Create(n, GxTex_Abgr16161616F, GxTex_Nearest, true);
    t.slopes = Create(n, GxTex_Abgr16161616F, GxTex_LinearMipLinear, true);
    t.state = Create(n, GxTex_Argb8888, GxTex_LinearMipLinear, true);
    if (!t.h0 || !t.h0Low || !t.foam[0] || !t.foam[1] || !t.slopes || !t.state) {
        DestroyTile(t);
        return false;
    }
    GxTexUpdate(t.h0, 0, 0, static_cast<int32_t>(n), static_cast<int32_t>(n), 1);
    GxTexUpdate(t.h0Low, 0, 0, static_cast<int32_t>(n), static_cast<int32_t>(n), 1);
    t.lastSeconds = -1.0;
    t.configured = true;
    return true;
}

void Row(uint32_t reg, float x, float y = 0.0f, float z = 0.0f, float w = 0.0f) {
    const float row[4] = { x, y, z, w };
    SetPs(reg, row, 1);
}

void Pass(CGxShader* shader, CGxTex* target, uint32_t level, uint32_t side) {
    GxRenderTargetSet(GxBuffers_Color, target, level);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, shader);
    DrawQuad(side, side);
}

// Evolve one pair into chain[out], then its row and column stages ping-ponging with chain[1]; returns the result.
CGxTex* Transform(const GpuTile& t, int n, int pair, int out, float loopFraction) {
    const float fn = static_cast<float>(n);
    Row(0, fn, 1.0f / fn, t.spectrum.loopPerRootRadius, loopFraction);
    Row(1, static_cast<float>(pair));
    Bind(0, t.h0);
    Bind(1, t.h0Low);
    Pass(s_gpu.evolve, s_gpu.chain[out], 0, n);
    Bind(1, nullptr);
    CGxTex* source = s_gpu.chain[out];
    CGxTex* other = s_gpu.chain[1];
    for (int axis = 0; axis < 2; axis++) {
        Row(1, axis == 0 ? 1.0f : 0.0f, axis == 0 ? 0.0f : 1.0f);
        for (int length = 2; length <= n; length <<= 1) {
            Row(0, fn, 1.0f / fn, static_cast<float>(length), 1.0f / length);
            Bind(0, source);
            Pass(s_gpu.fft, other, 0, n);
            std::swap(source, other);
        }
    }
    Bind(0, nullptr);
    return source;      // 2 log2 n stages (even): back in chain[out]
}

void StepTile(GpuTile& t, double seconds) {
    const int n = t.settings.resolution;
    const float fn = static_cast<float>(n);
    const float loopFraction = static_cast<float>(seconds / kLoopSeconds - std::floor(seconds / kLoopSeconds));
    CGxTex* heightAndX = Transform(t, n, 0, 0, loopFraction);
    CGxTex* alongY = Transform(t, n, 1, 2, loopFraction);

    const float dt = t.lastSeconds < 0.0 ? 0.0f : static_cast<float>(std::clamp(seconds - t.lastSeconds, 0.0, kMaxFoamStepSeconds));
    t.lastSeconds = seconds;
    const WaveSpectrum& s = t.spectrum;
    Row(1, s.halfTexelsPerUnit, static_cast<float>(s.wide), s.inverseWide, dt);
    Row(2, t.params.foam[0], t.params.foam[1], t.params.foam[2], t.foamValid ? 1.0f : 0.0f);
    Row(3, t.params.oxygen[0], t.params.oxygen[1], t.params.oxygen[2]);
    Bind(0, heightAndX);
    Bind(1, alongY);
    Bind(2, t.foam[t.foamCurrent]);
    const float exponentScale = std::ldexp(1.0f, s.exponent);
    Row(0, fn, 1.0f / fn, exponentScale, 0.0f);
    Pass(s_gpu.surface, s_gpu.moments[0], 0, n);
    Row(0, fn, 1.0f / fn, exponentScale, 1.0f);
    Pass(s_gpu.surface, t.foam[1 - t.foamCurrent], 0, n);
    t.foamCurrent = 1 - t.foamCurrent;
    t.foamValid = true;
    Bind(1, nullptr);
    Bind(2, nullptr);

    int levels = 1;
    for (int side = n >> 1; side >= 1; side >>= 1, levels++) {
        Row(0, static_cast<float>(side), 0.5f / side);
        Bind(0, s_gpu.moments[levels - 1]);
        Pass(s_gpu.mip, s_gpu.moments[levels], 0, side);
    }
    Bind(1, t.foam[t.foamCurrent]);
    for (int level = 0; level < levels; level++) {
        const uint32_t side = static_cast<uint32_t>(n) >> level;
        Bind(0, s_gpu.moments[level]);
        Row(0, static_cast<float>(side), 1.0f / side, 0.0f, level == 0 ? 1.0f : 0.0f);
        Pass(s_gpu.pack, t.slopes, level, side);
        Row(0, static_cast<float>(side), 1.0f / side, 1.0f, level == 0 ? 1.0f : 0.0f);
        Pass(s_gpu.pack, t.state, level, side);
    }
    Bind(0, nullptr);
    Bind(1, nullptr);
    t.mapsValid = true;
}

}  // namespace

bool WaveGpuStep(const std::vector<WaterFftTile>& tiles, uint32_t tileMask, const WaveSettings& settings, double seconds, const WorldViewport& vp) {
    if (!g_theGxDevicePtr || !Prepare(settings.resolution)) {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < kWaterMaxTiles && i < static_cast<int>(tiles.size()) && ok; i++) {
        if (tileMask & (1u << i)) {
            ok = PrepareTile(s_gpu.tiles[i], tiles[i], settings);
        }
    }
    if (!ok) {
        WaveGpuRelease();
        s_gpu.unsupported = true;
        return false;
    }
    static SavedPassState s_saved;
    BeginPasses(s_saved);
    for (int i = 0; i < kWaterMaxTiles && i < static_cast<int>(tiles.size()); i++) {
        if (tileMask & (1u << i)) {
            StepTile(s_gpu.tiles[i], seconds);
        }
    }
    GxRenderTargetSet(GxBuffers_Color, vp.target, 0);
    EndPasses(s_saved, vp);
    return true;
}

bool WaveGpuMaps(int tile, CGxTex*& slopes, CGxTex*& state) {
    if (tile < 0 || tile >= kWaterMaxTiles || !s_gpu.tiles[tile].mapsValid) {
        return false;
    }
    slopes = s_gpu.tiles[tile].slopes;
    state = s_gpu.tiles[tile].state;
    return true;
}

void WaveGpuRelease() {
    DestroyAll();
}

void WaveGpuShutdown() {
    DestroyAll();
    if (g_theGxDevicePtr) {
        for (CGxShader** shader : { &s_gpu.evolve, &s_gpu.fft, &s_gpu.surface, &s_gpu.mip, &s_gpu.pack }) {
            if (*shader) {
                g_theGxDevicePtr->ShaderDestroy(*shader);
            }
        }
    }
    s_gpu = Gpu{};
}

}  // namespace VolumetricFog
