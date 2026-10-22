// VFOG WP6: GPU passes, a port of coa-vfog src/renderer.cpp (RenderPasses :1265-1525) onto the Gx API.
// docs/VFOG_PORT.md sections 2-5 are the spec: pass list, targets, register and sampler contract.
// God rays: coa DrawGodRayMask / DrawGodRaysOverScene (renderer.cpp:1189-1260), always drawn after the world.
// Not ported (doc section 1): the depth probe, logging.
#include "WoW/Source/WorldClient/CataWater.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/VolumetricFogModel.h"
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include "WoW/Source/WorldClient/TerrainSunShadow.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/Tempest/CRect.h"

namespace VolumetricFog {

namespace {

constexpr uint32_t kLayerRegister = 12;
constexpr uint32_t kLayerNoiseRegister = 36;
constexpr uint32_t kLocalControlRegister = 53;
constexpr uint32_t kVariationRegister = 78;
constexpr uint32_t kCompositeRegister = 96;
constexpr uint32_t kMoonMarkerRegister = 95;       // vf_composite cMoonMarker (c81-c95 free in the composite, D3D and Vulkan)
constexpr uint32_t kHorizonBandRegister = 99;
constexpr uint32_t kSampleSideRegister = 80;        // VFogCompositeSplit c80 (coa c99, which is the horizon band here)
constexpr uint32_t kDitherRegister = 79;            // vf_common cDither (x on, y frame)
constexpr uint32_t kGradeRegister = 10;             // VFogGrade c10 strength, c11-c18 curve
constexpr uint32_t kPostRegister = 10;              // LegionPost c10 FXAA, c11-c13 colour matrix
constexpr uint32_t kRefractRegister = 10;           // LegionRefraction c10 rect, c11 haze, c12 size
constexpr uint32_t kLocalLightRegister = 100;
constexpr uint32_t kShadowRegister = 56;           // vf_shadow.hlsli c56-c61 band rows, c62 control, c63 terrain
constexpr uint32_t kShadowMaskRegister = 10;      // VFogShadowMask c10-c13 (light, ambient + bias, diffuse, stock fog)
constexpr float kShadowMaskSurfaceBias = 4.0f;     // yd: the ADT surface between WDL vertices may sit this far under them
constexpr uint32_t kShadowMaskBandRegister = 65;   // VFogShadowMask c65-c67 band 0, c68-c70 unit map, c71 (band 0 on, unit on)
constexpr float kReceiverShadowStrength = 0.3f;    // stock receivers: lit colour x (0.7 + 0.3 x cascade lit)
constexpr uint32_t kSsaoRegister = 10;             // LegionSsao c10 (radius, fade start, 1 / fade length, strength), c11 stock fog
constexpr int kShadowStage = 5;                     // vf_shadow.hlsli s5 band 1, s6 band 2, s7 terrain
constexpr int kMarchDepthStage = 8;                 // vf_composite.hlsli sMarchDepth: the depth the low-res fog was marched at
constexpr uint32_t kSavedPixelRegisters = kLocalLightRegister + 4 * kMaxLocalLights;    // c0-c131
static_assert(kSavedPixelRegisters == kPassSavedPixelRegisters, "SavedPassState holds the whole register range");
constexpr float kDensityNoiseSize = 32.0f;          // coa noise_volume.h kDensityNoiseSize
constexpr double kHistoryMaxSeconds = 0.25;         // coa renderer.cpp:84-86
constexpr float kHistoryMaxMove = 30.0f;
constexpr float kHistoryMinForwardDot = 0.70710678f;
constexpr double kMaxNoiseStepSeconds = 0.25;       // coa renderer.cpp:67
constexpr float kNearQuarterTurn = 1.57f;           // wxl-vfog ec1c66b
constexpr uint32_t kMinTargetSize = 8;              // GxTexCreate's minimum
constexpr uint32_t kRayScale = 4;                   // coa renderer.cpp:68-83, 244
constexpr float kRayFalloff = 8.0f;
constexpr float kRayThreshold = 0.12f;
constexpr float kRayStep = 0.075f;
constexpr float kRayDecay = 0.9f;
constexpr int kRayTaps = 8;
constexpr float kMinGodRayStrength = 0.005f;

enum Blend { Blend_GammaFixedFunction = 0, Blend_LinearOverSceneCopy = 1, Blend_LinearFixedFunction = 2, Blend_GammaOverSceneCopy = 3 };     // coa FogBlend

struct Shaders {
    CGxShader* march[3][2][2];          // [quality-1][local lights][noise]
    CGxShader* composite[3][2][2];
    CGxShader* compositeSplit[3][2][2]; // VFogCompositeSplit: one side of an MSAA silhouette (vfogMsaaSplit)
    CGxShader* silhouette;              // VFogSilhouette: oDepth (Vulkan: gl_FragDepth)
    CGxShader* temporal;
    CGxShader* historyDepth;
    CGxShader* rayMask;
    CGxShader* rayBlur;
    CGxShader* rayComposite;
    CGxShader* grade;
    CGxShader* depthCopy;               // VFogDepthCopy: the scene depth into an R32F target (fading cross-fade)
    CGxShader* shadowMask;              // VFogShadowMask: the land's sun shadow over the opaque world (shadowMask)
    CGxShader* post[3];                 // LegionPost{0,1,2}: colour shift, + FXAA low, + FXAA high (ffxAntiAliasingMode)
    CGxShader* ssao[4];                 // LegionSsao, LegionSsaoBox, LegionSsaoBilateral, LegionSsaoApply (SSAO)
    CGxShader* refraction;              // LegionRefraction: heat haze over one fire emitter (refraction)
};

struct Targets {
    uint32_t width;
    uint32_t height;
    CGxTex* march;
    CGxTex* history[2];
    CGxTex* historyDepth[2];
    CGxTex* sceneCopy;          // full world viewport, coa LinearOverSceneCopy (s3)
    uint32_t sceneCopyWidth;
    uint32_t sceneCopyHeight;
    CGxTex* rays[2];            // world viewport / kRayScale, bilinear: [1] mask -> [0] blur -> [1] blur
    uint32_t rayWidth;
    uint32_t rayHeight;
    EGxTexFormat colourFormat;  // march, history and rays: Abgr16161616F (vfogHdrTargets) or Argb8888
    CGxTex* ao[2];              // SSAO: world viewport / 1 or 2, bilinear: [0] AO -> [1] blurred
};

// coa GodRayFrame: drawn by RenderGodRays at the end of the world.
struct GodRays {
    bool pending;
    float strength;
    float colour[3];
    float glow;
};

struct History {
    bool valid;
    int index;
    float worldToView[16];
    float proj[16];
    float camPos[3];
    float viewport[6];
    uint32_t scale;
    Config config;
    int mapId;
    int lightSlot;
    int localLightCount;
    double seconds;
};

Shaders s_shaders;
Targets s_targets;
CGxTex* s_densityNoise;
CGxTex* s_authoredNoise;
History s_history;
double s_noiseSeconds = -1.0;
bool s_callbackAdded;
bool s_depthUnsupported;        // DeviceCopySceneDepth gave nullptr; cleared on device restore
bool s_shaderMissing;           // a needed VFog shader is not in the MPQ
bool s_overSceneCopy;           // the last composite blended in linear light over a scene copy
GodRays s_godRays;

// vfogFadeStencil (wxl-vfog 0.6.3 KeepDepthBehind / DrawOverMarked): R32F copies of the depth behind the fading models
// (taken before they draw) and with them (taken in RenderFog). armed = behind is this frame's.
struct FadeDepth {
    CGxTex* behind;
    CGxTex* with;
    bool armed;
};
FadeDepth s_fade;


CGxShader* VariantShader(CGxShader* (&table)[3][2][2], const char* base, int quality, bool lit, bool noisy) {
    char name[32];
    snprintf(name, sizeof(name), "%s%d%s%s", base, quality, lit ? "L" : "", noisy ? "N" : "");
    return Shader(table[quality - 1][lit][noisy], name);
}

}  // namespace

CGxShader* Shader(CGxShader*& slot, const char* name) {
    if (!slot) {
        g_theGxDevicePtr->ShaderCreate(&slot, GxSh_Pixel, "Shaders\\Pixel", name, 1);
    }

    return slot && slot->Valid() ? slot : nullptr;
}

void DropInvalidShaders(CGxShader** slots, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (slots[i] && !slots[i]->Valid()) {
            g_theGxDevicePtr->ShaderDestroy(slots[i]);
            slots[i] = nullptr;
        }
    }
}

void DestroyTex(CGxTex*& tex) {
    if (tex) {
        GxTexDestroy(tex);
        tex = nullptr;
    }
}

namespace {

void DestroyTargets() {
    DestroyTex(s_targets.march);
    for (int i = 0; i < 2; i++) {
        DestroyTex(s_targets.history[i]);
        DestroyTex(s_targets.historyDepth[i]);
    }
    DestroyTex(s_targets.sceneCopy);
    s_targets.sceneCopyWidth = 0;
    s_targets.sceneCopyHeight = 0;
    DestroyTex(s_targets.rays[0]);
    DestroyTex(s_targets.rays[1]);
    s_targets.rayWidth = 0;
    s_targets.rayHeight = 0;
    DestroyTex(s_targets.ao[0]);
    DestroyTex(s_targets.ao[1]);
    s_godRays.pending = false;

    s_targets.width = 0;
    s_targets.height = 0;
    s_history.valid = false;
}

// Not in DestroyTargets: RenderFog rebuilds the fog targets after KeepDepthBehindFading armed s_fade.behind.
void DestroyFadeDepth() {
    DestroyTex(s_fade.behind);
    DestroyTex(s_fade.with);
    s_fade.armed = false;
}

void __fastcall OnDeviceRestored() {
    DestroyTargets();
    DestroyFadeDepth();
    DestroyTex(s_densityNoise);
    DestroyTex(s_authoredNoise);
    TerrainSunShadow::Release();
    s_depthUnsupported = false;     // MSAA may have changed with the reset
}

CGxTex* CreateTarget(uint32_t width, uint32_t height, EGxTexFilter filter = GxTex_Nearest, EGxTexFormat format = GxTex_Argb8888) {
    CGxTex* tex = nullptr;
    CGxTexFlags flags(filter, 0, 0, 0, 0, 1, 1, 0, 0, 0);
    g_theGxDevicePtr->TexCreate(GxTex_2d, width, height, 0, format, format, flags, nullptr, GxTexNullUserFunc, "VFogTarget", tex);
    return tex;
}

// Fog colour storage: RGBA half float where the device renders and filters it (vfogHdrTargets 1), else 8 bit,
// which bands once the low-res fog is upsampled bilinearly. The history depth packs 16 bits into two 8-bit
// channels and the scene copy is a copy of the 8-bit back buffer, so both stay Argb8888.
bool s_hdrCreateFailed = false;     // a half-float create failed although the caps offered it: 8 bit from then on

EGxTexFormat FogColourFormat(const Config& cfg) {
    return cfg.hdrTargets && !s_hdrCreateFailed && g_theGxDevicePtr->Caps().m_rttAbgr16161616F ? GxTex_Abgr16161616F : GxTex_Argb8888;
}

bool EnsureTargets(uint32_t width, uint32_t height, EGxTexFormat format) {
    if (s_targets.march && s_targets.width == width && s_targets.height == height && s_targets.colourFormat == format) {
        return true;
    }

    DestroyTargets();
    s_targets.colourFormat = format;
    s_targets.march = CreateTarget(width, height, GxTex_Nearest, format);
    for (int i = 0; i < 2; i++) {
        s_targets.history[i] = CreateTarget(width, height, GxTex_Nearest, format);
        s_targets.historyDepth[i] = CreateTarget(width, height);
    }

    if (!s_targets.march || !s_targets.history[0] || !s_targets.history[1] || !s_targets.historyDepth[0] || !s_targets.historyDepth[1]) {
        DestroyTargets();
        return false;
    }

    s_targets.width = width;
    s_targets.height = height;
    return true;
}

// coa renderer.cpp:1176 god ray targets, world viewport / 4 (coa :1276), bilinear and clamped.
// Same colour format as the fog targets (EnsureTargets runs first and sets it); a format change recreates them.
bool EnsureRayTargets(uint32_t width, uint32_t height) {
    const EGxTexFormat format = s_targets.colourFormat;
    if (s_targets.rays[0] && s_targets.rays[1] && s_targets.rayWidth == width && s_targets.rayHeight == height && s_targets.rays[0]->m_format == format) {
        return true;
    }
    DestroyTex(s_targets.rays[0]);
    DestroyTex(s_targets.rays[1]);
    s_targets.rays[0] = CreateTarget(width, height, GxTex_Linear, format);
    s_targets.rays[1] = CreateTarget(width, height, GxTex_Linear, format);
    if (!s_targets.rays[0] || !s_targets.rays[1]) {
        DestroyTex(s_targets.rays[0]);
        DestroyTex(s_targets.rays[1]);
        return false;
    }
    s_targets.rayWidth = width;
    s_targets.rayHeight = height;
    return true;
}

// coa renderer.cpp:1446 LinearOverSceneCopy: the world viewport of the back buffer, before the fog. Every backend's
// TexCopy reads the back buffer (D3D9 StretchRect, D3D11 resolved back buffer, Vulkan frame::CopyFromScene).
// Bilinear: the god ray mask samples it at quarter-res texel centres (coa's linear StretchRect); every other read
// is at a texel centre, where bilinear is the texel.
CGxTex* CopySceneBeforeFog(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!s_targets.sceneCopy || s_targets.sceneCopyWidth != width || s_targets.sceneCopyHeight != height) {
        DestroyTex(s_targets.sceneCopy);
        s_targets.sceneCopy = CreateTarget(width, height, GxTex_Linear);
        if (!s_targets.sceneCopy) {
            return nullptr;
        }
        s_targets.sceneCopyWidth = width;
        s_targets.sceneCopyHeight = height;
    }
    const C2iVector dest = { 0, 0 };
    const C2iVector source = { static_cast<int32_t>(x), static_cast<int32_t>(y) };
    const C2iVector size = { static_cast<int32_t>(width), static_cast<int32_t>(height) };
    return GxTexCopy(s_targets.sceneCopy, dest, source, size, 0, 0) ? s_targets.sceneCopy : nullptr;
}

void AtlasTexCallback(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    if (command == GxTex_Latch) {
        const NoiseAtlas* atlas = static_cast<const NoiseAtlas*>(userArg);
        texels = atlas->pixels;
        texelStrideInBytes = atlas->width * 4;
    }
}

// Static noise volumes packed as 2D atlases (doc "Noise atlases"), bilinear + wrap, uploaded once.
CGxTex* AtlasTex(CGxTex*& tex, const NoiseAtlas& atlas, const char* name) {
    if (!tex && atlas.pixels && atlas.width >= kMinTargetSize && atlas.height >= kMinTargetSize) {
        CGxTexFlags flags(GxTex_Linear, 1, 1, 0, 0, 0, 1, 0, 0, 0);
        g_theGxDevicePtr->TexCreate(GxTex_2d, atlas.width, atlas.height, 0, GxTex_Argb8888, GxTex_Argb8888, flags, const_cast<NoiseAtlas*>(&atlas), AtlasTexCallback, name, tex);
    }

    return tex;
}

C44Matrix ToMatrix(const float m[16]) {
    C44Matrix out;
    std::memcpy(static_cast<void*>(&out), m, sizeof(float) * 16);
    return out;
}

// coa renderer.cpp:186 WorldViewFromCameraRelative.
bool WorldViewFromCameraRelative(const FrameInputs& in, float viewToWorld[16], float worldToView[16]) {
    C44Matrix view = ToMatrix(in.cameraRelativeView);
    float det = view.Determinant();
    if (!(std::fabs(det) > 1e-12f)) {
        return false;
    }

    C44Matrix inverse = view.Inverse(det);
    std::memcpy(viewToWorld, &inverse, sizeof(float) * 16);
    viewToWorld[12] = in.camPos[0];
    viewToWorld[13] = in.camPos[1];
    viewToWorld[14] = in.camPos[2];

    inverse = ToMatrix(viewToWorld);
    det = inverse.Determinant();
    if (!(std::fabs(det) > 1e-12f)) {
        return false;
    }

    C44Matrix world = inverse.Inverse(det);
    std::memcpy(worldToView, &world, sizeof(float) * 16);
    return true;
}

// coa config.cpp:336 SameFogSettings compares every setting that changes the fog.
bool SameSettings(const Config& a, const Config& b) {
    return a.enable == b.enable && a.quality == b.quality && a.density == b.density && a.haze == b.haze
        && a.groundFog == b.groundFog && a.farFog == b.farFog && a.transparentFog == b.transparentFog
        && a.dataMode == b.dataMode && a.colorSpace == b.colorSpace && a.classicPhase == b.classicPhase
        && a.sunScatter == b.sunScatter && a.ambient == b.ambient && a.exposure == b.exposure
        && a.classicExposure == b.classicExposure && a.localLights == b.localLights
        && a.localLightIntensity == b.localLightIntensity && a.localLightPhase == b.localLightPhase
        && a.interiorAware == b.interiorAware && a.interiorDensity == b.interiorDensity
        && a.noiseAmount == b.noiseAmount && a.noiseScale == b.noiseScale && a.noiseWindSpeed == b.noiseWindSpeed
        && a.classicNoise == b.classicNoise && a.glowCompensation == b.glowCompensation
        && a.maxDistance == b.maxDistance && a.startOffset == b.startOffset && a.temporal == b.temporal
        && a.debugView == b.debugView && a.interiorFarFog == b.interiorFarFog && a.underwater == b.underwater
        && a.stockFog == b.stockFog && a.sunMarker == b.sunMarker && a.shadowedSun == b.shadowedSun && a.canopy == b.canopy;
}

// wxl-vfog ec1c66b TangentSpan: tangents (lateral over depth) of the directions ahead that can meet a circle.
void TangentSpan(float lateral, float depth, float radius, float& low, float& high) {
    low = -std::numeric_limits<float>::infinity();
    high = std::numeric_limits<float>::infinity();
    float distance = std::hypot(lateral, depth);
    if (distance <= radius) {
        return;
    }

    float centre = std::atan2(lateral, depth);
    float half = std::asin(radius / distance);
    if (centre - half > -kNearQuarterTurn) {
        low = std::tan(centre - half);
    }

    if (centre + half < kNearQuarterTurn) {
        high = std::tan(centre + half);
    }
}

struct TexelRect {
    int32_t left, top, right, bottom;
};

// wxl-vfog ec1c66b LocalLightTexels: the march texels whose rays can meet a light sphere, a little wide.
TexelRect LocalLightTexels(const float (*lights)[4], uint32_t count, const float* proj, float vpW, float vpH, uint32_t scale, uint32_t lowW, uint32_t lowH) {
    TexelRect texels = { static_cast<int32_t>(lowW), static_cast<int32_t>(lowH), 0, 0 };
    auto widen = [scale](float first, float second, float pixels, uint32_t size, int32_t& low, int32_t& high) {
        float a = (first * pixels - scale / 2 - 0.5f) / scale;
        float b = (second * pixels - scale / 2 - 0.5f) / scale;
        float limit = static_cast<float>(size);
        low = std::min(low, static_cast<int32_t>(std::clamp(std::floor(std::min(a, b)) - 2.0f, 0.0f, limit)));
        high = std::max(high, static_cast<int32_t>(std::clamp(std::ceil(std::max(a, b)) + 3.0f, 0.0f, limit)));
    };

    for (uint32_t i = 0; i < count; i++) {
        const float* light = lights[i * 4];
        float radius = light[3] * 1.001f;
        if (light[2] + radius <= 0.0f) {
            continue;
        }

        if (light[0] * light[0] + light[1] * light[1] + light[2] * light[2] <= radius * radius) {
            return { 0, 0, static_cast<int32_t>(lowW), static_cast<int32_t>(lowH) };
        }

        float low[2];
        float high[2];
        TangentSpan(light[0], light[2], radius, low[0], high[0]);
        TangentSpan(light[1], light[2], radius, low[1], high[1]);
        widen((low[0] * proj[0] + proj[8]) * 0.5f + 0.5f, (high[0] * proj[0] + proj[8]) * 0.5f + 0.5f, vpW, lowW, texels.left, texels.right);
        widen(0.5f - (low[1] * proj[5] + proj[9]) * 0.5f, 0.5f - (high[1] * proj[5] + proj[9]) * 0.5f, vpH, lowH, texels.top, texels.bottom);
    }

    return texels;
}

}  // namespace

// Shared with VolumetricFogWater.cpp (declared in VolumetricFogWater.h).
// One past the highest pixel register written since the innermost BeginPasses: EndPasses restores only c0 up to it.
uint32_t s_psHighWater = 0;

void SetPs(uint32_t reg, const float* values, uint32_t count) {
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, reg, values, count);
    s_psHighWater = std::max(s_psHighWater, reg + count);
}

void Bind(int stage, CGxTex* tex) {
    g_theGxDevicePtr->RsSet(static_cast<EGxRenderState>(GxRs_Texture0 + stage), tex);
}

void BindNormalBuffer() {
    CGxTex* normals = g_theGxDevicePtr->DeviceNormalBuffer();
    const float on[4] = { normals ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
    SetPs(9, on, 1);
    Bind(4, normals);
}

// One quad over the Gx viewport [minX..maxX, minY..maxY] (normalised; default the whole target) of width x height
// pixels, through the fixed-function VS. TEXCOORD0 = uv across that viewport, (x + 0.5) / width with v = 0 on the
// top row, at every pixel centre on every backend: D3D9 rasterises at integer pixel coordinates, and Vulkan emulates
// that (its viewport moves half a pixel, kD3d9CentreShift), so both carry the half texel (as FFX does); caps say which.
void DrawQuad(uint32_t width, uint32_t height, float minX, float maxX, float minY, float maxY) {
    static uint16_t s_indices[4] = { 0, 3, 1, 2 };
    static CGxBatch s_batch = { GxPrim_TriangleStrip, 0, 4, 0, 3 };

    C44Matrix projection;
    GxuXformCreateOrtho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -1.0f, 1.0f, projection);
    GxXformSetProjection(projection);
    GxXformSetViewport(minX, maxX, minY, maxY, 0.0f, 1.0f);

    float offset = g_theGxDevicePtr->Caps().m_pixelCenterOnEdge ? 0.0f : 0.5f;
    float w = static_cast<float>(width);
    float h = static_cast<float>(height);
    float minU = offset / w;
    float maxU = (w + offset) / w;
    float minV = offset / h;
    float maxV = (h + offset) / h;

    C3Vector pos[4];
    C2Vector tex[4];
    pos[0].x = 0.0f; pos[0].y = 0.0f; pos[0].z = 0.0f; tex[0].x = minU; tex[0].y = maxV;
    pos[1].x = 0.0f; pos[1].y = h;    pos[1].z = 0.0f; tex[1].x = minU; tex[1].y = minV;
    pos[2].x = w;    pos[2].y = h;    pos[2].z = 0.0f; tex[2].x = maxU; tex[2].y = minV;
    pos[3].x = w;    pos[3].y = 0.0f; pos[3].z = 0.0f; tex[3].x = maxU; tex[3].y = maxV;

    CImVector white(255, 255, 255, 255);
    GxPrimVertexPtr(4, pos, sizeof(C3Vector), nullptr, 0, &white, 0, tex, sizeof(C2Vector), nullptr, 0);
    GxPrimIndexPtr(4, s_indices);
    g_theGxDevicePtr->Draw(&s_batch, 1);
}

// World viewport in pixels of the current target, D3D-style top-left origin (coa D3DVIEWPORT9), from the
// live Gx viewport: the same formula every backend uses for the back buffer (CGxDeviceD3d::IXformSetViewport).

WorldViewport LiveWorldViewport() {
    WorldViewport vp = {};
    CGxDevice* device = g_theGxDevicePtr;
    device->DeviceGetRenderTarget(GxBuffers_Color, vp.target);
    const CRect window = device->DeviceCurWindow();
    GxXformViewport(vp.minX, vp.maxX, vp.minY, vp.maxY, vp.minZ, vp.maxZ);
    float screenW = window.maxX - window.minX;
    float screenH = window.maxY - window.minY;
    vp.x = std::floor(vp.minX * screenW + 0.5f);
    vp.y = vp.target ? std::floor(vp.minY * screenH + 0.5f) : std::floor((1.0f - vp.maxY) * screenH + 0.5f);
    vp.w = std::floor(vp.maxX * screenW - vp.x + 0.5f);
    vp.h = vp.target ? std::floor((vp.maxY - vp.minY) * screenH + 0.5f) : std::floor((1.0f - vp.minY) * screenH - vp.y + 0.5f);
    return vp;
}


void BeginPasses(SavedPassState& saved) {
    CGxDevice* device = g_theGxDevicePtr;
    std::memcpy(saved.constants, CGxDevice::s_shadowConstants[0].constants, sizeof(saved.constants));     // [0] = pixel
    saved.outerHighWater = s_psHighWater;
    s_psHighWater = 0;
    GxXformProjection(saved.projection);
    GxXformView(saved.view);
    C44Matrix identity;
    GxXformSetView(identity);
    GxXformPush(GxXform_World);
    device->m_xforms[GxXform_World].Identity();

    device->RsPush();
    GxRsSet(GxRs_VertexShader, static_cast<void*>(nullptr));
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_ClipPlaneMask, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_ScissorTest, 0);
    GxRsSet(GxRs_StencilTest, 0);
}

bool StencilAvailable() {
    return g_theGxDevicePtr && g_theGxDevicePtr->m_caps.m_stencilBits > 0;
}

void SetStencil(EGxStencilCmp func, int32_t ref, int32_t readMask, int32_t writeMask, EGxStencilOp pass, EGxStencilOp zfail) {
    GxRsSet(GxRs_StencilTest, 1);
    GxRsSet(GxRs_StencilFunc, static_cast<int32_t>(func));
    GxRsSet(GxRs_StencilRef, ref);
    GxRsSet(GxRs_StencilReadMask, readMask);
    GxRsSet(GxRs_StencilWriteMask, writeMask);
    GxRsSet(GxRs_StencilFail, static_cast<int32_t>(GxStencilOp_Keep));
    GxRsSet(GxRs_StencilZFail, static_cast<int32_t>(zfail));
    GxRsSet(GxRs_StencilPass, static_cast<int32_t>(pass));
}

void EndPasses(const SavedPassState& saved, const WorldViewport& vp) {
    for (int stage = 0; stage <= 4; stage++) {
        Bind(stage, nullptr);
    }
    g_theGxDevicePtr->RsPop();
    GxXformPop(GxXform_World);
    GxXformSetView(saved.view);
    GxXformSetProjection(saved.projection);
    GxXformSetViewport(vp.minX, vp.maxX, vp.minY, vp.maxY, vp.minZ, vp.maxZ);
    const uint32_t written = std::min(s_psHighWater, kSavedPixelRegisters);
    if (written) {
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, saved.constants, written);
    }
    // An enclosing pass group must still restore what this one wrote (to its own saved values).
    s_psHighWater = std::max(saved.outerHighWater, written);
}

namespace {

// Scissored to the world viewport of the back buffer, RGB only.
void ScissorToWorld(const WorldViewport& vp) {
    CRect world;
    world.minX = vp.minX;
    world.maxX = vp.maxX;
    world.minY = vp.minY;
    world.maxY = vp.maxY;
    g_theGxDevicePtr->SetScissor(world);
    GxRsSet(GxRs_ScissorTest, 1);
    GxRsSet(GxRs_ColorWrite, 7);
}

void DrawToTarget(CGxTex* target, CGxShader* shader) {
    GxRenderTargetSet(GxBuffers_Color, target, 0);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, shader);
    DrawQuad(target->m_width, target->m_height);
}

// The whole scene depth texture, point for point, into an R32F target of its size (VFogDepthCopy).
bool CopyRawDepth(CGxTex*& target, CGxTex* depth) {
    if (target && (target->m_width != depth->m_width || target->m_height != depth->m_height)) {
        DestroyTex(target);
    }
    if (!target) {
        target = CreateTarget(depth->m_width, depth->m_height, GxTex_Nearest, GxTex_R32F);
    }
    if (!target) {
        return false;
    }
    const WorldViewport vp = LiveWorldViewport();
    static SavedPassState s_saved;
    BeginPasses(s_saved);
    Bind(0, depth);
    g_theGxDevicePtr->DeviceSceneDepthBound(false);
    DrawToTarget(target, s_shaders.depthCopy);
    g_theGxDevicePtr->DeviceSceneDepthBound(true);
    GxRenderTargetSet(GxBuffers_Color, vp.target, 0);
    EndPasses(s_saved, vp);
    return true;
}

// coa Renderer::DrawCompositeBySampleDepth + MarkSilhouetteSamples (vfogMsaaSplit, docs section 3 MSAA): the samples
// of each silhouette are marked in the stencil by VFogSilhouette's oDepth against the live multisampled depth (far =
// 0xFF, near = 1, the rest 0), then the plain composite draws (everywhere when it overwrites, i.e. over the scene copy;
// else the unmarked samples), and the split composite fogs the near and the far samples at their own side's depth.
// The pixel shader runs once per pixel; the stencil test is per sample, so each side lands on its own samples.
// Stencil bits (docs section 3, "Stencil"): the split owns the low five (near 1, far 0x1F), the fading-model alpha the
// top three (0xE0, DrawFadingEarly). fadeMarks = those are this frame's: DrawFadingEarly cleared the whole stencil and
// wrote only 0xE0, so the low bits are already 0 and the clear (which would wipe the marks) is skipped.
void DrawCompositeBySampleDepth(CGxShader* composite, CGxShader* split, bool overwrites, bool fadeMarks, const WorldViewport& vp) {
    CGxDevice* device = g_theGxDevicePtr;
    const uint32_t w = static_cast<uint32_t>(vp.w);
    const uint32_t h = static_cast<uint32_t>(vp.h);
    GxXformSetViewport(vp.minX, vp.maxX, vp.minY, vp.maxY, 0.0f, 1.0f);
    if (!fadeMarks) {
        GxSceneClear(GxBufferBit_Stencil, CImVector { 0, 0, 0, 0 });
    }
    GxRsSet(GxRs_ColorWrite, 0);
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthFunc, 3);     // LESS (s_cmpFunc[3])
    SetStencil(GxStencilCmp_Always, 1, kSplitBits, kSplitBits, GxStencilOp_Invert, GxStencilOp_Replace);
    device->RsSet(GxRs_PixelShader, s_shaders.silhouette);
    DrawQuad(w, h, vp.minX, vp.maxX, vp.minY, vp.maxY);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_ColorWrite, 7);

    const float nearSide[4] = { 0.0f, overwrites ? 1.0f : 0.0f, 0.0f, 0.0f };
    const float farSide[4] = { 1.0f, overwrites ? 1.0f : 0.0f, 0.0f, 0.0f };
    if (overwrites) {
        GxRsSet(GxRs_StencilTest, 0);
    } else {
        SetStencil(GxStencilCmp_Equal, 0, kSplitBits, 0, GxStencilOp_Keep);
    }
    device->RsSet(GxRs_PixelShader, composite);
    DrawQuad(w, h, vp.minX, vp.maxX, vp.minY, vp.maxY);
    device->RsSet(GxRs_PixelShader, split);
    SetStencil(GxStencilCmp_Equal, 1, kSplitBits, 0, GxStencilOp_Keep);
    SetPs(kSampleSideRegister, nearSide, 1);
    DrawQuad(w, h, vp.minX, vp.maxX, vp.minY, vp.maxY);
    SetStencil(GxStencilCmp_Equal, kSplitBits, kSplitBits, 0, GxStencilOp_Keep);
    SetPs(kSampleSideRegister, farSide, 1);
    DrawQuad(w, h, vp.minX, vp.maxX, vp.minY, vp.maxY);
    GxRsSet(GxRs_StencilTest, 0);
}

}  // namespace

bool KeepDepthBehindFading() {
    s_fade.armed = false;
    CGxDevice* device = g_theGxDevicePtr;
    // The weights need BLENDFACTOR. With MSAA (unlike wxl-vfog 0.6.3) too: the copy is the single-sample depth every
    // pass reads (sample 0: D3D9 RESZ, D3D11 blit_depth_ms_ps, Vulkan's SAMPLE_ZERO resolve), the marks are per sample
    // and the sample split keeps them (its own stencil bits).
    if (!device || s_depthUnsupported || !StencilAvailable() || !device->m_caps.m_blendFactor
        || !device->Caps().m_rttFormat[GxTex_R32F] || LiveWorldViewport().target || !Shader(s_shaders.depthCopy, "VFogDepthCopy")) {
        return false;
    }
    CGxTex* depth = device->DeviceCopySceneDepth();
    s_fade.armed = depth && CopyRawDepth(s_fade.behind, depth);
    return s_fade.armed;
}

bool RendererReady() {
    if (!g_theGxDevicePtr || s_depthUnsupported || s_shaderMissing) {
        return false;
    }

    if (!s_callbackAdded) {
        g_theGxDevicePtr->AddDeviceRestoredCallback(OnDeviceRestored);
        s_callbackAdded = true;
    }

    // The per-quality variants load lazily in RenderFog; these two prove the shader set is in the MPQ.
    return Shader(s_shaders.temporal, "VFogTemporal") && Shader(s_shaders.historyDepth, "VFogHistoryDepth");
}

void InvalidateHistory() {
    s_history.valid = false;
}

// gfxShadowFade: vf_shadow.hlsli cShadowFade (c64.x = 1 - fade) for SunVisibility, the fog's and the caustics' sun
// tap (shafts, in-scatter, canopy). The sun glare's occlusion is real and is not faded. Shared with the caustics.
void SetShadowFadeRegister() {
    const float unfade[4] = { 1.0f - DayNight::g_shadowFade, 0.0f, 0.0f, 0.0f };
    SetPs(kShadowRegister + 8, unfade, 1);
}

// A map's texgen rows composed with this frame's view: vfog view space -> camera-relative world -> the Gx view
// PostUpdate built them in.
static void ComposeShadowRows(const float viewToWorld[16], const ShadowView& view, const C4Vector rows[3], float out[3][4]) {
    float rotation[16];
    std::memcpy(rotation, viewToWorld, sizeof(rotation));
    rotation[12] = rotation[13] = rotation[14] = 0.0f;   // camera relative, as the Gx view stack top
    float a[16];
    C44Matrix composed = ToMatrix(rotation) * view.view;
    std::memcpy(a, &composed, sizeof(a));
    for (int k = 0; k < 3; k++) {
        const float col[4] = { rows[k].x, rows[k].y, rows[k].z, rows[k].w };
        for (int i = 0; i < 4; i++) {
            out[k][i] = a[i * 4] * col[0] + a[i * 4 + 1] * col[1] + a[i * 4 + 2] * col[2] + a[i * 4 + 3] * col[3];
        }
    }
}

// D3D11 with R32F maps (hwPCF 0): the SM4 shaders sample s5/s6 (and the mask's s8/s9) with SampleCmp, which needs
// the comparison sampler only depth maps get.
static bool CompareUnusable(const ShadowView& view) {
    return !view.pcf && g_theGxDevicePtr->DeviceApi() == GxApi_D3d11;
}

// Shared with the caustics (VolumetricFogWater.h).
void SunShadowRegisters(bool on, float canopy, const float viewToWorld[16], float shadow[7][4], CGxTex* maps[2]) {
    std::memset(shadow, 0, sizeof(float) * 28);
    maps[0] = maps[1] = nullptr;
    ShadowView view;
    if (!on || !CShadowCache::GetPublished(view) || CompareUnusable(view)) {
        return;
    }

    for (int band = 0; band < 2; band++) {
        ComposeShadowRows(viewToWorld, view, view.rows[band + 1], &shadow[band * 3]);
        maps[band] = view.map[band + 1];
    }
    shadow[6][0] = 1.0f;
    shadow[6][1] = static_cast<float>(view.pcf);
    shadow[6][2] = canopy;
    shadow[6][3] = static_cast<float>(view.size);
}

namespace {

// One RenderFog frame: what the setup acquired and computed, read by the passes (audit O3). Every resource (targets,
// shaders, the depth, the fade depth) is acquired by the setup before any pass draws.
struct FogFrame {
    const FrameInputs& in;
    const Config& cfg;
    double seconds;
    CGxDevice* device;
    WorldViewport vp;
    CGxTex* prevTarget;
    float vpX, vpY, vpW, vpH;
    float vpMinX, vpMaxX, vpMinY, vpMaxY;
    uint32_t scale, lowW, lowH;
    float viewToWorld[16];
    float worldToView[16];
    bool noisy;
    CGxTex* authoredNoise;
    CGxTex* densityNoise;
    FogParams fog;
    uint32_t lightCount;
    bool lit;
    CGxShader* unlitMarch;
    CGxShader* litMarch;
    CGxShader* composite;
    CGxShader* splitComposite;
    bool temporal;
    CGxTex* depth;
    CGxTex* fadeDepth;
    const float* proj;
    float toLightInView[3];
    float viewport[6];
    bool historyValid;
    C44Matrix reprojection;
    float lights[4 * kMaxLocalLights][4];
    float variation[4];
    LayerNoiseRegisters noiseRegisters[kSceneLayers];
    float celestialLight[4];
    float march[4];
    float localControl[4];
    float sunPx[2];
    float markers[2][4];   // vfogSunMarker: sun then moon (pixel x, y, shown, 0)
    float rayStrength;
    uint32_t rayW, rayH;
    bool rays;
    CGxTex* copy;       // CompositePass: the scene copy (linear blend, rays, fade), or nullptr
    float shadow[8][4]; // vf_shadow.hlsli c56-c62 (control x = 0: sun unshadowed), c63 terrain (w = 0: off)
    CGxTex* terrainShadow;  // s7, gfxTerrainShadow
    bool sunShadowed;       // a cascade or the terrain shadow is tested (c62.x, c63.w)
    CGxTex* shadowMaps[2];
};

// vfogShadowedSun (SHADOW_PORT_PLAN S3): bands 1 and 2 of the engine's sun shadow maps, their texgen rows composed
// with this frame's view (vfog view space -> camera-relative world -> the view PostUpdate built them in, which
// CShadowQuery::Update refreshes every world frame), so vf_shadow.hlsli reads them as the receivers do. Off
// (control 0) without the bands (extShadowQuality <= 2) and on D3D11 with R32F maps (hwPCF 0): its SM4 marches
// sample s5/s6 with SampleCmp, which needs the comparison sampler only depth maps get.
// Also the shadow mask's (RenderShadowMask): the outputs are passed in, not a FogFrame.
void ShadowConstants(bool shadowedSun, float canopy, const FrameInputs& in, const float viewToWorld[16], float (&shadow)[8][4],
                     CGxTex* (&shadowMaps)[2], CGxTex*& terrainShadow) {
    SunShadowRegisters(shadowedSun, canopy, viewToWorld, shadow, shadowMaps);   // c56-c62, shared with the caustics
    std::memset(shadow[7], 0, sizeof(shadow[7]));
    // gfxTerrainShadow: the land (ADT and WDL) blocks the sun past the cascades, which terrain does not cast into.
    TerrainSunShadow::Update(in.toLight, in.camPos, terrainShadow, shadow[7]);
}

void BindShadows(const FogFrame& f, bool on) {
    if (on) {
        SetPs(kShadowRegister, &f.shadow[0][0], 8);
        SetShadowFadeRegister();
    }
    Bind(kShadowStage, on ? f.shadowMaps[0] : nullptr);
    Bind(kShadowStage + 1, on ? f.shadowMaps[1] : nullptr);
    Bind(kShadowStage + 2, on ? f.terrainShadow : nullptr);
}

void MarchPass(const FogFrame& f) {
    // ---- march (coa :1369-1430) ----------------------------------------------------------------------
    SetPs(9, f.celestialLight, 1);
    SetPs(11, f.march, 1);
    SetPs(kLayerRegister, &f.fog.layers[0].start, 6 * kFogLayers);
    SetPs(kLayerNoiseRegister, &f.noiseRegisters[0].octaveOffsetAndInverseTile[0][0], 4 * kSceneLayers);
    SetPs(kLocalControlRegister, f.localControl, 1);
    SetPs(kVariationRegister, f.variation, 1);
    SetPs(kLocalLightRegister, &f.lights[0][0], 4 * kMaxLocalLights);
    Bind(0, f.depth);
    Bind(9, f.densityNoise);
    Bind(10, f.authoredNoise);
    BindShadows(f, true);

    // wxl-vfog ec1c66b: the lit march only over the texels a light can reach, the unlit one over the rest.
    TexelRect litTexels = LocalLightTexels(f.lights, f.lightCount, f.proj, f.vpW, f.vpH, f.scale, f.lowW, f.lowH);
    bool litAnywhere = f.lit && litTexels.right > litTexels.left && litTexels.bottom > litTexels.top;
    bool litEverywhere = litAnywhere && litTexels.left == 0 && litTexels.top == 0
        && litTexels.right == static_cast<int32_t>(f.lowW) && litTexels.bottom == static_cast<int32_t>(f.lowH);
    if (litAnywhere && !litEverywhere) {
        // Each texel marched once: the unlit march as up to four bands around the lit rect, then the lit one in it.
        // With a target bound, Gx scissor y runs top-down (CGxDeviceD3d::IStateSyncScissorRect).
        GxRenderTargetSet(GxBuffers_Color, s_targets.march, 0);
        GxRsSet(GxRs_ScissorTest, 1);
        auto drawRect = [&](CGxShader* shader, int32_t left, int32_t top, int32_t right, int32_t bottom) {
            if (right <= left || bottom <= top) {
                return;
            }
            CRect scissor;
            scissor.minX = static_cast<float>(left) / f.lowW;
            scissor.maxX = static_cast<float>(right) / f.lowW;
            scissor.minY = static_cast<float>(top) / f.lowH;
            scissor.maxY = static_cast<float>(bottom) / f.lowH;
            f.device->SetScissor(scissor);
            f.device->RsSet(GxRs_PixelShader, shader);
            DrawQuad(f.lowW, f.lowH);
        };
        const int32_t w = static_cast<int32_t>(f.lowW), h = static_cast<int32_t>(f.lowH);
        const TexelRect& r = litTexels;
        drawRect(f.unlitMarch, 0, 0, w, r.top);
        drawRect(f.unlitMarch, 0, r.bottom, w, h);
        drawRect(f.unlitMarch, 0, r.top, r.left, r.bottom);
        drawRect(f.unlitMarch, r.right, r.top, w, r.bottom);
        drawRect(f.litMarch, r.left, r.top, r.right, r.bottom);
        GxRsSet(GxRs_ScissorTest, 0);
    } else {
        DrawToTarget(s_targets.march, litEverywhere ? f.litMarch : f.unlitMarch);
    }

    Bind(9, nullptr);
    Bind(10, nullptr);
    BindShadows(f, false);
}

// Returns the fog the composite reads: the march, or with vfogTemporal the filtered history.
CGxTex* TemporalPass(const FogFrame& f) {
    // ---- temporal + history depth (coa FilterWithHistory :980-1004) ---------------------------------
    // A shadowed sun (cascades or terrain) counts as changing light: its shafts are in the air, not on the surface the history
    // is reprojected by, so where the orbit camera's move shifts them the stale ones are rejected, not blended in (ghosts).
    // vfog38 (F7): only the cascades' shafts ghost; the land's 16 yd soft shadow does not need the lighting reject.
    bool adaptive = f.lit || s_history.localLightCount > 0 || f.cfg.noiseAmount > 0.0f || f.noisy || f.shadow[6][0] > 0.0f;
    CGxTex* fogResult = s_targets.march;
    if (f.temporal) {
        int read = s_history.index;
        int write = read ^ 1;
        float temporalConstants[4] = { f.cfg.temporal, f.historyValid ? 1.0f : 0.0f, adaptive ? 1.0f : 0.0f, 0.0f };
        SetPs(9, reinterpret_cast<const float*>(&f.reprojection), 4);     // c9-c12
        SetPs(13, temporalConstants, 1);
        Bind(0, s_targets.march);
        Bind(1, s_targets.history[read]);
        Bind(2, f.depth);
        Bind(3, s_targets.historyDepth[read]);
        DrawToTarget(s_targets.history[write], s_shaders.temporal);

        Bind(1, nullptr);
        Bind(2, nullptr);
        Bind(3, nullptr);
        Bind(0, f.depth);
        DrawToTarget(s_targets.historyDepth[write], s_shaders.historyDepth);
        s_history.index = write;
        fogResult = s_targets.history[write];
    }

    return fogResult;
}

void CompositePass(FogFrame& f, CGxTex* fogResult) {
    // ---- composite onto the scene target (coa :1450-1499), premultiplied as straight alpha ----------
    GxRenderTargetSet(GxBuffers_Color, f.prevTarget, 0);
    ScissorToWorld(f.vp);

    // coa's default for ColorSpace=1: blend in linear light over a copy of the scene, drawn opaque; the
    // fixed-function alpha blend is the fallback when the copy fails (or the world is not on the back buffer).
    // The god ray mask reads the same copy (coa CopySceneForGodRays :1440, taken whenever rays draw).
    f.copy = (f.fog.linear || f.rays || f.fadeDepth) && !f.prevTarget ? CopySceneBeforeFog(static_cast<uint32_t>(f.vpX), static_cast<uint32_t>(f.vpY), static_cast<uint32_t>(f.vpW), static_cast<uint32_t>(f.vpH)) : nullptr;
    CGxTex* sceneCopy = f.fog.linear ? f.copy : nullptr;
    s_overSceneCopy = sceneCopy != nullptr;
    GxRsSet(GxRs_BlendingMode, sceneCopy ? GxBlend_Opaque : GxBlend_Alpha);
    const Blend blend = sceneCopy ? Blend_LinearOverSceneCopy : f.fog.linear ? Blend_LinearFixedFunction : Blend_GammaFixedFunction;
    const float glow = sceneCopy && f.cfg.glowCompensation ? f.in.clientGlowAmount : 0.0f;

    float exposure = f.fog.authored ? f.cfg.classicExposure : f.cfg.exposure;
    float compositeConstants[3][4] = {
        { exposure, 0.0f, static_cast<float>(f.cfg.debugView), static_cast<float>(blend) },
        { f.fog.rayColor[0], f.fog.rayColor[1], f.fog.rayColor[2], 0.0f },    // w = 0: no dither
        { f.markers[0][0], f.markers[0][1], f.markers[0][2], glow },                               // coa m_drawnGlowCompensation
    };
    const FogParams compositeFog = f.lit ? WithMeanNoise(f.fog) : f.fog;
    SetPs(9, f.celestialLight, 1);
    SetPs(11, f.march, 1);
    SetPs(kLayerRegister, &compositeFog.layers[0].start, 6 * kFogLayers);
    SetPs(kCompositeRegister, &compositeConstants[0][0], 3);
    SetPs(kMoonMarkerRegister, f.markers[1], 1);
    SetPs(kLayerNoiseRegister, &f.noiseRegisters[0].octaveOffsetAndInverseTile[0][0], 4 * kSceneLayers);   // AnyLayerNoise (0d6a2f6)
    Bind(0, f.depth);
    Bind(1, fogResult);
    Bind(2, nullptr);
    Bind(3, sceneCopy);
    Bind(4, s_targets.march);
    Bind(kMarchDepthStage, f.depth);   // the upsample's tap depths; stays the behind depth for the fade over-pass
    // The full-res fallback march (vf_composite FogWithoutMatchingTap, at silhouettes no low-res tap matches) samples
    // the noise as the march does (coa leaves them bound). Unbound, Vulkan's white slot doubled the density there and
    // D3D's black one dropped it: a fog-coloured outline round objects in front of the fog.
    Bind(9, f.densityNoise);
    Bind(10, f.authoredNoise);
    BindShadows(f, true);   // its full-res re-march at edges (vf_composite FogWithoutMatchingTap)
    f.device->RsSet(GxRs_PixelShader, f.composite);
    // VFogComposite reads TEXCOORD0 as uv across the world viewport (vf_common FullPixelFromUv: c0.xy + uv * c0.zw).
    if (f.splitComposite) {
        DrawCompositeBySampleDepth(f.composite, f.splitComposite, sceneCopy != nullptr, f.fadeDepth != nullptr, f.vp);   // MSAA: INTZ is a copy, the MS depth stays bound
    } else {
        f.device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
        DrawQuad(static_cast<uint32_t>(f.vpW), static_cast<uint32_t>(f.vpH), f.vpMinX, f.vpMaxX, f.vpMinY, f.vpMaxY);
        f.device->DeviceSceneDepthBound(true);
    }

    if (f.fadeDepth && f.copy) {
        // wxl-vfog Renderer::DrawOverMarked: out = lerp(frame fogged behind, the unfogged copy fogged at the model's
        // own depth, alpha), the alpha in eight steps from the stencil's top three bits (step 0 is left as is).
        // The depth-stencil stays bound for the test; s0 is the R32F copy, so nothing samples it.
        // s0 is the depth with the models, s8 (kMarchDepthStage) still the depth behind them that the fog was marched
        // at: a model pixel matches no upsample tap and is fogged at its own depth, not handed the background's fog.
        // MSAA with the split: its near side (c80 = 0, nothing clipped) fogs a marked sample at the nearest depth of
        // its 3x3, i.e. the model's own where sample 0 is the background behind its edge (else a halo of background fog).
        compositeConstants[0][3] = static_cast<float>(f.fog.linear ? Blend_LinearOverSceneCopy : Blend_GammaOverSceneCopy);
        SetPs(kCompositeRegister, &compositeConstants[0][0], 1);
        if (f.splitComposite) {
            const float nearSide[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            SetPs(kSampleSideRegister, nearSide, 1);
            f.device->RsSet(GxRs_PixelShader, f.splitComposite);
        }
        Bind(0, f.fadeDepth);
        Bind(3, f.copy);
        GxRsSet(GxRs_BlendingMode, GxBlend_ConstantAlpha);
        for (int32_t step = 1; step <= 7; step++) {
            const float weight = step == 7 ? 1.0f : (2 * step + 1) / 16.0f + 0.5f / 255.0f;
            SetStencil(GxStencilCmp_Equal, step << 5, kFadeBits, 0, GxStencilOp_Keep);
            GxRsSet(Unk84, weight);     // BLENDFACTOR (GxBlend_ConstantAlpha)
            DrawQuad(static_cast<uint32_t>(f.vpW), static_cast<uint32_t>(f.vpH), f.vpMinX, f.vpMaxX, f.vpMinY, f.vpMaxY);
        }
        GxRsSet(GxRs_StencilTest, 0);
        Bind(0, f.depth);
    }
    Bind(9, nullptr);
    Bind(10, nullptr);
    Bind(kMarchDepthStage, nullptr);
    BindShadows(f, false);
}

void GodRayPass(const FogFrame& f) {
    // ---- god ray mask + radial blur (coa DrawGodRayMask :1200-1232) ----------------------------------
    // coa draws these at the end of the world in its early mode (RenderGodRaysAfterWorld), from the scene copy it
    // took here: the same inputs, so drawing them now gives the same rays. RenderGodRays composites them.
    if (f.rays && f.copy) {
        GxRsSet(GxRs_ScissorTest, 0);
        GxRsSet(GxRs_ColorWrite, 15);
        GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
        const float mask[2][4] = {
            { f.toLightInView[0], f.toLightInView[1], f.toLightInView[2], kRayFalloff },
            { 0.0f, kRayThreshold, 1.0f / f.rayW, 1.0f / f.rayH },
        };
        SetPs(9, &mask[0][0], 2);
        for (int stage = 1; stage <= 4; stage++) {
            Bind(stage, nullptr);
        }
        Bind(0, f.depth);
        Bind(1, f.copy);
        f.device->DeviceSceneDepthBound(false);
        DrawToTarget(s_targets.rays[1], s_shaders.rayMask);
        f.device->DeviceSceneDepthBound(true);
        Bind(1, nullptr);

        float norm = 0.0f;
        for (int k = 0; k < kRayTaps; k++) {
            norm += std::pow(kRayDecay, static_cast<float>(k));
        }
        float step = kRayStep;
        for (int pass = 0; pass < 2; pass++) {
            const float blur[2][4] = {
                { (f.sunPx[0] - f.vpX) / f.vpW, (f.sunPx[1] - f.vpY) / f.vpH, step, 1.0f / norm },
                { static_cast<float>(f.rayW), static_cast<float>(f.rayH), 1.0f / f.rayW, 1.0f / f.rayH },
            };
            SetPs(9, &blur[0][0], 2);
            Bind(0, s_targets.rays[pass == 0 ? 1 : 0]);
            DrawToTarget(s_targets.rays[pass == 0 ? 0 : 1], s_shaders.rayBlur);
            step /= kRayTaps;
        }
        GxRenderTargetSet(GxBuffers_Color, f.prevTarget, 0);

        s_godRays.pending = true;
        s_godRays.strength = f.rayStrength;
        std::memcpy(s_godRays.colour, f.fog.rayColor, sizeof(s_godRays.colour));
        s_godRays.glow = f.cfg.glowCompensation ? f.in.clientGlowAmount : 0.0f;
    }
}

void StoreHistory(const FogFrame& f) {
    // coa :1510-1522
    std::memcpy(s_history.worldToView, f.worldToView, sizeof(f.worldToView));
    std::memcpy(s_history.proj, f.proj, sizeof(s_history.proj));
    std::memcpy(s_history.camPos, f.in.camPos, sizeof(s_history.camPos));
    std::memcpy(s_history.viewport, f.viewport, sizeof(f.viewport));
    s_history.scale = f.scale;
    s_history.config = f.cfg;
    s_history.mapId = f.in.mapId;
    s_history.lightSlot = f.in.screenEffectSlot;
    s_history.localLightCount = static_cast<int>(f.lightCount);
    s_history.seconds = f.seconds;
    s_history.valid = f.temporal;
}

}  // namespace

bool RenderFog(const FogParams& fogIn, const FrameInputs& in, const Config& cfg, uint32_t frame, double seconds) {
    CGxDevice* device = g_theGxDevicePtr;
    if (!device || s_depthUnsupported || s_shaderMissing) {
        return false;
    }

    s_godRays.pending = false;
    // Taken once, so an early return cannot leave it armed for a later frame (whose stencil the split would then keep).
    const bool fadeArmed = s_fade.armed;
    s_fade.armed = false;
    const WorldViewport vp = LiveWorldViewport();
    CGxTex* prevTarget = vp.target;
    const float vpX = vp.x, vpY = vp.y, vpW = vp.w, vpH = vp.h;
    const float vpMinX = vp.minX, vpMaxX = vp.maxX, vpMinY = vp.minY, vpMaxY = vp.maxY;

    uint32_t scale = cfg.quality == 1 ? 4u : 2u;
    uint32_t lowW = (static_cast<uint32_t>(vpW) + scale - 1) / scale;
    uint32_t lowH = (static_cast<uint32_t>(vpH) + scale - 1) / scale;
    if (vpW < 1.0f || vpH < 1.0f || lowW < kMinTargetSize || lowH < kMinTargetSize) {
        return false;
    }

    float viewToWorld[16];
    float worldToView[16];
    if (!WorldViewFromCameraRelative(in, viewToWorld, worldToView)) {
        return false;
    }

    int quality = std::clamp(cfg.quality, 1, 3);
    bool noisy = AnyLayerNoise(fogIn);
    CGxTex* authoredNoise = noisy ? AtlasTex(s_authoredNoise, AuthoredNoiseAtlas(), "VFogAuthoredNoise") : nullptr;
    // coa DrawableFog :957: without the authored volume the noisy layers draw at their mean.
    FogParams fog = noisy && !authoredNoise ? WithMeanNoise(fogIn) : fogIn;
    noisy = noisy && authoredNoise;

    // coa compiles the density variation into every march and composite; here it lives in the N variants.
    CGxTex* densityNoise = cfg.noiseAmount > 0.0f ? AtlasTex(s_densityNoise, DensityNoiseAtlas(), "VFogDensityNoise") : nullptr;
    const bool noiseVariant = noisy || densityNoise != nullptr;

    uint32_t lightCount = cfg.localLights ? static_cast<uint32_t>(std::clamp(in.localLightCount, 0, kMaxLocalLights)) : 0;
    bool lit = lightCount > 0;
    CGxShader* unlitMarch = VariantShader(s_shaders.march, "VFogMarch", quality, false, noiseVariant);
    CGxShader* litMarch = lit ? VariantShader(s_shaders.march, "VFogMarch", quality, true, noiseVariant) : unlitMarch;
    CGxShader* composite = VariantShader(s_shaders.composite, "VFogComposite", quality, lit, noiseVariant);
    // vfogMsaaSplit: only with a multisampled depth (D3D9 RESZ, D3D11, Vulkan MSAA).
    CGxShader* splitComposite = cfg.msaaSplit && device->m_format.m_multisampleCount > 1 && StencilAvailable() && !vp.target
            && Shader(s_shaders.silhouette, "VFogSilhouette")
        ? VariantShader(s_shaders.compositeSplit, "VFogCompositeSplit", quality, lit, noiseVariant)
        : nullptr;
    bool temporal = cfg.temporal > 0.0f;
    if (!unlitMarch || !litMarch || !composite || (temporal && (!Shader(s_shaders.temporal, "VFogTemporal") || !Shader(s_shaders.historyDepth, "VFogHistoryDepth")))) {
        s_shaderMissing = true;
        return false;
    }

    const EGxTexFormat colourFormat = FogColourFormat(cfg);
    if (!EnsureTargets(lowW, lowH, colourFormat)) {
        if (colourFormat == GxTex_Argb8888) {
            return false;
        }
        s_hdrCreateFailed = true;
        if (!EnsureTargets(lowW, lowH, GxTex_Argb8888)) {
            return false;
        }
    }

    // Scene depth after the liquid pass (doc section 2 step 1).
    bool depthTransient = false;
    CGxTex* depth = device->DeviceCopySceneDepth(&depthTransient);
    if (!depth) {
        s_depthUnsupported = !depthTransient;
        return false;
    }

    if (vpX < 0.0f || vpY < 0.0f || vpX + vpW > depth->m_width || vpY + vpH > depth->m_height) {
        return false;
    }

    // wxl-vfog 0.6.3 (vfogFadeStencil): with the fading models' depth behind them kept, the frame is fogged from that
    // depth, and the composite at the live depth goes over each model by its stencil-marked alpha (after the composite).
    // Where the backend's depth is already a snapshot (D3D9 RESZ, D3D11, Vulkan) it is the "with" depth as is; only
    // D3D9's live INTZ needs the R32F copy (audit P9).
    CGxTex* fadeDepth = nullptr;
    if (fadeArmed && s_fade.behind && !prevTarget) {
        if (device->DeviceSceneDepthIsCopy()) {
            fadeDepth = depth;
        } else if (CopyRawDepth(s_fade.with, depth)) {
            fadeDepth = s_fade.with;
        }
        if (fadeDepth) {
            depth = s_fade.behind;
        }
    }

    const float* proj = in.glProjection;

    // coa MapWorldDepth :200: viewZ = c3.y / (d - c3.x) inside the main band [minZ, maxZ].
    float worldMinZ = in.viewport[4];
    float worldExtent = in.viewport[5] - in.viewport[4];
    float atInfinity = worldMinZ + worldExtent * (1.0f + proj[10]) * 0.5f;
    float perInverseViewDepth = worldExtent * proj[14] * 0.5f;

    float toLightInView[3];
    TransformDirection(in.toLight, in.cameraRelativeView, toLightInView);
    float lightLen = std::sqrt(toLightInView[0] * toLightInView[0] + toLightInView[1] * toLightInView[1] + toLightInView[2] * toLightInView[2]);
    if (lightLen > 1e-6f) {
        toLightInView[0] /= lightLen;
        toLightInView[1] /= lightLen;
        toLightInView[2] /= lightLen;
    }

    // coa :1297-1309 c0-c8.
    float common[9][4] = {
        { vpX, vpY, vpW, vpH },
        { static_cast<float>(scale), static_cast<float>(frame % 1024u), 1.0f / depth->m_width, 1.0f / depth->m_height },
        { proj[0], proj[5], proj[8], proj[9] },
        { atInfinity, perInverseViewDepth, fog.maxDistance, in.viewport[5] },
        {},
        {},
        {},
        {},
        { static_cast<float>(lowW), static_cast<float>(lowH), 1.0f / lowW, 1.0f / lowH },
    };
    std::memcpy(common[4], viewToWorld, sizeof(viewToWorld));

    // coa :1311-1332 history validity, keyed on the view (water reflection re-runs world passes).
    float viewport[6] = { vpX, vpY, vpW, vpH, in.viewport[4], in.viewport[5] };
    float dx = in.camPos[0] - s_history.camPos[0];
    float dy = in.camPos[1] - s_history.camPos[1];
    float dz = in.camPos[2] - s_history.camPos[2];
    float forwardDot = worldToView[2] * s_history.worldToView[2] + worldToView[6] * s_history.worldToView[6] + worldToView[10] * s_history.worldToView[10];
    bool historyValid = s_history.valid && temporal && s_history.scale == scale && SameSettings(cfg, s_history.config)
        && in.mapId == s_history.mapId && in.screenEffectSlot == s_history.lightSlot && forwardDot > kHistoryMinForwardDot
        && std::memcmp(s_history.proj, proj, sizeof(s_history.proj)) == 0 && std::memcmp(s_history.viewport, viewport, sizeof(viewport)) == 0
        && seconds - s_history.seconds < kHistoryMaxSeconds && seconds >= s_history.seconds
        && dx * dx + dy * dy + dz * dz < kHistoryMaxMove * kHistoryMaxMove;

    // cViewToPreviousClip = viewToWorld * prevWorldToView * prevProj (coa :1333-1336).
    C44Matrix reprojection = ToMatrix(viewToWorld) * (ToMatrix(s_history.worldToView) * ToMatrix(s_history.proj));

    // Local lights in view space, 4 registers each (replaces coa's 32x1 light texture, :1378-1418).
    float lights[4 * kMaxLocalLights][4] = {};
    float marchLightLimit = 0.0f;
    for (uint32_t i = 0; i < lightCount; i++) {
        const LocalLight& light = in.localLights[i];
        float relative[3] = { light.position[0] - in.camPos[0], light.position[1] - in.camPos[1], light.position[2] - in.camPos[2] };
        float position[3];
        TransformDirection(relative, in.cameraRelativeView, position);
        float* reg = lights[i * 4];
        reg[0] = position[0];
        reg[1] = position[1];
        reg[2] = position[2];
        reg[3] = light.cutoff;
        std::memcpy(lights[i * 4 + 1], light.color, sizeof(light.color));
        std::memcpy(lights[i * 4 + 2], light.attenuation, sizeof(light.attenuation));

        double x = position[0];
        double y = position[1];
        double z = position[2];
        float limit = static_cast<float>(std::sqrt(x * x + y * y + z * z) + light.cutoff + 0.001);
        marchLightLimit = std::max(marchLightLimit, std::nextafter(limit, std::numeric_limits<float>::infinity()));
    }

    // coa :1424-1429 density variation, the wind drift wrapped at the noise period.
    double windPeriod = kDensityNoiseSize / std::max(cfg.noiseScale, 0.001f);
    float variation[4] = { densityNoise ? cfg.noiseAmount : 0.0f, cfg.noiseScale, static_cast<float>(std::fmod(seconds * cfg.noiseWindSpeed, windPeriod)), 0.0f };

    // coa UploadLayerNoise :966: advance the authored noise by the clamped frame step.
    double noiseStep = s_noiseSeconds >= 0.0 ? std::clamp(seconds - s_noiseSeconds, 0.0, kMaxNoiseStepSeconds) : 0.0;
    s_noiseSeconds = seconds;
    AdvanceNoise(fog, in.camPos, noiseStep);
    LayerNoiseRegisters noiseRegisters[kSceneLayers];
    NoiseRegisters(fog, noiseRegisters);

    float celestialLight[4] = { toLightInView[0], toLightInView[1], toLightInView[2], fog.lightAboveHorizon };
    float march[4] = { temporal ? 1.0f : 0.0f, fog.maxDistance, fog.horizonStart, fog.farClip };
    float localControl[4] = { static_cast<float>(lightCount), marchLightLimit, cfg.localLightPhase, 0.0f };

    // Sun position for c98 and the god rays (coa :1338-1347), and vfogSunMarker's dot (coa sunInFront).
    float sunPx[2] = {};
    float sunScreenFade = 0.0f;
    if (toLightInView[2] > 0.05f) {
        float ndcX = toLightInView[0] / toLightInView[2] * proj[0] + proj[8];
        float ndcY = toLightInView[1] / toLightInView[2] * proj[5] + proj[9];
        sunPx[0] = vpX + (ndcX * 0.5f + 0.5f) * vpW;
        sunPx[1] = vpY + (0.5f - ndcY * 0.5f) * vpH;
        sunScreenFade = std::clamp(1.6f - std::max(std::fabs(ndcX), std::fabs(ndcY)), 0.0f, 1.0f);
    }

    // vfogSunMarker: a dot on the sun and one on the moon (DayNight positions about the sky centre), each when in front.
    float markers[2][4] = {};
    if (cfg.sunMarker) {
        const C3Vector* toward[2] = { &DayNight::g_sunToward, &DayNight::g_moonToward };
        for (int i = 0; i < 2; ++i) {
            float dir[3] = { toward[i]->x, toward[i]->y, toward[i]->z };
            float v[3];
            TransformDirection(dir, in.cameraRelativeView, v);
            float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len < 1e-6f || v[2] / len <= 0.05f) {
                continue;
            }
            markers[i][0] = vpX + ((v[0] / v[2] * proj[0] + proj[8]) * 0.5f + 0.5f) * vpW;
            markers[i][1] = vpY + (0.5f - (v[1] / v[2] * proj[5] + proj[9]) * 0.5f) * vpH;
            markers[i][2] = 1.0f;
        }
    }

    // coa :1344. The rays need the back buffer (scene copies) and their three shaders; without them the fog still draws.
    // vfog39: the rays come from the shown disc (in.toLight, gfxSunArc's g_lightBody, not gfxSunArcMatch's capped
    // shading light): none at or under the horizon, full by 3 degrees (lightVisibility keeps the fog's sunset glow),
    // and none when the land hides the disc (gfxTerrainShadow, the glare's GetPointOcclusion test).
    const float rayElevation = SmoothStep(0.0f, 0.0523f, in.toLight[2]);
    const float rayLand = rayElevation > 0.0f ? TerrainSunShadow::Visibility(in.camPos, in.toLight) : 0.0f;
    const float rayStrength = cfg.godRays * fog.lightVisibility * sunScreenFade * rayElevation * rayLand;
    const uint32_t rayW = std::max(kMinTargetSize, static_cast<uint32_t>(vpW) / kRayScale);
    const uint32_t rayH = std::max(kMinTargetSize, static_cast<uint32_t>(vpH) / kRayScale);
    const bool rays = rayStrength > kMinGodRayStrength && !prevTarget && Shader(s_shaders.rayMask, "VFogRayMask")
        && Shader(s_shaders.rayBlur, "VFogRayBlur") && Shader(s_shaders.rayComposite, "VFogRayComposite")
        && EnsureRayTargets(rayW, rayH);

    FogFrame f { in, cfg };
    f.seconds = seconds;
    f.device = device;
    f.vp = vp;
    f.prevTarget = prevTarget;
    f.vpX = vpX; f.vpY = vpY; f.vpW = vpW; f.vpH = vpH;
    f.vpMinX = vpMinX; f.vpMaxX = vpMaxX; f.vpMinY = vpMinY; f.vpMaxY = vpMaxY;
    f.scale = scale; f.lowW = lowW; f.lowH = lowH;
    std::memcpy(f.viewToWorld, viewToWorld, sizeof(viewToWorld));
    std::memcpy(f.worldToView, worldToView, sizeof(worldToView));
    f.noisy = noisy;
    f.authoredNoise = authoredNoise;
    f.densityNoise = densityNoise;
    f.fog = fog;
    f.lightCount = lightCount;
    f.lit = lit;
    f.unlitMarch = unlitMarch;
    f.litMarch = litMarch;
    f.composite = composite;
    f.splitComposite = splitComposite;
    f.temporal = temporal;
    f.depth = depth;
    f.fadeDepth = fadeDepth;
    f.proj = proj;
    std::memcpy(f.toLightInView, toLightInView, sizeof(toLightInView));
    std::memcpy(f.viewport, viewport, sizeof(viewport));
    f.historyValid = historyValid;
    f.reprojection = reprojection;
    std::memcpy(f.lights, lights, sizeof(lights));
    std::memcpy(f.variation, variation, sizeof(variation));
    std::memcpy(f.noiseRegisters, noiseRegisters, sizeof(noiseRegisters));
    std::memcpy(f.celestialLight, celestialLight, sizeof(celestialLight));
    std::memcpy(f.march, march, sizeof(march));
    std::memcpy(f.localControl, localControl, sizeof(localControl));
    std::memcpy(f.sunPx, sunPx, sizeof(sunPx));
    std::memcpy(f.markers, markers, sizeof(markers));
    f.rayStrength = rayStrength;
    f.rayW = rayW; f.rayH = rayH;
    f.rays = rays;
    f.copy = nullptr;
    ShadowConstants(cfg.shadowedSun, cfg.canopy, in, viewToWorld, f.shadow, f.shadowMaps, f.terrainShadow);
    // vfog38 (shadow audit F7): vfogShadowedSun 0 leaves the fog's sun unshadowed, the land's shadow too.
    if (!cfg.shadowedSun) {
        std::memset(f.shadow[7], 0, sizeof(f.shadow[7]));
        f.terrainShadow = nullptr;
    }
    // F6: the canopy cut on the haze glow fades with the light it is the shadow of (elevation, moon, handover), and
    // works with the land's shadow alone (no cascades).
    f.shadow[6][2] = cfg.shadowedSun ? cfg.canopy * fog.lightVisibility * DayNight::g_lightStrength : 0.0f;
    // c11.x, the march jitter (vf_integrate StepJitter): 1 animated (vfogTemporal), 0 none (coa), and 0.5 spatial only
    // when temporal is off but a sun shadow is tested: its compares at fixed mid-steps draw the shadow volumes as shells
    // round the camera scaled by each pixel's depth, a warped copy of the scene in the fog fixed to the screen.
    f.sunShadowed = f.shadow[6][0] > 0.0f || f.shadow[7][3] > 0.0f;
    if (!temporal && f.sunShadowed) {
        f.march[0] = 0.5f;
    }

    // ---- state: everything below is undone at the end --------------------------------------------
    static SavedPassState s_saved;     // 2 KB of constants, off the stack
    BeginPasses(s_saved);

    SetPs(0, &common[0][0], 9);
    SetPs(kHorizonBandRegister, in.horizonBand, 1);
    // c79: x = vfogDither strength on the 8-bit outputs (composite, back buffer), z = on the fog stores, which
    // only the 8-bit targets need (half float has 11 bits of mantissa). y = frame.
    const float ditherStrength = static_cast<float>(cfg.dither);
    const bool floatStores = s_targets.march->m_format == GxTex_Abgr16161616F;
    const float dither[4] = { ditherStrength, static_cast<float>(frame % 1024u), floatStores ? 0.0f : ditherStrength, 0.0f };
    SetPs(kDitherRegister, dither, 1);

    MarchPass(f);
    CGxTex* fogResult = TemporalPass(f);
    CompositePass(f, fogResult);
    GodRayPass(f);

    // ---- restore ------------------------------------------------------------------------------------
    EndPasses(s_saved, vp);
    StoreHistory(f);
    return true;
}

// coa DrawGodRaysOverScene :1234-1260: the blurred rays over a copy of the finished world, drawn opaque.
void RenderGodRays() {
    CGxDevice* device = g_theGxDevicePtr;
    if (!s_godRays.pending || !device) {
        return;
    }
    s_godRays.pending = false;

    const WorldViewport vp = LiveWorldViewport();
    if (vp.target || vp.w < 1.0f || vp.h < 1.0f) {
        return;
    }
    CGxTex* scene = CopySceneBeforeFog(static_cast<uint32_t>(vp.x), static_cast<uint32_t>(vp.y), static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h));
    if (!scene) {
        return;     // coa "no scene copy for the god rays"
    }

    static SavedPassState s_saved;
    BeginPasses(s_saved);
    ScissorToWorld(vp);
    const float viewport[4] = { vp.x, vp.y, vp.w, vp.h };
    const float composite[3][4] = {
        { 0.0f, s_godRays.strength, 0.0f, 0.0f },
        { s_godRays.colour[0], s_godRays.colour[1], s_godRays.colour[2], 0.0f },
        { 0.0f, 0.0f, 0.0f, s_godRays.glow },
    };
    SetPs(0, viewport, 1);
    SetPs(kCompositeRegister, &composite[0][0], 3);
    Bind(2, s_targets.rays[1]);
    Bind(3, scene);
    device->RsSet(GxRs_PixelShader, s_shaders.rayComposite);
    DrawQuad(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), vp.minX, vp.maxX, vp.minY, vp.maxY);
    EndPasses(s_saved, vp);
}

// coa GradingRenderer::GradeTarget / DrawGraded: copy the world viewport, then draw the graded copy over it.
bool RenderGrade(const float* curve32, float strength, int dither, uint32_t frame, const WorldViewport& world) {
    CGxDevice* device = g_theGxDevicePtr;
    if (!device || world.w < 1.0f || world.h < 1.0f || !Shader(s_shaders.grade, "VFogGrade")) {
        return false;
    }
    const WorldViewport live = LiveWorldViewport();
    if (live.target) {
        return false;       // coa: grades only the render target the world was drawn into, the back buffer here
    }
    CGxTex* scene = CopySceneBeforeFog(static_cast<uint32_t>(world.x), static_cast<uint32_t>(world.y), static_cast<uint32_t>(world.w), static_cast<uint32_t>(world.h));
    if (!scene) {
        return false;       // coa "scene copy failed"
    }

    static SavedPassState s_saved;
    BeginPasses(s_saved);
    ScissorToWorld(world);
    const float viewport[4] = { world.x, world.y, world.w, world.h };
    float grade[1 + kGradingCurveEntries / 4][4] = { { strength, 0.0f, 0.0f, 0.0f } };
    std::memcpy(grade[1], curve32, kGradingCurveEntries * sizeof(float));
    const float ditherOn[4] = { static_cast<float>(dither), static_cast<float>(frame % 1024u), 0.0f, 0.0f };
    SetPs(0, viewport, 1);
    SetPs(kGradeRegister, &grade[0][0], 1 + kGradingCurveEntries / 4);
    SetPs(kDitherRegister, ditherOn, 1);
    Bind(0, scene);
    device->RsSet(GxRs_PixelShader, s_shaders.grade);
    DrawQuad(static_cast<uint32_t>(world.w), static_cast<uint32_t>(world.h), world.minX, world.maxX, world.minY, world.maxY);
    EndPasses(s_saved, live);
    return true;
}

// shadowMask (Legion survey 2.7; docs/LEGION_PORT.md): the land's sun shadow (gfxTerrainShadow) over the opaque world,
// one multiply pass from the scene depth, so every receiver (terrain, WMO, M2, the far WDL) darkens under a mountain
// toward its ambient. The cascades are read only to leave what the stock receivers already darkened alone.
// ponytail: after the receivers, not before them (no depth pre-pass): the receivers keep their own cascade sampling, so
// the cascades are not softened here and a stock tree shadow is not widened; that needs a depth pre-pass or two-phase
// opaque so the receivers can read the mask (SHADOW_PORT_PLAN S4a/S4b replaced by one mask read).
bool RenderShadowMask(const FrameInputs& in, float softYards, bool stockFogInColour) {
    CGxDevice* device = g_theGxDevicePtr;
    if (!device || s_depthUnsupported || !Shader(s_shaders.shadowMask, "VFogShadowMask")) {
        return false;
    }
    float viewToWorld[16];
    float worldToView[16];
    if (!WorldViewFromCameraRelative(in, viewToWorld, worldToView)) {
        return false;
    }
    float shadow[8][4];
    CGxTex* maps[2];
    CGxTex* land = nullptr;
    ShadowConstants(true, 0.0f, in, viewToWorld, shadow, maps, land);
    if (!land) {
        return false;     // gfxTerrainShadow 0, no WDL, or the light under the horizon: nothing to add
    }
    shadow[7][3] = 1.0f / std::max(softYards, 1.0f);
    // vfog38 (shadow audit M1): the receivers take min(unit map, band 0/1/2); bands 1/2 are c56-c61, band 0 and the
    // unit map go in c65-c70 / s8, s9 (c71 = band 0 on, unit on). The unit map exists at every shadow quality, so
    // c62.y/w (compare mode, size) are set for it even where the bands are off (c62.x 0).
    float extra[7][4] = {};
    CGxTex* band0 = nullptr;
    CGxTex* unit = nullptr;
    ShadowView view;
    if (CShadowCache::GetPublishedUnit(view) && !CompareUnusable(view)) {
        ComposeShadowRows(viewToWorld, view, view.unitRows, &extra[3]);
        unit = view.unitMap;
        extra[6][1] = 1.0f;
        shadow[6][1] = static_cast<float>(view.pcf);
        shadow[6][3] = static_cast<float>(view.size);
        if (shadow[6][0] > 0.0f && CShadowCache::GetPublished(view)) {
            ComposeShadowRows(viewToWorld, view, view.rows[0], &extra[0]);
            band0 = view.map[0];
            extra[6][0] = 1.0f;
        }
    }

    const WorldViewport vp = LiveWorldViewport();
    bool transient = false;
    CGxTex* depth = device->DeviceCopySceneDepth(&transient);
    if (!depth || vp.w < 1.0f || vp.h < 1.0f || vp.x < 0.0f || vp.y < 0.0f || vp.x + vp.w > depth->m_width || vp.y + vp.h > depth->m_height) {
        return false;
    }

    const float* proj = in.glProjection;
    const float worldMinZ = in.viewport[4];
    const float worldExtent = in.viewport[5] - in.viewport[4];
    float common[9][4] = {
        { vp.x, vp.y, vp.w, vp.h },
        { 1.0f, 0.0f, 1.0f / depth->m_width, 1.0f / depth->m_height },
        { proj[0], proj[5], proj[8], proj[9] },
        { worldMinZ + worldExtent * (1.0f + proj[10]) * 0.5f, worldExtent * proj[14] * 0.5f, std::max(in.horizonBand[3], 1.0f), in.viewport[5] },
        {}, {}, {}, {}, {},
    };
    std::memcpy(common[4], viewToWorld, sizeof(viewToWorld));
    const auto channel = [](uint32_t argb, int shift) { return static_cast<float>((argb >> shift) & 0xFFu) / 255.0f; };
    const float span = in.fogEnd - in.fogStart;
    // vfog38 (shadow audit F2): ndl and the ratio take the light the receivers shaded with (m_light1, after PickLightBody:
    // gfxSunArcMatch's capped height, the handover's diffuse/ambient); the land texture keeps the true sun (in.toLight).
    // c12.w: how much of the light a stock receiver's cascade shadow takes away (0.7 + 0.3 x lit), times gfxShadowFade.
    const float mask[4][4] = {
        { in.shadeToLight[0], in.shadeToLight[1], in.shadeToLight[2], (1.0f - in.interiorBlend) * DayNight::g_shadowFade },   // gfxShadowFade
        { channel(in.shadeAmbient, 16), channel(in.shadeAmbient, 8), channel(in.shadeAmbient, 0), kShadowMaskSurfaceBias },
        { channel(in.shadeDiffuse, 16), channel(in.shadeDiffuse, 8), channel(in.shadeDiffuse, 0), kReceiverShadowStrength * DayNight::g_shadowFade },
        { in.fogStart, span > 1.0f ? 1.0f / span : 0.0f, stockFogInColour && span > 1.0f ? 1.0f : 0.0f, 0.0f },
    };

    static SavedPassState s_saved;
    BeginPasses(s_saved);
    GxRsSet(GxRs_BlendingMode, GxBlend_Mod);
    GxRsSet(GxRs_ColorWrite, 7);
    SetPs(0, &common[0][0], 9);
    SetPs(kShadowMaskRegister, &mask[0][0], 4);
    SetPs(kShadowRegister, &shadow[0][0], 8);
    SetPs(kShadowMaskBandRegister, &extra[0][0], 7);
    SetPs(kHorizonBandRegister, in.horizonBand, 1);
    Bind(0, depth);
    Bind(kShadowStage, maps[0]);
    Bind(kShadowStage + 1, maps[1]);
    Bind(kShadowStage + 2, land);
    Bind(kShadowStage + 3, band0);
    Bind(kShadowStage + 4, unit);
    BindNormalBuffer();
    device->RsSet(GxRs_PixelShader, s_shaders.shadowMask);
    device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
    DrawQuad(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), vp.minX, vp.maxX, vp.minY, vp.maxY);
    device->DeviceSceneDepthBound(true);
    for (int stage = kShadowStage; stage < kShadowStage + 5; stage++) {
        Bind(stage, nullptr);
    }
    EndPasses(s_saved, vp);
    return true;
}

// SSAO (Legion survey 2.1, FEATURE_BACKLOG #5; docs/LEGION_PORT.md): ambient occlusion from the scene depth over the
// opaque world, a multiply like the shadow mask. AO into a target at the world viewport / scale (SSAO 1 half, 2 full),
// optionally blurred there (SSAOBlur 1 box, 2 depth-weighted), then one bilinear apply pass (GxBlend_Mod).
// ponytail: the apply upsamples bilinearly (no depth test), so at half res the AO can bleed one pixel past a
// silhouette; a joint-bilateral upsample (4 depth taps per pixel) fixes it if it shows.
bool RenderSsao(const FrameInputs& in, int mode, int blur, float distance, float strength, bool stockFogInColour) {
    static const char* const kNames[4] = { "LegionSsao", "LegionSsaoBox", "LegionSsaoBilateral", "LegionSsaoApply" };
    CGxDevice* device = g_theGxDevicePtr;
    if (!device || s_depthUnsupported || blur < 0 || blur > 2) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!Shader(s_shaders.ssao[i], kNames[i])) {
            return false;
        }
    }
    const WorldViewport vp = LiveWorldViewport();
    bool transient = false;
    CGxTex* depth = device->DeviceCopySceneDepth(&transient);
    if (!depth || vp.w < 1.0f || vp.h < 1.0f || vp.x < 0.0f || vp.y < 0.0f || vp.x + vp.w > depth->m_width || vp.y + vp.h > depth->m_height) {
        return false;
    }

    const uint32_t scale = mode == 2 ? 1 : 2;
    const uint32_t w = std::max(kMinTargetSize, (static_cast<uint32_t>(vp.w) + scale - 1) / scale);
    const uint32_t h = std::max(kMinTargetSize, (static_cast<uint32_t>(vp.h) + scale - 1) / scale);
    CGxTex*(&ao)[2] = s_targets.ao;
    if (!ao[0] || !ao[1] || ao[0]->m_width != w || ao[0]->m_height != h) {
        DestroyTex(ao[0]);
        DestroyTex(ao[1]);
        ao[0] = CreateTarget(w, h, GxTex_Linear);
        ao[1] = CreateTarget(w, h, GxTex_Linear);
        if (!ao[0] || !ao[1]) {
            DestroyTex(ao[0]);
            DestroyTex(ao[1]);
            return false;
        }
    }

    const float* proj = in.glProjection;
    const float worldMinZ = in.viewport[4];
    const float worldExtent = in.viewport[5] - in.viewport[4];
    const float common[4][4] = {
        { vp.x, vp.y, vp.w, vp.h },
        { static_cast<float>(scale), 0.0f, 1.0f / depth->m_width, 1.0f / depth->m_height },
        { proj[0], proj[5], proj[8], proj[9] },
        { worldMinZ + worldExtent * (1.0f + proj[10]) * 0.5f, worldExtent * proj[14] * 0.5f, std::max(in.horizonBand[3], 1.0f), in.viewport[5] },
    };
    const float target[4] = { static_cast<float>(w), static_cast<float>(h), 1.0f / w, 1.0f / h };
    distance = std::max(distance, 1.0f);
    const float span = in.fogEnd - in.fogStart;
    // Radius 1.5 yd (contact shading under props and in corners); the fade covers the last half of SSAODistance.
    const float ssao[2][4] = {
        { 1.5f, distance * 0.5f, 2.0f / distance, strength },
        { in.fogStart, span > 1.0f ? 1.0f / span : 0.0f, stockFogInColour && span > 1.0f ? 1.0f : 0.0f, 0.0f },
    };

    static SavedPassState s_saved;
    BeginPasses(s_saved);
    SetPs(0, &common[0][0], 4);
    SetPs(8, target, 1);
    SetPs(kHorizonBandRegister, in.horizonBand, 1);
    SetPs(kSsaoRegister, &ssao[0][0], 2);
    device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
    Bind(0, depth);
    BindNormalBuffer();     // gxNormalBuffer (the AO pass only; stage 4 is unbound by EndPasses)
    DrawToTarget(ao[0], s_shaders.ssao[0]);
    if (blur) {
        Bind(1, ao[0]);
        DrawToTarget(ao[1], s_shaders.ssao[blur]);
    }
    Bind(0, nullptr);
    device->DeviceSceneDepthBound(true);
    GxRenderTargetSet(GxBuffers_Color, vp.target, 0);

    GxRsSet(GxRs_BlendingMode, GxBlend_Mod);
    GxRsSet(GxRs_ColorWrite, 7);
    const float shift[4] = { blur ? 0.5f : 0.0f, 0.0f, 0.0f, 0.0f };     // the 4x4 box is centred half a texel back
    SetPs(kSsaoRegister, shift, 1);
    Bind(1, ao[blur ? 1 : 0]);
    device->RsSet(GxRs_PixelShader, s_shaders.ssao[3]);
    DrawQuad(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), vp.minX, vp.maxX, vp.minY, vp.maxY);
    EndPasses(s_saved, vp);
    return true;
}

// LEGION FFX post (ffxAntiAliasingMode, FFXColorShift): FXAA and the colour matrix over a bilinear copy of the world
// viewport, like RenderGrade (after it, before names and the UI). docs/LEGION_PORT.md.
bool RenderPost(int fxaa, const float colourShift[3][4], const WorldViewport& world) {
    static const char* const kNames[3] = { "LegionPost0", "LegionPost1", "LegionPost2" };
    CGxDevice* device = g_theGxDevicePtr;
    if (!device || fxaa < 0 || fxaa > 2 || world.w < 1.0f || world.h < 1.0f || !Shader(s_shaders.post[fxaa], kNames[fxaa])) {
        return false;
    }
    const WorldViewport live = LiveWorldViewport();
    if (live.target) {
        return false;
    }
    if (!s_callbackAdded) {     // the scene copy goes with a reset even while vfog is off
        device->AddDeviceRestoredCallback(OnDeviceRestored);
        s_callbackAdded = true;
    }
    CGxTex* scene = CopySceneBeforeFog(static_cast<uint32_t>(world.x), static_cast<uint32_t>(world.y), static_cast<uint32_t>(world.w), static_cast<uint32_t>(world.h));
    if (!scene) {
        return false;
    }

    static SavedPassState s_saved;
    BeginPasses(s_saved);
    ScissorToWorld(world);
    const float viewport[4] = { world.x, world.y, world.w, world.h };
    // FXAA 3.11 quality defaults: subpix 0.75, edge threshold 0.166 / 0.125, edge threshold min 0.0833 / 0.0625.
    const float fxaaParams[4] = { 0.75f, fxaa == 2 ? 0.125f : 0.166f, fxaa == 2 ? 0.0625f : 0.0833f, 0.0f };
    SetPs(0, viewport, 1);
    SetPs(kPostRegister, fxaaParams, 1);
    SetPs(kPostRegister + 1, &colourShift[0][0], 3);
    Bind(0, scene);
    device->RsSet(GxRs_PixelShader, s_shaders.post[fxaa]);
    DrawQuad(static_cast<uint32_t>(world.w), static_cast<uint32_t>(world.h), world.minX, world.maxX, world.minY, world.maxY);
    EndPasses(s_saved, live);
    return true;
}

// LEGION refraction (survey 2.6, docs/LEGION_PORT.md): heat haze over tagged fire emitters. Each world box is projected
// on the CPU to a screen rect and drawn as one scissored LegionRefraction quad over a copy of the world viewport.
bool RenderRefraction(const FrameInputs& in, const float (*boxes)[6], uint32_t count, bool high, float strength, double seconds) {
    CGxDevice* device = g_theGxDevicePtr;
    if (!count || !device || s_depthUnsupported || !Shader(s_shaders.refraction, "LegionRefraction")) {
        return false;
    }
    float viewToWorld[16];
    float worldToView[16];
    const WorldViewport vp = LiveWorldViewport();
    if (vp.target || vp.w < 1.0f || vp.h < 1.0f || !WorldViewFromCameraRelative(in, viewToWorld, worldToView)) {
        return false;
    }
    const float* proj = in.glProjection;
    const auto mul = [](const float v[4], const float* m, float out[4]) {
        for (int j = 0; j < 4; j++) {
            out[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
        }
    };
    struct Quad {
        float rect[4];      // px, top-left origin, unclipped (the mask's frame)
        CRect scissor;      // normalised, Gx (bottom-up) like the viewport
        float haze[4];
        float size[4];
    };
    static std::vector<Quad> quads;
    quads.clear();
    const float t = static_cast<float>(std::fmod(seconds, 1024.0));
    for (uint32_t i = 0; i < count; i++) {
        const float* b = boxes[i];
        float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }, nearZ = 1e30f;
        for (int c = 0; c < 8; c++) {
            const float p[4] = { b[(c & 1) ? 3 : 0], b[(c & 2) ? 4 : 1], b[(c & 4) ? 5 : 2], 1.0f };
            float v[4], clip[4];
            mul(p, worldToView, v);
            mul(v, proj, clip);
            nearZ = std::min(nearZ, clip[3]);
            if (clip[3] > 0.1f) {
                for (int k = 0; k < 2; k++) {
                    lo[k] = std::min(lo[k], clip[k] / clip[3]);
                    hi[k] = std::max(hi[k], clip[k] / clip[3]);
                }
            }
        }
        // ponytail: a box the nearZ plane cuts (camera in the fire) is skipped, not clipped
        if (nearZ < 0.5f || lo[0] >= 1.0f || hi[0] <= -1.0f || lo[1] >= 1.0f || hi[1] <= -1.0f) {
            continue;
        }
        Quad q;
        q.rect[0] = vp.x + (lo[0] * 0.5f + 0.5f) * vp.w;
        q.rect[1] = vp.y + (0.5f - hi[1] * 0.5f) * vp.h;
        q.rect[2] = vp.x + (hi[0] * 0.5f + 0.5f) * vp.w;
        q.rect[3] = vp.y + (0.5f - lo[1] * 0.5f) * vp.h;
        const float sx = vp.maxX - vp.minX;
        const float sy = vp.maxY - vp.minY;
        q.scissor.minX = vp.minX + (std::max(lo[0], -1.0f) * 0.5f + 0.5f) * sx;
        q.scissor.maxX = vp.minX + (std::min(hi[0], 1.0f) * 0.5f + 0.5f) * sx;
        q.scissor.minY = vp.minY + (std::max(lo[1], -1.0f) * 0.5f + 0.5f) * sy;
        q.scissor.maxY = vp.minY + (std::min(hi[1], 1.0f) * 0.5f + 0.5f) * sy;
        const float width = std::max(b[3] - b[0], b[4] - b[1]);
        const float height = b[5] - b[2];
        // ~2 cm of shimmer per yard of flame (capped), in pixels at the source's distance, at most 12 px
        const float yards = 0.02f * strength * std::clamp(height, 0.5f, 4.0f);
        const float pixels = std::min(yards * proj[5] * 0.5f * vp.h / nearZ, 12.0f * strength);
        if (pixels < 0.25f) {
            continue;
        }
        q.haze[0] = nearZ;
        q.haze[1] = pixels;
        q.haze[2] = t;
        q.haze[3] = high ? 1.0f : 0.0f;
        q.size[0] = width;
        q.size[1] = height;
        q.size[2] = q.size[3] = 0.0f;
        quads.push_back(q);
    }
    if (quads.empty()) {
        return false;
    }
    bool transient = false;
    CGxTex* depth = device->DeviceCopySceneDepth(&transient);
    if (!depth || vp.x < 0.0f || vp.y < 0.0f || vp.x + vp.w > depth->m_width || vp.y + vp.h > depth->m_height) {
        return false;
    }
    CGxTex* scene = CopySceneBeforeFog(static_cast<uint32_t>(vp.x), static_cast<uint32_t>(vp.y), static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h));
    if (!scene) {
        return false;
    }
    const float worldMinZ = in.viewport[4];
    const float worldExtent = in.viewport[5] - in.viewport[4];
    const float common[4][4] = {     // vf_common c0-c3, as RenderShadowMask
        { vp.x, vp.y, vp.w, vp.h },
        { 1.0f, 0.0f, 1.0f / depth->m_width, 1.0f / depth->m_height },
        { proj[0], proj[5], proj[8], proj[9] },
        { worldMinZ + worldExtent * (1.0f + proj[10]) * 0.5f, worldExtent * proj[14] * 0.5f, std::max(in.horizonBand[3], 1.0f), in.viewport[5] },
    };

    static SavedPassState s_saved;
    BeginPasses(s_saved);
    GxRsSet(GxRs_ColorWrite, 7);
    GxRsSet(GxRs_ScissorTest, 1);
    SetPs(0, &common[0][0], 4);
    SetPs(kHorizonBandRegister, in.horizonBand, 1);
    Bind(0, depth);
    Bind(1, scene);
    device->RsSet(GxRs_PixelShader, s_shaders.refraction);
    device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
    for (const Quad& q : quads) {
        device->SetScissor(q.scissor);
        SetPs(kRefractRegister, q.rect, 1);
        SetPs(kRefractRegister + 1, q.haze, 1);
        SetPs(kRefractRegister + 2, q.size, 1);
        DrawQuad(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), vp.minX, vp.maxX, vp.minY, vp.maxY);
    }
    device->DeviceSceneDepthBound(true);
    EndPasses(s_saved, vp);
    return true;
}

bool CompositeOverSceneCopy() {
    return s_overSceneCopy;
}

void RendererShutdown() {
    WaterShutdown();
    CataWater::Shutdown();
    DestroyTargets();
    DestroyFadeDepth();
    DestroyTex(s_densityNoise);
    DestroyTex(s_authoredNoise);
    TerrainSunShadow::Release();

    if (g_theGxDevicePtr) {
        CGxShader** all = &s_shaders.march[0][0][0];
        for (size_t i = 0; i < sizeof(s_shaders) / sizeof(CGxShader*); i++) {
            if (all[i]) {
                g_theGxDevicePtr->ShaderDestroy(all[i]);
            }
        }

        if (s_callbackAdded) {
            g_theGxDevicePtr->RemoveDeviceRestoredCallback(OnDeviceRestored);
        }
    }

    s_shaders = Shaders {};
    s_history = History {};
    s_godRays = GodRays {};
    s_noiseSeconds = -1.0;
    s_callbackAdded = false;
    s_depthUnsupported = false;
    s_shaderMissing = false;
}

}  // namespace VolumetricFog
