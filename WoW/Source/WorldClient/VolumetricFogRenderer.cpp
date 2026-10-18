// VFOG WP6: GPU passes, a port of coa-vfog src/renderer.cpp (RenderPasses :1265-1525) onto the Gx API.
// docs/VFOG_PORT.md sections 2-5 are the spec: pass list, targets, register and sampler contract.
// God rays: coa DrawGodRayMask / DrawGodRaysOverScene (renderer.cpp:1189-1260), always drawn after the world.
// Not ported (doc section 1): the depth probe, logging.
#include "WoW/Source/WorldClient/VolumetricFogModel.h"
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
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
constexpr uint32_t kSampleSideRegister = 80;        // VFogCompositeSplit c80 (coa c99, which is the horizon band here)
constexpr uint32_t kDitherRegister = 79;            // vf_common cDither (x on, y frame)
constexpr uint32_t kGradeRegister = 10;             // VFogGrade c10 strength, c11-c18 curve
constexpr uint32_t kLocalLightRegister = 100;
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
    DestroyTex(s_targets.sceneCopy);
    s_targets.sceneCopyWidth = 0;
    s_targets.sceneCopyHeight = 0;
    DestroyTex(s_targets.rays[0]);
    DestroyTex(s_targets.rays[1]);
    s_targets.rayWidth = 0;
    s_targets.rayHeight = 0;
    s_godRays.pending = false;
    DestroyTex(s_fade.behind);
    DestroyTex(s_fade.with);
    s_fade.armed = false;

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
        && a.stockFog == b.stockFog && a.sunMarker == b.sunMarker;
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
void DrawQuad(uint32_t width, uint32_t height, float minX, float maxX, float minY, float maxY) {
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
    SetPs(0, saved.constants, kSavedPixelRegisters);
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
void DrawCompositeBySampleDepth(CGxShader* composite, CGxShader* split, bool overwrites, const WorldViewport& vp) {
    CGxDevice* device = g_theGxDevicePtr;
    const uint32_t w = static_cast<uint32_t>(vp.w);
    const uint32_t h = static_cast<uint32_t>(vp.h);
    GxXformSetViewport(vp.minX, vp.maxX, vp.minY, vp.maxY, 0.0f, 1.0f);
    GxSceneClear(GxBufferBit_Stencil, CImVector { 0, 0, 0, 0 });
    GxRsSet(GxRs_ColorWrite, 0);
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthFunc, 3);     // LESS (s_cmpFunc[3])
    SetStencil(GxStencilCmp_Always, 1, 0xFF, 0xFF, GxStencilOp_Invert, GxStencilOp_Replace);
    device->RsSet(GxRs_PixelShader, s_shaders.silhouette);
    DrawQuad(w, h, vp.minX, vp.maxX, vp.minY, vp.maxY);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_ColorWrite, 7);

    const float nearSide[4] = { 0.0f, overwrites ? 1.0f : 0.0f, 0.0f, 0.0f };
    const float farSide[4] = { 1.0f, overwrites ? 1.0f : 0.0f, 0.0f, 0.0f };
    if (overwrites) {
        GxRsSet(GxRs_StencilTest, 0);
    } else {
        SetStencil(GxStencilCmp_Equal, 0, 0xFF, 0, GxStencilOp_Keep);
    }
    device->RsSet(GxRs_PixelShader, composite);
    DrawQuad(w, h, vp.minX, vp.maxX, vp.minY, vp.maxY);
    device->RsSet(GxRs_PixelShader, split);
    SetStencil(GxStencilCmp_Equal, 1, 0xFF, 0, GxStencilOp_Keep);
    SetPs(kSampleSideRegister, nearSide, 1);
    DrawQuad(w, h, vp.minX, vp.maxX, vp.minY, vp.maxY);
    SetStencil(GxStencilCmp_Equal, 0xFF, 0xFF, 0, GxStencilOp_Keep);
    SetPs(kSampleSideRegister, farSide, 1);
    DrawQuad(w, h, vp.minX, vp.maxX, vp.minY, vp.maxY);
    GxRsSet(GxRs_StencilTest, 0);
}

}  // namespace

bool KeepDepthBehindFading() {
    s_fade.armed = false;
    CGxDevice* device = g_theGxDevicePtr;
    // wxl-vfog: not on a multisampled depth (the sample split would clear the marks); the weights need BLENDFACTOR.
    if (!device || s_depthUnsupported || !StencilAvailable() || device->m_format.m_multisampleCount > 1 || !device->m_caps.m_blendFactor
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

bool RenderFog(const FogParams& fogIn, const FrameInputs& in, const Config& cfg, uint32_t frame, double seconds) {
    CGxDevice* device = g_theGxDevicePtr;
    if (!device || s_depthUnsupported || s_shaderMissing) {
        return false;
    }

    s_godRays.pending = false;
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
        s_fade.armed = false;
        return false;
    }

    // wxl-vfog 0.6.3 (vfogFadeStencil): with the fading models' depth behind them kept, the frame is fogged from that
    // depth, and the composite at the live depth goes over each model by its stencil-marked alpha (after the composite).
    CGxTex* fadeDepth = nullptr;
    if (s_fade.armed && !prevTarget && CopyRawDepth(s_fade.with, depth)) {
        fadeDepth = s_fade.with;
        depth = s_fade.behind;
    }
    s_fade.armed = false;

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

    // coa :1344. The rays need the back buffer (scene copies) and their three shaders; without them the fog still draws.
    const float rayStrength = cfg.godRays * fog.lightVisibility * sunScreenFade;
    const uint32_t rayW = std::max(kMinTargetSize, static_cast<uint32_t>(vpW) / kRayScale);
    const uint32_t rayH = std::max(kMinTargetSize, static_cast<uint32_t>(vpH) / kRayScale);
    const bool rays = rayStrength > kMinGodRayStrength && !prevTarget && Shader(s_shaders.rayMask, "VFogRayMask")
        && Shader(s_shaders.rayBlur, "VFogRayBlur") && Shader(s_shaders.rayComposite, "VFogRayComposite")
        && EnsureRayTargets(rayW, rayH);

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
    ScissorToWorld(vp);

    // coa's default for ColorSpace=1: blend in linear light over a copy of the scene, drawn opaque; the
    // fixed-function alpha blend is the fallback when the copy fails (or the world is not on the back buffer).
    // The god ray mask reads the same copy (coa CopySceneForGodRays :1440, taken whenever rays draw).
    CGxTex* copy = (fog.linear || rays || fadeDepth) && !prevTarget ? CopySceneBeforeFog(static_cast<uint32_t>(vpX), static_cast<uint32_t>(vpY), static_cast<uint32_t>(vpW), static_cast<uint32_t>(vpH)) : nullptr;
    CGxTex* sceneCopy = fog.linear ? copy : nullptr;
    s_overSceneCopy = sceneCopy != nullptr;
    GxRsSet(GxRs_BlendingMode, sceneCopy ? GxBlend_Opaque : GxBlend_Alpha);
    const Blend blend = sceneCopy ? Blend_LinearOverSceneCopy : fog.linear ? Blend_LinearFixedFunction : Blend_GammaFixedFunction;
    const float glow = sceneCopy && cfg.glowCompensation ? in.clientGlowAmount : 0.0f;

    float exposure = fog.authored ? cfg.classicExposure : cfg.exposure;
    float compositeConstants[3][4] = {
        { exposure, 0.0f, static_cast<float>(cfg.debugView), static_cast<float>(blend) },
        { fog.rayColor[0], fog.rayColor[1], fog.rayColor[2], 0.0f },    // w = 0: no dither
        { sunPx[0], sunPx[1], cfg.sunMarker && toLightInView[2] > 0.05f ? 1.0f : 0.0f, glow },                               // coa m_drawnGlowCompensation
    };
    const FogParams compositeFog = lit ? WithMeanNoise(fog) : fog;
    SetPs(9, celestialLight, 1);
    SetPs(11, march, 1);
    SetPs(kLayerRegister, &compositeFog.layers[0].start, 6 * kFogLayers);
    SetPs(kCompositeRegister, &compositeConstants[0][0], 3);
    Bind(0, depth);
    Bind(1, fogResult);
    Bind(2, nullptr);
    Bind(3, sceneCopy);
    Bind(4, s_targets.march);
    device->RsSet(GxRs_PixelShader, composite);
    // VFogComposite reads TEXCOORD0 as uv across the world viewport (vf_common FullPixelFromUv: c0.xy + uv * c0.zw).
    if (splitComposite) {
        DrawCompositeBySampleDepth(composite, splitComposite, sceneCopy != nullptr, vp);   // MSAA: INTZ is a copy, the MS depth stays bound
    } else {
        device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
        DrawQuad(static_cast<uint32_t>(vpW), static_cast<uint32_t>(vpH), vpMinX, vpMaxX, vpMinY, vpMaxY);
        device->DeviceSceneDepthBound(true);
    }

    if (fadeDepth && copy) {
        // wxl-vfog Renderer::DrawOverMarked: out = lerp(frame fogged behind, the unfogged copy fogged at the model's
        // own depth, alpha), the alpha in eight steps from the stencil's top three bits (step 0 is left as is).
        // The depth-stencil stays bound for the test; s0 is the R32F copy, so nothing samples it.
        compositeConstants[0][3] = static_cast<float>(fog.linear ? Blend_LinearOverSceneCopy : Blend_GammaOverSceneCopy);
        SetPs(kCompositeRegister, &compositeConstants[0][0], 1);
        Bind(0, fadeDepth);
        Bind(3, copy);
        GxRsSet(GxRs_BlendingMode, GxBlend_ConstantAlpha);
        for (int32_t step = 1; step <= 7; step++) {
            const float weight = step == 7 ? 1.0f : (2 * step + 1) / 16.0f + 0.5f / 255.0f;
            SetStencil(GxStencilCmp_Equal, step << 5, 0xE0, 0, GxStencilOp_Keep);
            GxRsSet(Unk84, weight);     // BLENDFACTOR (GxBlend_ConstantAlpha)
            DrawQuad(static_cast<uint32_t>(vpW), static_cast<uint32_t>(vpH), vpMinX, vpMaxX, vpMinY, vpMaxY);
        }
        GxRsSet(GxRs_StencilTest, 0);
        Bind(0, depth);
    }

    // ---- god ray mask + radial blur (coa DrawGodRayMask :1200-1232) ----------------------------------
    // coa draws these at the end of the world in its early mode (RenderGodRaysAfterWorld), from the scene copy it
    // took here: the same inputs, so drawing them now gives the same rays. RenderGodRays composites them.
    if (rays && copy) {
        GxRsSet(GxRs_ScissorTest, 0);
        GxRsSet(GxRs_ColorWrite, 15);
        GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
        const float mask[2][4] = {
            { toLightInView[0], toLightInView[1], toLightInView[2], kRayFalloff },
            { 0.0f, kRayThreshold, 1.0f / rayW, 1.0f / rayH },
        };
        SetPs(9, &mask[0][0], 2);
        for (int stage = 1; stage <= 4; stage++) {
            Bind(stage, nullptr);
        }
        Bind(0, depth);
        Bind(1, copy);
        device->DeviceSceneDepthBound(false);
        DrawToTarget(s_targets.rays[1], s_shaders.rayMask);
        device->DeviceSceneDepthBound(true);
        Bind(1, nullptr);

        float norm = 0.0f;
        for (int k = 0; k < kRayTaps; k++) {
            norm += std::pow(kRayDecay, static_cast<float>(k));
        }
        float step = kRayStep;
        for (int pass = 0; pass < 2; pass++) {
            const float blur[2][4] = {
                { (sunPx[0] - vpX) / vpW, (sunPx[1] - vpY) / vpH, step, 1.0f / norm },
                { static_cast<float>(rayW), static_cast<float>(rayH), 1.0f / rayW, 1.0f / rayH },
            };
            SetPs(9, &blur[0][0], 2);
            Bind(0, s_targets.rays[pass == 0 ? 1 : 0]);
            DrawToTarget(s_targets.rays[pass == 0 ? 0 : 1], s_shaders.rayBlur);
            step /= kRayTaps;
        }
        GxRenderTargetSet(GxBuffers_Color, prevTarget, 0);

        s_godRays.pending = true;
        s_godRays.strength = rayStrength;
        std::memcpy(s_godRays.colour, fog.rayColor, sizeof(s_godRays.colour));
        s_godRays.glow = cfg.glowCompensation ? in.clientGlowAmount : 0.0f;
    }

    // ---- restore ------------------------------------------------------------------------------------
    EndPasses(s_saved, vp);

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

bool CompositeOverSceneCopy() {
    return s_overSceneCopy;
}

void RendererShutdown() {
    WaterShutdown();
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
    s_godRays = GodRays {};
    s_noiseSeconds = -1.0;
    s_callbackAdded = false;
    s_depthUnsupported = false;
    s_shaderMissing = false;
}

}  // namespace VolumetricFog
