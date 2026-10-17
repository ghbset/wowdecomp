// VFOG water: coa-vfog's modern water (src/water_renderer.cpp, water_data.cpp, water_classify.h; shaders/vw_*.hlsl,
// @ 4e31ddf, GPLv3) on the Gx API. docs/VFOG_PORT.md "Water" is the spec.
//
// coa tags each class into the stencil while the client draws its liquids and shades every class after the pass. The
// Gx API has no stencil states, so here the transparent liquid pass draws the water last, grouped by class (stable,
// the stock order holds inside a class), and each class is shaded right after its own draws: a depth copy before and
// after the class's draws (water writes depth while vfog draws) marks the pixels where that class drew the front
// surface (VFogWaterMask: signed view Z), and one VFogWater pass shades them over the back buffer with alpha 1 there
// and 0 elsewhere. Magma, slime and the classes waterdata.bin lacks keep the client's look; under water the fog and
// the water are both off (coa Underwater=0).
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include "WoW/Source/WorldClient/VolumetricFogModel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/DB/WowClientDB.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/FrameTime.h"

namespace VolumetricFog {
namespace {

// ---- constants (coa water_renderer.cpp, water_classify.h, water_types.h, water_data.cpp) -----------------------
constexpr uint32_t kShadingFirstRegister = 9;
constexpr uint32_t kHorizonBandRegister = 99;
constexpr int kSceneColourStage = 0;
constexpr int kSceneDepthStage = 1;
constexpr int kWaterDepthStage = 2;
constexpr int kFirstSurfaceStage = 3;
constexpr int kFirstFoamStateStage = 7;
constexpr int kWaveFoamMaskStage = 11;
constexpr int kRippleStage = 12;
constexpr int kShoreFoamMaskStage = 14;
constexpr int kDepthFoamMaskStage = 15;
constexpr int kLastStage = 15;
constexpr int kShoreFoamSlot = 3;
constexpr int kDepthFoamSlot = 4;
constexpr int kWaveFoamMaskSlots = 3;
constexpr int kShadedMaskSlots = 5;
constexpr int kSkyBands = 5;
constexpr int kSkyColorCount = 6;
constexpr int kSkyColourOfBand[kSkyBands] = { 0, 1, 2, 3, 4 };    // top, middle, upper band, lower band, horizon
constexpr float kWaterMaxViewDepth = 65536.0f;
constexpr float kDisplayGamma = 2.2f;
constexpr float kSunVisibilityHalfWidth = 0.1f;
constexpr float kWaterF0 = 0.02f;
constexpr float kMinClarity = 0.01f;
constexpr float kShoreDistancePerDepth = 4.0f;
constexpr float kWindDirection[2] = { 0.8f, 0.6f };
constexpr int kFftResolutionLow = 128;              // coa: 128 at WaterQuality=1, 256 above
constexpr int kFftResolution = 256;
constexpr uint32_t kMinTargetSize = 8;
constexpr uint32_t kLiquidMaterialWater = 1;
constexpr int32_t kLiquidSoundBankOcean = 1;
constexpr int32_t kLiquidTypeFastWater = 9;
constexpr int32_t kLiquidTypeWmoWaterInterior = 17;
constexpr uint32_t kForeverLiquidOfClass[kWaterClassCount] = { 0, 1240, 1288, 1250, 1290 };    // lake, river, ocean, WMO interior
constexpr uint32_t kMaxMaskSide = 8192;
constexpr uint32_t kPackedMaskOpaque = 0xFF000000u;
constexpr int kPackedMaskChannelShift[kWaveFoamMaskSlots] = { 16, 8, 0 };

struct Float4 {
    float x, y, z, w;
};

struct ReflectionFogLayer {
    Float4 curve, height, emissive, diffuse, scattering;
};

// c9-c67, the order of vw_constants.hlsli (coa ShadingConstants).
struct ShadingConstants {
    Float4 light, sunColour, ambient, isotropicLight;
    Float4 sky[kSkyBands];
    Float4 stockFogColour, stockFog;
    Float4 absorption, scatteringIntensities, scatteringTop, scatteringBottom;
    Float4 depthFadeFoam, shoreFoam, waveFoam, waveFoamScaling;
    Float4 surfaceResponse, inverseTileSizes, waveControl, foamScroll, depthFoamScroll, depthDecode;
    Float4 maskTints[kShadedMaskSlots * 2];
    ReflectionFogLayer reflectionFog[kFogLayers];
    Float4 reflectionFogRange;
    Float4 ripple[3];
};
static_assert(sizeof(ShadingConstants) == 59 * 16, "c9-c67");

// ---- CVars ------------------------------------------------------------------------------------------------------
struct WaterConfig {
    bool enable = true;         // vfogWater (coa Water)
    int quality = 2;            // vfogWaterQuality 1..3
    float waves = 1.0f;         // vfogWaterWaves 0..2
    float wind = 2.0f;          // vfogWaterWind 0.5..10
    float foam = 1.0f;          // vfogWaterFoam 0..2
    float reflections = 1.0f;   // vfogWaterReflections 0..2
    float specular = 1.0f;      // vfogWaterSpecular 0..4
    float clarity = 1.0f;       // vfogWaterClarity 0.25..4
    float zoneColors = 0.5f;    // vfogWaterZoneColors 0..1
    float ripples = 0.5f;       // vfogWaterRipples 0..2 (0 = off: no simulation, no reading of the units)
    bool clientSplashes = false;    // vfogWaterClientSplashes (coa WaterClientSplashes)
    int debug = 0;              // vfogWaterDebug 0..6
};

struct WaterCVars {
    CVar* enable;
    CVar* quality;
    CVar* waves;
    CVar* wind;
    CVar* foam;
    CVar* reflections;
    CVar* specular;
    CVar* clarity;
    CVar* zoneColors;
    CVar* ripples;
    CVar* clientSplashes;
    CVar* debug;
};

WaterCVars s_cvars = {};

float CVarFloat(CVar* var, float fallback, float low, float high) {
    return std::clamp(var && std::isfinite(var->m_floatValue) ? var->m_floatValue : fallback, low, high);
}

WaterConfig ReadWaterConfig() {
    WaterConfig c;
    c.enable = !s_cvars.enable || s_cvars.enable->m_intValue != 0;
    c.quality = std::clamp(s_cvars.quality ? s_cvars.quality->m_intValue : c.quality, 1, 3);
    c.waves = CVarFloat(s_cvars.waves, c.waves, 0.0f, 2.0f);
    c.wind = CVarFloat(s_cvars.wind, c.wind, 0.5f, 10.0f);
    c.foam = CVarFloat(s_cvars.foam, c.foam, 0.0f, 2.0f);
    c.reflections = CVarFloat(s_cvars.reflections, c.reflections, 0.0f, 2.0f);
    c.specular = CVarFloat(s_cvars.specular, c.specular, 0.0f, 4.0f);
    c.clarity = CVarFloat(s_cvars.clarity, c.clarity, 0.25f, 4.0f);
    c.zoneColors = CVarFloat(s_cvars.zoneColors, c.zoneColors, 0.0f, 1.0f);
    c.ripples = CVarFloat(s_cvars.ripples, c.ripples, 0.0f, 2.0f);
    c.clientSplashes = s_cvars.clientSplashes && s_cvars.clientSplashes->m_intValue != 0;
    c.debug = std::clamp(s_cvars.debug ? s_cvars.debug->m_intValue : 0, 0, 6);
    return c;
}

// ---- waterdata.bin (coa water_data.cpp, VWD1 v1) -----------------------------------------------------------------
struct Section {
    uint32_t offset, count, recordSize;
};

struct Header {
    char magic[4];
    uint32_t version;
    uint32_t fileSize;
    Section presets, tiles, masks;
    uint32_t mipDataOffset;
    uint32_t mipDataSize;
};

struct MaskRecord {
    uint32_t foreverFileDataId;
    uint32_t size;
    uint32_t mipCount;
    float tintLow[3];
    float tintHigh[3];
    uint32_t mipDataOffset;
    uint32_t mipDataSize;
};
static_assert(sizeof(Header) == 56 && sizeof(MaskRecord) == 44, "tools/convert_forever_water.py records");

// A foam mask texture: coa FoamMaskTexture. packed = the three wave foam masks in R, G, B (PackWaveFoamMasks);
// otherwise one mask in every channel. Every level down to 1x1, Argb8888.
struct MaskTexture {
    bool packed;
    int32_t sources[kWaveFoamMaskSlots];
    bool present[kWaveFoamMaskSlots];
    uint32_t size;
    std::vector<std::vector<uint32_t>> levels;
    CGxTex* tex;
};

struct WaterData {
    bool tried;
    bool loaded;
    std::vector<WaterPreset> presets;
    std::vector<WaterFftTile> tiles;
    std::vector<MaskRecord> maskInfo;
    std::vector<MaskTexture> masks;
};

WaterData s_data;

uint32_t FullMipCount(uint32_t size) {
    uint32_t count = 1;
    for (; size > 1; size >>= 1) {
        count++;
    }
    return count;
}

bool Finite(const float* v, int n) {
    for (int i = 0; i < n; i++) {
        if (!std::isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

const char* ParseWaterData(const uint8_t* bytes, size_t size, std::vector<std::vector<const uint8_t*>>& levelPointers) {
    if (size < sizeof(Header)) {
        return "shorter than its header";
    }
    Header h;
    std::memcpy(&h, bytes, sizeof(h));
    if (std::memcmp(h.magic, "VWD1", 4) != 0 || h.version != 1) {
        return "not a VWD1 v1 water data file";
    }
    if (h.fileSize != size || h.presets.recordSize != sizeof(WaterPreset) || h.tiles.recordSize != sizeof(WaterFftTile)
        || h.masks.recordSize != sizeof(MaskRecord) || h.tiles.count > static_cast<uint32_t>(kWaterMaxTiles)) {
        return "sizes differ from this build";
    }
    auto fits = [size](uint64_t offset, uint64_t bytesLong) { return offset <= size && bytesLong <= size - offset; };
    if (!fits(h.presets.offset, uint64_t(h.presets.count) * h.presets.recordSize) || !fits(h.tiles.offset, uint64_t(h.tiles.count) * h.tiles.recordSize)
        || !fits(h.masks.offset, uint64_t(h.masks.count) * h.masks.recordSize) || !fits(h.mipDataOffset, h.mipDataSize)) {
        return "a section leaves the file";
    }
    s_data.presets.resize(h.presets.count);
    s_data.tiles.resize(h.tiles.count);
    s_data.maskInfo.resize(h.masks.count);
    std::memcpy(s_data.presets.data(), bytes + h.presets.offset, s_data.presets.size() * sizeof(WaterPreset));
    std::memcpy(s_data.tiles.data(), bytes + h.tiles.offset, s_data.tiles.size() * sizeof(WaterFftTile));
    std::memcpy(s_data.maskInfo.data(), bytes + h.masks.offset, s_data.maskInfo.size() * sizeof(MaskRecord));
    const uint8_t* mipData = bytes + h.mipDataOffset;
    levelPointers.assign(s_data.maskInfo.size(), {});
    for (size_t m = 0; m < s_data.maskInfo.size(); m++) {
        const MaskRecord& r = s_data.maskInfo[m];
        if (!r.size || r.size > kMaxMaskSide || !r.mipCount || r.mipCount > FullMipCount(r.size) || !Finite(r.tintLow, 3) || !Finite(r.tintHigh, 3)) {
            return "a mask's geometry is invalid";
        }
        uint64_t chain = 0;
        for (uint32_t level = 0; level < r.mipCount; level++) {
            const uint64_t side = std::max(r.size >> level, 1u);
            levelPointers[m].push_back(mipData + r.mipDataOffset + chain);
            chain += side * side;
        }
        if (chain != r.mipDataSize || !(r.mipDataOffset <= h.mipDataSize && chain <= h.mipDataSize - r.mipDataOffset)) {
            return "a mask's mip chain does not match its size";
        }
    }
    for (const WaterPreset& p : s_data.presets) {
        if (!Finite(&p.absorption[0], 40)) {
            return "a preset value is not finite";
        }
        for (int32_t t : p.tiles) {
            if (t < -1 || t >= static_cast<int32_t>(s_data.tiles.size())) {
                return "a preset tile index is out of range";
            }
        }
        for (int32_t m : p.masks) {
            if (m < -1 || m >= static_cast<int32_t>(s_data.maskInfo.size())) {
                return "a preset mask index is out of range";
            }
        }
    }
    for (const WaterFftTile& t : s_data.tiles) {
        if (!Finite(&t.size, 10) || !(t.size > 0.0f)) {
            return "a tile is invalid";
        }
    }
    return nullptr;
}

// Builds the mask textures' texels (coa PlanFoamMasks + PackWaveFoamMasks). A mask needs its full mip chain here
// (the Gx API always samples one); the shipped data has them.
void PlanMasks(const std::vector<std::vector<const uint8_t*>>& levels) {
    auto plan = [&](const int32_t (&sources)[kWaveFoamMaskSlots], bool packed) {
        for (const MaskTexture& m : s_data.masks) {
            if (m.packed == packed && std::equal(sources, sources + kWaveFoamMaskSlots, m.sources)) {
                return;
            }
        }
        MaskTexture m = {};
        m.packed = packed;
        std::copy(sources, sources + kWaveFoamMaskSlots, m.sources);
        const MaskRecord* layout = nullptr;
        for (int slot = 0; slot < (packed ? kWaveFoamMaskSlots : 1); slot++) {
            const int32_t i = sources[slot];
            if (i < 0) {
                continue;
            }
            const MaskRecord& r = s_data.maskInfo[i];
            if (r.mipCount != FullMipCount(r.size) || (layout && r.size != layout->size)) {
                continue;
            }
            layout = layout ? layout : &r;
            m.present[slot] = true;
        }
        if (!layout) {
            return;
        }
        m.size = layout->size;
        for (uint32_t level = 0; level < layout->mipCount; level++) {
            const size_t side = std::max(m.size >> level, 1u);
            std::vector<uint32_t> texels(side * side, kPackedMaskOpaque);
            for (int slot = 0; slot < kWaveFoamMaskSlots; slot++) {
                if (!m.present[slot]) {
                    continue;
                }
                const uint8_t* src = levels[sources[slot]][level];
                for (size_t t = 0; t < texels.size(); t++) {
                    texels[t] |= packed ? static_cast<uint32_t>(src[t]) << kPackedMaskChannelShift[slot] : src[t] * 0x00010101u;
                }
            }
            m.levels.push_back(std::move(texels));
        }
        s_data.masks.push_back(std::move(m));
    };
    for (const WaterPreset& p : s_data.presets) {
        const int32_t wave[kWaveFoamMaskSlots] = { p.masks[0], p.masks[1], p.masks[2] };
        if (wave[0] >= 0 || wave[1] >= 0 || wave[2] >= 0) {
            plan(wave, true);
        }
        for (int slot : { kShoreFoamSlot, kDepthFoamSlot }) {
            const int32_t single[kWaveFoamMaskSlots] = { p.masks[slot], -1, -1 };
            if (single[0] >= 0) {
                plan(single, false);
            }
        }
    }
}

bool EnsureData() {
    if (s_data.tried) {
        return s_data.loaded;
    }
    s_data.tried = true;
    SFileImpl* file = nullptr;
    SFileOpenFile("VFog\\waterdata.bin", &file);
    if (!file) {
        ConsolePrintf("vfog: no VFog\\waterdata.bin; the client's own water");
        return false;
    }
    std::vector<uint8_t> bytes(SFileGetFileSize(file, nullptr));
    const bool read = !bytes.empty() && SFileReadFile(file, bytes.data(), static_cast<uint32_t>(bytes.size()), nullptr, nullptr, 0);
    SFileCloseFile(file);
    std::vector<std::vector<const uint8_t*>> levels;
    const char* problem = read ? ParseWaterData(bytes.data(), bytes.size(), levels) : "could not be read";
    if (problem) {
        ConsolePrintf("vfog: VFog\\waterdata.bin rejected (%s); the client's own water", problem);
        s_data.presets.clear();
        s_data.tiles.clear();
        return false;
    }
    PlanMasks(levels);
    s_data.loaded = true;
    return true;
}

const WaterPreset* Preset(WaterClass c) {
    const uint32_t id = kForeverLiquidOfClass[static_cast<int>(c)];
    for (const WaterPreset& p : s_data.presets) {
        if (id && p.foreverLiquidId == id) {
            return &p;
        }
    }
    return nullptr;
}

const MaskTexture* WaveMasks(const WaterPreset& p) {
    for (const MaskTexture& m : s_data.masks) {
        if (m.packed && std::equal(m.sources, m.sources + kWaveFoamMaskSlots, p.masks)) {
            return &m;
        }
    }
    return nullptr;
}

const MaskTexture* SingleMask(int32_t index) {
    for (const MaskTexture& m : s_data.masks) {
        if (!m.packed && index >= 0 && m.sources[0] == index) {
            return &m;
        }
    }
    return nullptr;
}

bool MaskPresent(const WaterPreset& p, int slot) {
    if (slot < kWaveFoamMaskSlots) {
        const MaskTexture* wave = WaveMasks(p);
        return wave && wave->present[slot];
    }
    return SingleMask(p.masks[slot]) != nullptr;
}

// ---- classification (coa water_classify.h; engine.cpp ClassifyWaterSettings) --------------------------------------
bool ContainsIgnoringCase(const char* text, const char* part) {
    for (; text && *text; ++text) {
        const char* a = text;
        const char* b = part;
        for (; *a && *b && std::tolower(static_cast<unsigned char>(*a)) == *b; ++a, ++b) {
        }
        if (!*b) {
            return true;
        }
    }
    return false;
}

WaterClass ClassifyLiquid(int32_t liquidType, const LiquidTypeRec* rec, const char* texture0) {
    if (!rec || static_cast<uint32_t>(rec->m_materialID) != kLiquidMaterialWater
        || !(ContainsIgnoringCase(texture0, "\\river\\") || ContainsIgnoringCase(texture0, "\\ocean\\"))) {
        return WaterClass::None;
    }
    if (liquidType == kLiquidTypeWmoWaterInterior) {
        return WaterClass::Interior;
    }
    if (rec->m_soundBank == kLiquidSoundBankOcean) {
        return WaterClass::Ocean;
    }
    return liquidType == kLiquidTypeFastWater ? WaterClass::River : WaterClass::Lake;
}

struct ClassifiedSettings {
    const Liquid::CMaterialSettings* settings;
    WaterClass waterClass;
};
ClassifiedSettings s_classified[64];
uint32_t s_classifiedCount = 0;
uint32_t s_classifiedBank = 0;

WaterClass Classify(const Liquid::CMaterialSettings* settings) {
    const uint32_t bank = Liquid::CSettingsBank::s_settings.Count();
    if (bank != s_classifiedBank) {
        s_classifiedCount = 0;
        s_classifiedBank = bank;
    }
    for (uint32_t i = 0; i < s_classifiedCount; i++) {
        if (s_classified[i].settings == settings) {
            return s_classified[i].waterClass;
        }
    }
    WaterClass c = WaterClass::None;
    for (uint32_t id = 0; id < bank; id++) {
        if (Liquid::CSettingsBank::s_settings.m_data[id] == settings) {
            const LiquidTypeRec* rec = Liquid::s_liquidTypeDB ? Liquid::s_liquidTypeDB->GetRecord(static_cast<int32_t>(id)) : nullptr;
            c = ClassifyLiquid(static_cast<int32_t>(id), rec, settings->m_textureNames[0]);
            break;
        }
    }
    if (c != WaterClass::None && !Preset(c)) {
        c = WaterClass::None;   // waterdata.bin has no preset for it: the client's look
    }
    if (s_classifiedCount < 64) {
        s_classified[s_classifiedCount++] = { settings, c };
    }
    return c;
}

WaterClass ClassOf(const Liquid::CInstance* instance) {
    return instance && instance->m_settings ? Classify(instance->m_settings) : WaterClass::None;
}

// ---- GPU resources ------------------------------------------------------------------------------------------------
struct Shaders {
    CGxShader* depth;
    CGxShader* mask;
    CGxShader* shade[3];
};

struct Targets {
    uint32_t width, height;
    CGxTex* sceneColour;
    CGxTex* sceneDepth;
    CGxTex* mask[2];
};

struct WaveTextures {
    uint32_t resolution;
    CGxTex* slopes;
    CGxTex* state;
};

Shaders s_shaders;
Targets s_targets;
WaveTextures s_waveTex[kWaterMaxTiles];
CGxTex* s_flat;
bool s_callbackAdded;
bool s_shaderMissing;

void DestroyTex(CGxTex*& tex) {
    if (tex) {
        GxTexDestroy(tex);
        tex = nullptr;
    }
}

void DestroyTextures() {
    DestroyTex(s_targets.sceneColour);
    DestroyTex(s_targets.sceneDepth);
    DestroyTex(s_targets.mask[0]);
    DestroyTex(s_targets.mask[1]);
    s_targets.width = s_targets.height = 0;
    for (WaveTextures& w : s_waveTex) {
        DestroyTex(w.slopes);
        DestroyTex(w.state);
        w.resolution = 0;
    }
    for (MaskTexture& m : s_data.masks) {
        DestroyTex(m.tex);
    }
    DestroyTex(s_flat);
    RippleRelease();
}

void __fastcall OnDeviceRestored() {
    DestroyTextures();
}

CGxShader* Shader(CGxShader*& slot, const char* name) {
    if (!slot) {
        g_theGxDevicePtr->ShaderCreate(&slot, GxSh_Pixel, "Shaders\\Pixel", name, 1);
    }
    return slot && slot->Valid() ? slot : nullptr;
}

CGxTex* CreateTex(uint32_t w, uint32_t h, EGxTexFormat format, EGxTexFilter filter, bool wrap, bool target, void* arg, GxTexUserFunc func, const char* name) {
    CGxTex* tex = nullptr;
    CGxTexFlags flags(filter, wrap, wrap, 0, 0, target, 1, 0, 0, 0);
    g_theGxDevicePtr->TexCreate(GxTex_2d, w, h, 0, format, format, flags, arg, func ? func : GxTexNullUserFunc, name, tex);
    return tex;
}

void MaskCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& stride, const void*& texels) {
    const MaskTexture* m = static_cast<const MaskTexture*>(userArg);
    if (command == GxTex_Latch && mipLevel < m->levels.size()) {
        texels = m->levels[mipLevel].data();
        stride = width * 4;
    }
}

// Zero texels for the flat texture and for a wave texture re-uploaded while its tile has no maps (max 256^2).
const std::vector<uint32_t> s_zeroTexels(256 * 256, 0u);

void FlatCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& stride, const void*& texels) {
    if (command == GxTex_Latch) {
        texels = s_zeroTexels.data();
        stride = width * 4;
    }
}

// userArg = tile * 2 + (0 slopes, 1 state). A tile that lost its maps (settings changed) uploads zeros.
void WaveCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& stride, const void*& texels) {
    if (command != GxTex_Latch) {
        return;
    }
    const uintptr_t key = reinterpret_cast<uintptr_t>(userArg);
    const WaveMaps* maps = WaveMapsOf(static_cast<int>(key >> 1));
    stride = width * 4;
    if (!maps || mipLevel >= maps->levels || (maps->resolution >> mipLevel) != width) {
        texels = s_zeroTexels.data();
        return;
    }
    texels = ((key & 1) ? maps->state.data() : maps->slopes.data()) + maps->offset[mipLevel];
}

CGxTex* FlatTex() {
    if (!s_flat) {
        s_flat = CreateTex(kMinTargetSize, kMinTargetSize, GxTex_Argb8888, GxTex_Nearest, true, false, nullptr, FlatCallback, "VFogWaterFlat");
    }
    return s_flat;
}

CGxTex* MaskTex(const MaskTexture* m) {
    if (!m) {
        return FlatTex();
    }
    MaskTexture* mutableMask = const_cast<MaskTexture*>(m);
    if (!mutableMask->tex) {
        mutableMask->tex = CreateTex(m->size, m->size, GxTex_Argb8888, GxTex_LinearMipLinear, true, false, mutableMask, MaskCallback, "VFogWaterFoamMask");
    }
    return m->tex ? m->tex : FlatTex();
}

// After WaveCollect: (re)create each simulated tile's textures and upload its new maps.
void UploadWaves() {
    for (int tile = 0; tile < kWaterMaxTiles; tile++) {
        const WaveMaps* maps = WaveMapsOf(tile);
        WaveTextures& w = s_waveTex[tile];
        if (!maps) {
            continue;
        }
        if (w.resolution != maps->resolution) {
            DestroyTex(w.slopes);
            DestroyTex(w.state);
            void* slopesKey = reinterpret_cast<void*>(static_cast<uintptr_t>(tile * 2));
            void* stateKey = reinterpret_cast<void*>(static_cast<uintptr_t>(tile * 2 + 1));
            w.slopes = CreateTex(maps->resolution, maps->resolution, GxTex_Gr1616F, GxTex_LinearMipLinear, true, false, slopesKey, WaveCallback, "VFogWaterSlopes");
            w.state = CreateTex(maps->resolution, maps->resolution, GxTex_Argb8888, GxTex_LinearMipLinear, true, false, stateKey, WaveCallback, "VFogWaterWaveState");
            w.resolution = w.slopes && w.state ? maps->resolution : 0;
        }
        if (w.resolution) {
            const int32_t side = static_cast<int32_t>(w.resolution);
            GxTexUpdate(w.slopes, 0, 0, side, side, 1);
            GxTexUpdate(w.state, 0, 0, side, side, 1);
        }
    }
}

bool EnsureTargets(uint32_t width, uint32_t height) {
    if (s_targets.sceneColour && s_targets.width == width && s_targets.height == height) {
        return true;
    }
    DestroyTex(s_targets.sceneColour);
    DestroyTex(s_targets.sceneDepth);
    DestroyTex(s_targets.mask[0]);
    DestroyTex(s_targets.mask[1]);
    // Point sampled and clamped (coa: point, mirror; the Gx API has no mirror addressing).
    s_targets.sceneColour = CreateTex(width, height, GxTex_Argb8888, GxTex_Nearest, false, true, nullptr, nullptr, "VFogWaterScene");
    s_targets.sceneDepth = CreateTex(width, height, GxTex_R32F, GxTex_Nearest, false, true, nullptr, nullptr, "VFogWaterSceneDepth");
    s_targets.mask[0] = CreateTex(width, height, GxTex_R32F, GxTex_Nearest, false, true, nullptr, nullptr, "VFogWaterMask0");
    s_targets.mask[1] = CreateTex(width, height, GxTex_R32F, GxTex_Nearest, false, true, nullptr, nullptr, "VFogWaterMask1");
    if (!s_targets.sceneColour || !s_targets.sceneDepth || !s_targets.mask[0] || !s_targets.mask[1]) {
        DestroyTex(s_targets.sceneColour);
        DestroyTex(s_targets.sceneDepth);
        DestroyTex(s_targets.mask[0]);
        DestroyTex(s_targets.mask[1]);
        s_targets.width = s_targets.height = 0;
        return false;
    }
    s_targets.width = width;
    s_targets.height = height;
    return true;
}

// ---- per-frame pass state -----------------------------------------------------------------------------------------
struct Pass {
    bool armed;
    bool begun;
    bool failed;
    WaterClass current;
    const FrameInputs* in;
    const Config* cfg;
    WaterConfig water;
    WorldViewport vp;
    float common[9][4];
    CGxTex* previous;       // the depth copy before the current class (abs = view Z)
    int nextMask;
    uint32_t drawnTiles;
    double seconds;
    RippleShading ripples;
    ReflectionFogLayer reflectionFog[kFogLayers];
    Float4 reflectionFogRange;
};

Pass s_pass;

float Saturate(float x) {
    return std::clamp(x, 0.0f, 1.0f);
}

float SmoothStep(float edge0, float edge1, float x) {
    const float t = Saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

float FresnelSchlick(float cosine) {
    return kWaterF0 + (1.0f - kWaterF0) * std::pow(1.0f - Saturate(cosine), 5.0f);
}

void LinearColour(uint32_t argb, float* rgb) {
    UnpackColor(argb, rgb);
    for (int c = 0; c < 3; c++) {
        rgb[c] = std::pow(rgb[c], kDisplayGamma);
    }
}

uint32_t Argb(const CImVector& c) {
    return (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.b;
}

float ScrollOffset(double seconds, int axis, float multiplier) {
    const double travelled = seconds * kWindDirection[axis] * multiplier;
    return static_cast<float>(travelled - std::floor(travelled));
}

// coa BuildReflectionFog: the fog model's layers (mean noise) for the reflected rays.
void BuildReflectionFog() {
    const FogParams fog = WithMeanNoise(BuildFogParams(*s_pass.in, *s_pass.cfg));
    const float exposure = fog.authored ? s_pass.cfg->classicExposure : s_pass.cfg->exposure;
    auto linearScattered = [&fog, exposure](const float* rgb) {
        float out[3];
        for (int c = 0; c < 3; c++) {
            out[c] = (fog.linear ? rgb[c] : std::pow(std::max(rgb[c], 0.0f), kDisplayGamma)) * exposure;
        }
        return Float4{ out[0], out[1], out[2], 0.0f };
    };
    for (int i = 0; i < kFogLayers; i++) {
        const FogLayer& layer = fog.layers[i];
        const float shadow = layer.shadowed * (1.0f - fog.lightAboveHorizon);
        float emissive[3];
        float diffuse[3];
        for (int c = 0; c < 3; c++) {
            emissive[c] = layer.emissive[c] + (layer.shadowEmissive[c] - layer.emissive[c]) * shadow;
            diffuse[c] = layer.diffuse[c] * (1.0f - shadow);
        }
        const float density = layer.density * (1.0f + (layer.shadowDensity - 1.0f) * shadow);
        ReflectionFogLayer& out = s_pass.reflectionFog[i];
        out.curve = { layer.start, density, layer.strength, layer.exponent };
        out.height = { layer.upperHeight, layer.upperFalloff, layer.lowerHeight, layer.lowerFalloff };
        out.emissive = linearScattered(emissive);
        out.diffuse = linearScattered(diffuse);
        out.scattering = { layer.g, layer.isotropic, layer.endDistance, layer.skyFalloff };
    }
    s_pass.reflectionFogRange = { fog.maxDistance, fog.maxDistance, 0.0f, 1.0f };
}

// coa FillClassConstants. The stock fog never applies (water draws only while vfog pushes it out).
void FillClassConstants(ShadingConstants& c, const WaterPreset& preset, WaterClass waterClass) {
    c = {};
    const FrameInputs& in = *s_pass.in;
    const WaterConfig& cfg = s_pass.water;
    const bool noSun = waterClass == WaterClass::Interior;
    float toLight[3] = { in.toLight[0], in.toLight[1], in.toLight[2] };
    const float length = std::sqrt(toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2]);
    for (float& axis : toLight) {
        axis = length > 1e-6f ? axis / length : 0.0f;
    }
    const float sunVisibility = noSun ? 0.0f : SmoothStep(-kSunVisibilityHalfWidth, kSunVisibilityHalfWidth, toLight[2]);
    const float sunTransmission = noSun ? 1.0f : 1.0f - FresnelSchlick(Saturate(toLight[2]));
    float sun[3];
    LinearColour(in.directColor, sun);
    for (float& channel : sun) {
        channel *= in.lightIsMoon ? kMoonLightScale : 1.0f;
    }
    const DayNight::CurrentLight& bands = DayNight::g_dnInfo.m_bands;
    float sky[kSkyColorCount][3];
    float ambient[3] = {};
    for (int i = 0; i < kSkyColorCount; i++) {
        LinearColour(Argb(bands.m_skyColors[i]), sky[i]);
        for (int ch = 0; ch < 3; ch++) {
            ambient[ch] += sky[i][ch] / kSkyColorCount;
        }
    }
    if (noSun) {
        LinearColour(in.ambientColor, ambient);
    }
    c.light = { toLight[0], toLight[1], toLight[2], sunVisibility };
    c.sunColour = { sun[0], sun[1], sun[2], sunTransmission };
    c.ambient = { ambient[0], ambient[1], ambient[2], 0.0f };
    c.isotropicLight = noSun ? Float4{ 1.0f, 1.0f, 1.0f, 0.0f }
                             : Float4{ ambient[0] + sunVisibility * sun[0], ambient[1] + sunVisibility * sun[1], ambient[2] + sunVisibility * sun[2], 0.0f };
    for (int band = 0; band < kSkyBands; band++) {
        const float* colour = sky[kSkyColourOfBand[band]];
        c.sky[band] = noSun ? Float4{} : Float4{ colour[0], colour[1], colour[2], 0.0f };
    }

    const float absorptionScale = preset.absorption[3] / std::max(cfg.clarity, kMinClarity);
    c.absorption = { preset.absorption[0] * absorptionScale, preset.absorption[1] * absorptionScale, preset.absorption[2] * absorptionScale, 0.0f };
    c.scatteringIntensities = { preset.scatteringIntensities[0], preset.scatteringIntensities[1], preset.scatteringIntensities[2], preset.scatteringIntensities[3] };
    // DNInfo light bands: m_waterColors = ocean close, ocean far, river close, river far (coa 0xD38C0C..0xD38C18).
    const int zoneBase = waterClass == WaterClass::Ocean ? 0 : 2;
    float closeWater[3];
    float farWater[3];
    LinearColour(Argb(bands.m_waterColors[zoneBase]), closeWater);
    LinearColour(Argb(bands.m_waterColors[zoneBase + 1]), farWater);
    const float zoneWeight = Saturate(cfg.zoneColors);
    auto towardZone = [zoneWeight](const float* authored, const float* zone, float w) {
        return Float4{ authored[0] + (zone[0] - authored[0]) * zoneWeight, authored[1] + (zone[1] - authored[1]) * zoneWeight,
                       authored[2] + (zone[2] - authored[2]) * zoneWeight, w };
    };
    c.scatteringTop = towardZone(preset.scatteringTop, closeWater, preset.scatteringTop[3]);
    c.scatteringBottom = towardZone(preset.scatteringBottom, farWater, preset.scatteringBottom[3]);

    const float foam = std::max(cfg.foam, 0.0f);
    c.depthFadeFoam = { std::max(preset.depthFadeFoam[0], 0.0f) * foam, preset.depthFadeFoam[1], preset.depthFadeFoam[2], 0.0f };
    c.shoreFoam = { std::max(preset.shoreFoam[0], 0.0f) * foam, preset.shoreFoam[1], preset.shoreFoam[2], kShoreDistancePerDepth };
    c.waveFoam = { std::max(preset.waveFoam[0], 0.0f) * foam, 0.0f, 0.0f, 0.0f };
    c.waveFoamScaling = { preset.waveFoamScaling[0], preset.waveFoamScaling[1], preset.waveFoamScaling[2], 0.0f };
    c.surfaceResponse = { preset.roughness[0], preset.roughness[1], preset.roughness[2] * cfg.reflections, cfg.specular };

    float inverse[kWaterPresetTiles] = {};
    int tileCount = 0;
    for (int k = 0; k < kWaterPresetTiles; k++) {
        const int32_t tile = preset.tiles[k];
        if (tile < 0 || static_cast<size_t>(tile) >= s_data.tiles.size()) {
            continue;
        }
        inverse[k] = 1.0f / s_data.tiles[tile].size;
        tileCount++;
    }
    c.inverseTileSizes = { inverse[0], inverse[1], inverse[2], inverse[3] };
    c.waveControl = { 1.0f / static_cast<float>(std::max(tileCount, 1)), 0.0f, static_cast<float>(waterClass), static_cast<float>(cfg.debug) };
    const double seconds = s_pass.seconds;
    c.foamScroll = { ScrollOffset(seconds, 0, preset.waveFoam[1]), ScrollOffset(seconds, 1, preset.waveFoam[1]),
                     ScrollOffset(seconds, 0, preset.shoreFoam[3]), ScrollOffset(seconds, 1, preset.shoreFoam[3]) };
    c.depthFoamScroll = { ScrollOffset(seconds, 0, preset.depthFadeFoam[3]), ScrollOffset(seconds, 1, preset.depthFadeFoam[3]), 0.0f, 0.0f };
    if (!noSun) {
        std::memcpy(c.reflectionFog, s_pass.reflectionFog, sizeof(c.reflectionFog));
        c.reflectionFogRange = s_pass.reflectionFogRange;
    }
    c.depthDecode = { 1.0f, 0.0f, 0.0f, 0.0f };
    for (int slot = 0; slot < kShadedMaskSlots; slot++) {
        if (!MaskPresent(preset, slot)) {
            continue;
        }
        const MaskRecord& info = s_data.maskInfo[preset.masks[slot]];
        c.maskTints[slot * 2] = { Saturate(info.tintLow[0]), Saturate(info.tintLow[1]), Saturate(info.tintLow[2]), 1.0f };
        c.maskTints[slot * 2 + 1] = { Saturate(info.tintHigh[0]), Saturate(info.tintHigh[1]), Saturate(info.tintHigh[2]), 0.0f };
    }
}

bool BuildCommon(CGxTex* depth) {
    const FrameInputs& in = *s_pass.in;
    float viewToWorld[16];
    if (!Invert4x4(in.cameraRelativeView, viewToWorld)) {
        return false;
    }
    viewToWorld[12] = in.camPos[0];
    viewToWorld[13] = in.camPos[1];
    viewToWorld[14] = in.camPos[2];
    const float* proj = in.glProjection;
    const float worldMinZ = in.viewport[4];
    const float worldExtent = in.viewport[5] - in.viewport[4];
    const WorldViewport& vp = s_pass.vp;
    const float rows[9][4] = {
        { vp.x, vp.y, vp.w, vp.h },
        { 1.0f, 0.0f, 1.0f / depth->m_width, 1.0f / depth->m_height },
        { proj[0], proj[5], proj[8], proj[9] },
        { worldMinZ + worldExtent * (1.0f + proj[10]) * 0.5f, worldExtent * proj[14] * 0.5f, kWaterMaxViewDepth, in.viewport[5] },
        {}, {}, {}, {},
        { vp.w, vp.h, 1.0f / vp.w, 1.0f / vp.h },
    };
    std::memcpy(s_pass.common, rows, sizeof(rows));
    std::memcpy(s_pass.common[4], viewToWorld, sizeof(viewToWorld));
    return true;
}

// Linear view Z of the current scene depth into `target`: plain (VFogWaterDepth) or signed against s_pass.previous.
bool CopyDepth(CGxTex* target, bool mask) {
    CGxDevice* device = g_theGxDevicePtr;
    bool transient = false;
    CGxTex* depth = device->DeviceCopySceneDepth(&transient);
    if (!depth || s_pass.vp.x + s_pass.vp.w > depth->m_width || s_pass.vp.y + s_pass.vp.h > depth->m_height) {
        return false;
    }
    if (!mask && !BuildCommon(depth)) {
        return false;
    }
    static SavedPassState s_saved;
    BeginPasses(s_saved);
    SetPs(0, &s_pass.common[0][0], 9);
    SetPs(kHorizonBandRegister, s_pass.in->horizonBand, 1);
    Bind(0, depth);
    Bind(1, mask ? s_pass.previous : nullptr);
    GxRenderTargetSet(GxBuffers_Color, target, 0);
    device->RsSet(GxRs_PixelShader, mask ? s_shaders.mask : s_shaders.depth);
    device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
    DrawQuad(target->m_width, target->m_height);
    device->DeviceSceneDepthBound(true);
    GxRenderTargetSet(GxBuffers_Color, s_pass.vp.target, 0);
    EndPasses(s_saved, s_pass.vp);
    return true;
}

bool Begin() {
    s_pass.begun = true;
    s_pass.vp = LiveWorldViewport();
    const WorldViewport& vp = s_pass.vp;
    // The scene copy reads the back buffer (as the fog's LinearOverSceneCopy): no water when the world draws into a target.
    if (vp.target || vp.w < kMinTargetSize || vp.h < kMinTargetSize || !EnsureTargets(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h))) {
        return false;
    }
    const C2iVector dest = { 0, 0 };
    const C2iVector source = { static_cast<int32_t>(vp.x), static_cast<int32_t>(vp.y) };
    const C2iVector size = { static_cast<int32_t>(vp.w), static_cast<int32_t>(vp.h) };
    if (!GxTexCopy(s_targets.sceneColour, dest, source, size, 0, 0) || !CopyDepth(s_targets.sceneDepth, false)) {
        return false;
    }
    s_pass.previous = s_targets.sceneDepth;
    s_pass.nextMask = 0;
    s_pass.seconds = FrameTime::s_curTimeMs * 0.001;
    BuildReflectionFog();
    if (WaveCollect()) {
        UploadWaves();
    }
    RippleFrame(*s_pass.in, s_pass.water.ripples, s_pass.water.quality, s_pass.seconds, vp, s_pass.ripples);
    return true;
}

void ShadeClass(WaterClass waterClass) {
    const WaterPreset* preset = Preset(waterClass);
    CGxTex* waterDepth = s_targets.mask[s_pass.nextMask];
    if (!preset || !CopyDepth(waterDepth, true)) {
        return;
    }
    s_pass.previous = waterDepth;
    s_pass.nextMask ^= 1;
    for (int32_t tile : preset->tiles) {
        if (tile >= 0 && tile < kWaterMaxTiles) {
            s_pass.drawnTiles |= 1u << tile;
        }
    }

    CGxDevice* device = g_theGxDevicePtr;
    static ShadingConstants s_constants;
    FillClassConstants(s_constants, *preset, waterClass);
    std::memcpy(&s_constants.ripple[0], s_pass.ripples.window, sizeof(s_constants.ripple));
    static SavedPassState s_saved;
    BeginPasses(s_saved);
    SetPs(0, &s_pass.common[0][0], 9);
    SetPs(kShadingFirstRegister, &s_constants.light.x, sizeof(ShadingConstants) / 16);
    Bind(kSceneColourStage, s_targets.sceneColour);
    Bind(kSceneDepthStage, s_targets.sceneDepth);
    Bind(kWaterDepthStage, waterDepth);
    const bool waves = s_pass.water.waves > 0.0f;
    for (int k = 0; k < kWaterPresetTiles; k++) {
        const int32_t tile = preset->tiles[k];
        const WaveTextures* w = waves && tile >= 0 && tile < kWaterMaxTiles && s_waveTex[tile].resolution && WaveMapsOf(tile) ? &s_waveTex[tile] : nullptr;
        Bind(kFirstSurfaceStage + k, w ? w->slopes : FlatTex());
        Bind(kFirstFoamStateStage + k, w ? w->state : FlatTex());
    }
    Bind(kWaveFoamMaskStage, MaskTex(WaveMasks(*preset)));
    Bind(kShoreFoamMaskStage, MaskTex(SingleMask(preset->masks[kShoreFoamSlot])));
    Bind(kDepthFoamMaskStage, MaskTex(SingleMask(preset->masks[kDepthFoamSlot])));
    Bind(kRippleStage, s_pass.ripples.map ? s_pass.ripples.map : FlatTex());
    // coa ShadingVariant: the quality's reflection march, or sky reflections only when reflections are off.
    const int variant = s_pass.water.reflections > 0.0f ? s_pass.water.quality - 1 : 0;
    GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
    GxRsSet(GxRs_ColorWrite, 7);
    device->RsSet(GxRs_PixelShader, s_shaders.shade[variant]);
    const WorldViewport& vp = s_pass.vp;
    DrawQuad(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), vp.minX, vp.maxX, vp.minY, vp.maxY);
    for (int stage = 5; stage <= kLastStage; stage++) {
        Bind(stage, nullptr);
    }
    EndPasses(s_saved, vp);
}

}  // namespace

void RegisterWaterCVars() {
    // coa CoAVolFog.ini Water* defaults.
    s_cvars.enable = CVar::Register("vfogWater", "Modern water while the volumetric fog draws (lakes, rivers, sea, indoor pools; magma and slime keep the client's look)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.quality = CVar::Register("vfogWaterQuality", "Modern water quality: 1 = 128 wave grid, sky reflections only; 2 = 256 grid, 16-step screen reflections; 3 = 32 steps", 1, "2", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.waves = CVar::Register("vfogWaterWaves", "Modern water wave height, 0..2 (0 = flat)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.wind = CVar::Register("vfogWaterWind", "Modern water wind speed, 0.5..10", 1, "2", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.foam = CVar::Register("vfogWaterFoam", "Modern water foam, 0..2", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.reflections = CVar::Register("vfogWaterReflections", "Modern water reflections, 0..2 (0 = sky only, no screen reflections)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.specular = CVar::Register("vfogWaterSpecular", "Modern water sun and moon highlight, 0..4", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.clarity = CVar::Register("vfogWaterClarity", "Modern water clarity, 0.25..4 (larger is clearer)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.zoneColors = CVar::Register("vfogWaterZoneColors", "How much the zone's own water colours tint the modern water, 0..1", 1, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.ripples = CVar::Register("vfogWaterRipples", "Modern water ripples and wakes where units move through or jump into water, 0..2 (0 = off)", 1, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.clientSplashes = CVar::Register("vfogWaterClientSplashes", "Keep the client's own splash discs and wake trails on top of the modern water's ripples (0 = hidden while the ripples run)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.debug = CVar::Register("vfogWaterDebug", "Modern water debug view: 0 off, 1 normal, 2 foam, 3 transmittance, 4 reflection, 5 class, 6 ripples", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

bool WaterArm(Liquid::CInstance** instances, uint32_t count) {
    s_pass.armed = false;
    const FrameInputs* in = nullptr;
    const Config* cfg = nullptr;
    CGxDevice* device = g_theGxDevicePtr;
    if (!count || !device || s_shaderMissing || !WaterFrame(in, cfg)) {
        return false;
    }
    const WaterConfig water = ReadWaterConfig();
    if (!water.enable || !EnsureData() || !device->Caps().m_rttFormat[GxTex_R32F]) {
        return false;
    }
    if (!s_callbackAdded) {
        device->AddDeviceRestoredCallback(OnDeviceRestored);
        s_callbackAdded = true;
    }
    if (!Shader(s_shaders.depth, "VFogWaterDepth") || !Shader(s_shaders.mask, "VFogWaterMask") || !Shader(s_shaders.shade[0], "VFogWater1")
        || !Shader(s_shaders.shade[1], "VFogWater2") || !Shader(s_shaders.shade[2], "VFogWater3")) {
        s_shaderMissing = true;
        return false;
    }
    bool any = false;
    for (uint32_t i = 0; i < count && !any; i++) {
        any = ClassOf(instances[i]) != WaterClass::None;
    }
    if (!any) {
        return false;
    }
    std::stable_sort(instances, instances + count, [](const Liquid::CInstance* a, const Liquid::CInstance* b) { return ClassOf(a) < ClassOf(b); });
    s_pass = {};
    s_pass.armed = true;
    s_pass.in = in;
    s_pass.cfg = cfg;
    s_pass.water = water;
    return true;
}

void WaterBeforeDraw(const Liquid::CInstance* instance) {
    if (!s_pass.armed) {
        return;
    }
    const WaterClass next = ClassOf(instance);
    if (next == s_pass.current) {
        return;
    }
    if (s_pass.current != WaterClass::None && !s_pass.failed) {
        ShadeClass(s_pass.current);
    }
    if (next != WaterClass::None && !s_pass.begun) {
        s_pass.failed = !Begin();
    }
    s_pass.current = next;
}

void WaterFlushDone() {
    if (!s_pass.armed) {
        return;
    }
    if (s_pass.current != WaterClass::None && !s_pass.failed) {
        ShadeClass(s_pass.current);
    }
    if (s_pass.begun && !s_pass.failed && s_pass.water.waves > 0.0f) {
        // coa PrepareWaves: the drawn classes' tiles, 128 at WaterQuality=1, else 256; amplitude scale = waves^2.
        const WaveSettings settings = { s_pass.water.quality == 1 ? kFftResolutionLow : kFftResolution, s_pass.water.wind, s_pass.water.waves * s_pass.water.waves };
        WaveLaunch(s_data.tiles, s_pass.drawnTiles, settings, s_pass.seconds);
    }
    s_pass.armed = false;
}

bool WaterHidesClientRipples() {
    const WaterConfig water = ReadWaterConfig();
    return RipplesHideClientSprites(water.enable && water.ripples > 0.0f && !water.clientSplashes && !s_shaderMissing);
}

void WaterShutdown() {
    WaveReset();
    RippleShutdown();
    if (g_theGxDevicePtr) {
        DestroyTextures();
        CGxShader** all = &s_shaders.depth;
        for (size_t i = 0; i < sizeof(s_shaders) / sizeof(CGxShader*); i++) {
            if (all[i]) {
                g_theGxDevicePtr->ShaderDestroy(all[i]);
            }
        }
        if (s_callbackAdded) {
            g_theGxDevicePtr->RemoveDeviceRestoredCallback(OnDeviceRestored);
        }
    }
    s_shaders = Shaders{};
    s_callbackAdded = false;
    s_shaderMissing = false;
}

}  // namespace VolumetricFog
