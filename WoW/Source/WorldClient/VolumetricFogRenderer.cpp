// VFOG WP6: GPU passes, a port of coa-vfog src/renderer.cpp (RenderPasses :1265-1525) onto the Gx API.
// docs/VFOG_PORT.md sections 2-5 are the spec: pass list, targets, register and sampler contract.
// Not ported (doc section 1): god rays, the scene-copy blends, the MSAA split, the depth probe, logging.
#include "WoW/Source/WorldClient/VolumetricFogModel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/Services/Texture.h"
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
constexpr uint32_t kHorizonBandRegister = 99;
constexpr uint32_t kLocalLightRegister = 100;
constexpr uint32_t kSavedPixelRegisters = kLocalLightRegister + 4 * kMaxLocalLights;    // c0-c131
constexpr float kDensityNoiseSize = 32.0f;          // coa noise_volume.h kDensityNoiseSize
constexpr double kHistoryMaxSeconds = 0.25;         // coa renderer.cpp:84-86
constexpr float kHistoryMaxMove = 30.0f;
constexpr float kHistoryMinForwardDot = 0.70710678f;
constexpr double kMaxNoiseStepSeconds = 0.25;       // coa renderer.cpp:67
constexpr float kNearQuarterTurn = 1.57f;           // wxl-vfog ec1c66b
constexpr uint32_t kMinTargetSize = 8;              // GxTexCreate's minimum

enum Blend { Blend_GammaFixedFunction = 0, Blend_LinearFixedFunction = 2 };     // coa FogBlend, no scene copy

struct Shaders {
    CGxShader* march[3][2][2];          // [quality-1][local lights][noise]
    CGxShader* composite[3][2][2];
    CGxShader* temporal;
    CGxShader* historyDepth;
};

struct Targets {
    uint32_t width;
    uint32_t height;
    CGxTex* march;
    CGxTex* history[2];
    CGxTex* historyDepth[2];
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

CGxShader* Shader(CGxShader*& slot, const char* name) {
    if (!slot) {
        g_theGxDevicePtr->ShaderCreate(&slot, GxSh_Pixel, "Shaders\\Pixel", name, 1);
    }

    return slot && slot->Valid() ? slot : nullptr;
}

CGxShader* VariantShader(CGxShader* (&table)[3][2][2], const char* base, int quality, bool lit, bool noisy) {
    char name[32];
    snprintf(name, sizeof(name), "%s%d%s%s", base, quality, lit ? "L" : "", noisy ? "N" : "");
    return Shader(table[quality - 1][lit][noisy], name);
}

void DestroyTex(CGxTex*& tex) {
    if (tex) {
        GxTexDestroy(tex);
        tex = nullptr;
    }
}

void DestroyTargets() {
    DestroyTex(s_targets.march);
    for (int i = 0; i < 2; i++) {
        DestroyTex(s_targets.history[i]);
        DestroyTex(s_targets.historyDepth[i]);
    }

    s_targets.width = 0;
    s_targets.height = 0;
    s_history.valid = false;
}

void __fastcall OnDeviceRestored() {
    DestroyTargets();
    DestroyTex(s_densityNoise);
    DestroyTex(s_authoredNoise);
    s_depthUnsupported = false;     // MSAA may have changed with the reset
}

CGxTex* CreateTarget(uint32_t width, uint32_t height) {
    CGxTex* tex = nullptr;
    CGxTexFlags flags(GxTex_Nearest, 0, 0, 0, 0, 1, 1, 0, 0, 0);
    g_theGxDevicePtr->TexCreate(GxTex_2d, width, height, 0, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, GxTexNullUserFunc, "VFogTarget", tex);
    return tex;
}

bool EnsureTargets(uint32_t width, uint32_t height) {
    if (s_targets.march && s_targets.width == width && s_targets.height == height) {
        return true;
    }

    DestroyTargets();
    s_targets.march = CreateTarget(width, height);
    for (int i = 0; i < 2; i++) {
        s_targets.history[i] = CreateTarget(width, height);
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
        && a.debugView == b.debugView;
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

void SetPs(uint32_t reg, const float* values, uint32_t count) {
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, reg, values, count);
}

void Bind(int stage, CGxTex* tex) {
    g_theGxDevicePtr->RsSet(static_cast<EGxRenderState>(GxRs_Texture0 + stage), tex);
}

// One quad over the Gx viewport [minX..maxX, minY..maxY] (normalised; default the whole target) of width x height
// pixels, through the fixed-function VS. TEXCOORD0 = uv across that viewport, (x + 0.5) / width with v = 0 on the
// top row, at every pixel centre on every backend: D3D9 rasterises at integer pixel coordinates, so its uvs carry
// the half texel (as FFX does).
void DrawQuad(uint32_t width, uint32_t height, float minX = 0.0f, float maxX = 1.0f, float minY = 0.0f, float maxY = 1.0f) {
    static uint16_t s_indices[4] = { 0, 3, 1, 2 };
    static CGxBatch s_batch = { GxPrim_TriangleStrip, 0, 4, 0, 3 };

    C44Matrix projection;
    GxuXformCreateOrtho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -1.0f, 1.0f, projection);
    GxXformSetProjection(projection);
    GxXformSetViewport(minX, maxX, minY, maxY, 0.0f, 1.0f);

    EGxApi api = g_theGxDevicePtr->DeviceApi();
    float offset = api == GxApi_D3d9 || api == GxApi_D3d9Ex ? 0.5f : 0.0f;
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

void DrawToTarget(CGxTex* target, CGxShader* shader) {
    GxRenderTargetSet(GxBuffers_Color, target, 0);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, shader);
    DrawQuad(target->m_width, target->m_height);
}

}  // namespace

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

bool RenderFog(const FogParams& fogIn, const FrameInputs& in, const Config& cfg, uint32_t frame, double seconds) {
    CGxDevice* device = g_theGxDevicePtr;
    if (!device || s_depthUnsupported || s_shaderMissing) {
        return false;
    }

    // World viewport in pixels of the current target, D3D-style top-left origin (coa D3DVIEWPORT9), from the
    // live Gx viewport: the same formula every backend uses for the back buffer (CGxDeviceD3d::IXformSetViewport).
    CGxTex* prevTarget = nullptr;
    device->DeviceGetRenderTarget(GxBuffers_Color, prevTarget);
    const CRect window = device->DeviceCurWindow();
    float vpMinX, vpMaxX, vpMinY, vpMaxY, vpMinZ, vpMaxZ;
    GxXformViewport(vpMinX, vpMaxX, vpMinY, vpMaxY, vpMinZ, vpMaxZ);
    float screenW = window.maxX - window.minX;
    float screenH = window.maxY - window.minY;
    float vpX = std::floor(vpMinX * screenW + 0.5f);
    float vpY = prevTarget ? std::floor(vpMinY * screenH + 0.5f) : std::floor((1.0f - vpMaxY) * screenH + 0.5f);
    float vpW = std::floor(vpMaxX * screenW - vpX + 0.5f);
    float vpH = prevTarget ? std::floor((vpMaxY - vpMinY) * screenH + 0.5f) : std::floor((1.0f - vpMinY) * screenH - vpY + 0.5f);

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
    bool temporal = cfg.temporal > 0.0f;
    if (!unlitMarch || !litMarch || !composite || (temporal && (!Shader(s_shaders.temporal, "VFogTemporal") || !Shader(s_shaders.historyDepth, "VFogHistoryDepth")))) {
        s_shaderMissing = true;
        return false;
    }

    if (!EnsureTargets(lowW, lowH)) {
        return false;
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

    // Sun position for c98 (coa :1338-1347); the marker and god rays are not drawn.
    float sunPx[2] = {};
    if (toLightInView[2] > 0.05f) {
        float ndcX = toLightInView[0] / toLightInView[2] * proj[0] + proj[8];
        float ndcY = toLightInView[1] / toLightInView[2] * proj[5] + proj[9];
        sunPx[0] = vpX + (ndcX * 0.5f + 0.5f) * vpW;
        sunPx[1] = vpY + (0.5f - ndcY * 0.5f) * vpH;
    }

    // ---- state: everything below is undone at the end --------------------------------------------
    float savedConstants[kSavedPixelRegisters * 4];
    std::memcpy(savedConstants, CGxDevice::s_shadowConstants[0].constants, sizeof(savedConstants));     // [0] = pixel
    C44Matrix savedProjection;
    C44Matrix savedView;
    GxXformProjection(savedProjection);
    GxXformView(savedView);
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

    SetPs(0, &common[0][0], 9);
    SetPs(kHorizonBandRegister, in.horizonBand, 1);

    // ---- march (coa :1369-1430) ----------------------------------------------------------------------
    SetPs(9, celestialLight, 1);
    SetPs(11, march, 1);
    SetPs(kLayerRegister, &fog.layers[0].start, 6 * kFogLayers);
    SetPs(kLayerNoiseRegister, &noiseRegisters[0].octaveOffsetAndInverseTile[0][0], 4 * kSceneLayers);
    SetPs(kLocalControlRegister, localControl, 1);
    SetPs(kVariationRegister, variation, 1);
    SetPs(kLocalLightRegister, &lights[0][0], 4 * kMaxLocalLights);
    Bind(0, depth);
    Bind(9, densityNoise);
    Bind(10, authoredNoise);

    // wxl-vfog ec1c66b: the lit march only over the texels a light can reach, the unlit one over the rest.
    TexelRect litTexels = LocalLightTexels(lights, lightCount, proj, vpW, vpH, scale, lowW, lowH);
    bool litAnywhere = lit && litTexels.right > litTexels.left && litTexels.bottom > litTexels.top;
    bool litEverywhere = litAnywhere && litTexels.left == 0 && litTexels.top == 0
        && litTexels.right == static_cast<int32_t>(lowW) && litTexels.bottom == static_cast<int32_t>(lowH);
    DrawToTarget(s_targets.march, litEverywhere ? litMarch : unlitMarch);
    if (litAnywhere && !litEverywhere) {
        // With a target bound, Gx scissor y runs top-down (CGxDeviceD3d::IStateSyncScissorRect).
        CRect scissor;
        scissor.minX = static_cast<float>(litTexels.left) / lowW;
        scissor.maxX = static_cast<float>(litTexels.right) / lowW;
        scissor.minY = static_cast<float>(litTexels.top) / lowH;
        scissor.maxY = static_cast<float>(litTexels.bottom) / lowH;
        device->SetScissor(scissor);
        GxRsSet(GxRs_ScissorTest, 1);
        device->RsSet(GxRs_PixelShader, litMarch);
        DrawQuad(lowW, lowH);
        GxRsSet(GxRs_ScissorTest, 0);
    }

    Bind(9, nullptr);
    Bind(10, nullptr);

    // ---- temporal + history depth (coa FilterWithHistory :980-1004) ---------------------------------
    bool adaptive = lit || s_history.localLightCount > 0 || cfg.noiseAmount > 0.0f || noisy;
    CGxTex* fogResult = s_targets.march;
    if (temporal) {
        int read = s_history.index;
        int write = read ^ 1;
        float temporalConstants[4] = { cfg.temporal, historyValid ? 1.0f : 0.0f, adaptive ? 1.0f : 0.0f, 0.0f };
        SetPs(9, reinterpret_cast<const float*>(&reprojection), 4);     // c9-c12
        SetPs(13, temporalConstants, 1);
        Bind(0, s_targets.march);
        Bind(1, s_targets.history[read]);
        Bind(2, depth);
        Bind(3, s_targets.historyDepth[read]);
        DrawToTarget(s_targets.history[write], s_shaders.temporal);

        Bind(1, nullptr);
        Bind(2, nullptr);
        Bind(3, nullptr);
        Bind(0, depth);
        DrawToTarget(s_targets.historyDepth[write], s_shaders.historyDepth);
        s_history.index = write;
        fogResult = s_targets.history[write];
    }

    // ---- composite onto the scene target (coa :1450-1499), premultiplied as straight alpha ----------
    GxRenderTargetSet(GxBuffers_Color, prevTarget, 0);
    CRect world;
    world.minX = vpMinX;
    world.maxX = vpMaxX;
    world.minY = vpMinY;
    world.maxY = vpMaxY;
    device->SetScissor(world);
    GxRsSet(GxRs_ScissorTest, 1);
    GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
    GxRsSet(GxRs_ColorWrite, 7);    // RGB

    float exposure = fog.authored ? cfg.classicExposure : cfg.exposure;
    float compositeConstants[3][4] = {
        { exposure, 0.0f, static_cast<float>(cfg.debugView), static_cast<float>(fog.linear ? Blend_LinearFixedFunction : Blend_GammaFixedFunction) },
        { fog.rayColor[0], fog.rayColor[1], fog.rayColor[2], 0.0f },    // w = 0: no dither
        { sunPx[0], sunPx[1], 0.0f, 0.0f },                               // glow compensation only over a scene copy
    };
    const FogParams compositeFog = lit ? WithMeanNoise(fog) : fog;
    SetPs(9, celestialLight, 1);
    SetPs(11, march, 1);
    SetPs(kLayerRegister, &compositeFog.layers[0].start, 6 * kFogLayers);
    SetPs(kCompositeRegister, &compositeConstants[0][0], 3);
    Bind(0, depth);
    Bind(1, fogResult);
    Bind(2, nullptr);
    Bind(3, nullptr);
    Bind(4, s_targets.march);
    device->RsSet(GxRs_PixelShader, composite);
    // VFogComposite reads TEXCOORD0 as uv across the world viewport (vf_common FullPixelFromUv: c0.xy + uv * c0.zw).
    DrawQuad(static_cast<uint32_t>(vpW), static_cast<uint32_t>(vpH), vpMinX, vpMaxX, vpMinY, vpMaxY);

    // ---- restore ------------------------------------------------------------------------------------
    Bind(0, nullptr);
    Bind(1, nullptr);
    Bind(4, nullptr);
    device->RsPop();
    GxXformPop(GxXform_World);
    GxXformSetView(savedView);
    GxXformSetProjection(savedProjection);
    GxXformSetViewport(vpMinX, vpMaxX, vpMinY, vpMaxY, vpMinZ, vpMaxZ);
    SetPs(0, savedConstants, kSavedPixelRegisters);

    // coa :1510-1522
    std::memcpy(s_history.worldToView, worldToView, sizeof(worldToView));
    std::memcpy(s_history.proj, proj, sizeof(s_history.proj));
    std::memcpy(s_history.camPos, in.camPos, sizeof(s_history.camPos));
    std::memcpy(s_history.viewport, viewport, sizeof(viewport));
    s_history.scale = scale;
    s_history.config = cfg;
    s_history.mapId = in.mapId;
    s_history.lightSlot = in.screenEffectSlot;
    s_history.localLightCount = static_cast<int>(lightCount);
    s_history.seconds = seconds;
    s_history.valid = temporal;
    return true;
}

void RendererShutdown() {
    DestroyTargets();
    DestroyTex(s_densityNoise);
    DestroyTex(s_authoredNoise);

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
    s_noiseSeconds = -1.0;
    s_callbackAdded = false;
    s_depthUnsupported = false;
    s_shaderMissing = false;
}

}  // namespace VolumetricFog
