// VFOG WP7: frame driver, CVars and engine inputs (port of coa-vfog src/hooks.cpp, src/engine.cpp,
// src/engine_lights.cpp onto the decomp's own symbols). Not in the 3.3.5a binary; docs/VFOG_PORT.md sections 1, 7, 8.
#include "WoW/Source/WorldClient/VolumetricFog.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/VolumetricFogDriver.h"
#include "WoW/Source/WorldClient/World.h"

namespace VolumetricFog {
namespace {

// coa-vfog hooks.cpp kOutOfRangeStockFogStart: the stock fog of both DNInfo groups goes past everything while drawing.
constexpr float kOutOfRangeStockFogStart = 50000.0f;
// The stock horizon band (World.cpp s_horizonMinZ / s_horizonMaxZ, RenderHorizon's viewport depth range).
constexpr float kHorizonMinZ = 0.998046875f;
constexpr float kHorizonMaxZ = 0.9990234375f;
// The visible sun's day window (DayNight::DNClouds::SetupBumpMap; coa engine.cpp 0xA41CA4 / 0xA41CA0).
constexpr float kSunDayStart = 0.2013889f;
constexpr float kSunDayEnd = 0.9236111f;
// wxl-vfog kStockFogFitDepth: the screen-wide fit covers this much view depth; models beyond it get their own fit.
constexpr float kFitRange = 100.0f;
// coa-vfog engine_lights.h
constexpr float kLocalPointLightContributionCutoff = 1.0f / 256.0f;
constexpr float kMaxLocalPointLightRadius = 200.0f;
constexpr float kLocalLightGamma = 2.2f;
constexpr float kMaxWorldCoordinate = 1000000.0f;
constexpr float kMaxLightComponent = 10000.0f;
constexpr float kMaxUploadedLightComponent = 1000000.0f;
constexpr float kMaxAttenuationCoefficient = 1000000.0f;
constexpr int kLightGridCells = 64 * 64;   // CM2Scene::m_lightGrid (CM2Light::Link)
constexpr int kLightParamsSlots = 8;

struct CVars {
    CVar* enable;
    CVar* quality;
    CVar* temporal;
    CVar* density;
    CVar* haze;
    CVar* groundFog;
    CVar* farFog;
    CVar* ambient;
    CVar* data;
    CVar* localLights;
    CVar* transparent;
    CVar* startOffset;
    CVar* maxDistance;
    CVar* noise;
    CVar* debug;
};

struct SavedFog {
    float start;
    float end;
    float interiorStart;
    float interiorEnd;
};

CVars s_cvars = {};
Config s_cfg;
FrameInputs s_in = {};
FogParams s_params = {};
StockFogFit s_fit = {};
StockFogFit s_modelFit = {};
float s_modelFitPos[3] = {};
bool s_modelFitCached = false;
SavedFog s_saved = {};
bool s_pushed = false;
bool s_active = false;
bool s_wasActive = false;
bool s_opaqueValid = false;
bool s_armed = false;          // the composite is on the frame: later batches take the fitted fog
bool s_dataLoaded = false;
uint32_t s_frame = 0;

float CVarFloat(CVar* var, float fallback) {
    return var && std::isfinite(var->m_floatValue) ? var->m_floatValue : fallback;
}

int CVarInt(CVar* var, int fallback) {
    return var ? var->m_intValue : fallback;
}

Config ReadConfig() {
    Config cfg;
    cfg.enable = CVarInt(s_cvars.enable, 1) != 0;
    cfg.quality = std::clamp(CVarInt(s_cvars.quality, cfg.quality), 1, 3);
    cfg.temporal = std::clamp(CVarFloat(s_cvars.temporal, cfg.temporal), 0.0f, 0.98f);
    cfg.density = std::max(CVarFloat(s_cvars.density, cfg.density), 0.0f);
    cfg.haze = std::max(CVarFloat(s_cvars.haze, cfg.haze), 0.0f);
    cfg.groundFog = std::max(CVarFloat(s_cvars.groundFog, cfg.groundFog), 0.0f);
    cfg.farFog = std::max(CVarFloat(s_cvars.farFog, cfg.farFog), 0.0f);
    cfg.ambient = std::max(CVarFloat(s_cvars.ambient, cfg.ambient), 0.0f);
    cfg.dataMode = std::clamp(CVarInt(s_cvars.data, cfg.dataMode), 0, 1);
    cfg.localLights = CVarInt(s_cvars.localLights, 1) != 0;
    cfg.transparentFog = CVarInt(s_cvars.transparent, 1) != 0;
    cfg.startOffset = std::max(CVarFloat(s_cvars.startOffset, cfg.startOffset), 0.0f);
    cfg.maxDistance = std::max(CVarFloat(s_cvars.maxDistance, cfg.maxDistance), 100.0f);
    cfg.noiseAmount = std::clamp(CVarFloat(s_cvars.noise, cfg.noiseAmount), 0.0f, 1.0f);
    cfg.debugView = std::clamp(CVarInt(s_cvars.debug, 0), 0, 3);
    return cfg;
}

uint32_t Argb(const CImVector& c) {
    return (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.b;
}

float Clamp01(float v) {
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}

bool Finite(const float* v, int count) {
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

bool IsPerspective(const float* p) {
    return Finite(p, 16) && std::fabs(p[11] - 1.0f) < 1e-3f && std::fabs(p[15]) < 1e-3f && std::fabs(p[0]) > 1e-4f && std::fabs(p[5]) > 1e-4f;
}

// coa engine.cpp GlowScreenEffectRunsUnsafe: FFX on, the glow effect current and its own CVar on.
bool GlowScreenEffectRuns() {
    FFX::Effect* effect = FFX::s_effect;
    return FFX::s_cvarFFX && FFX::s_cvarFFX->m_intValue && effect && effect == CGWorldFrame::ffxEffectGlow && effect->m_cvar && effect->m_cvar->m_intValue;
}

void RestoreStockFog() {
    if (!s_pushed) {
        return;
    }
    DayNight::DNInfo& info = DayNight::g_dnInfo;
    info.m_fog.m_start = s_saved.start;
    info.m_fog.m_end = s_saved.end;
    info.m_fogInterior.m_start = s_saved.interiorStart;
    info.m_fogInterior.m_end = s_saved.interiorEnd;
    s_pushed = false;
}

// coa hooks.cpp PushStockFogOutOfRange, both DNInfo fog groups (m_fog, m_fogInterior).
void PushStockFogOutOfRange() {
    DayNight::DNInfo& info = DayNight::g_dnInfo;
    s_saved = { info.m_fog.m_start, info.m_fog.m_end, info.m_fogInterior.m_start, info.m_fogInterior.m_end };
    info.m_fog.m_start = kOutOfRangeStockFogStart;
    info.m_fog.m_end = kOutOfRangeStockFogStart * 2.0f;
    info.m_fogInterior.m_start = kOutOfRangeStockFogStart;
    info.m_fogInterior.m_end = kOutOfRangeStockFogStart * 2.0f;
    s_pushed = true;
}

// coa engine.cpp BuildFrameInputsUnsafe + ReadLightParamsSelection, the DNInfo half (before the push-out).
void ReadDayNightInputs(FrameInputs& in) {
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    in.dayFraction = info.m_dayProgression;
    in.fogColor = Argb(info.m_fogInterior.m_color);
    in.fogStart = info.m_fogInterior.m_start;
    in.fogEnd = info.m_fogInterior.m_end;
    in.ambientColor = Argb(info.m_bands.m_ambient);
    in.directColor = Argb(info.m_bands.m_diffuse);
    in.sunColor = Argb(info.m_bands.m_sunCloudColors[0]);
    in.zoneFogDistance = info.m_bands.m_fogEnd;
    in.farClip = World::s_farClip;
    in.inLiquid = false;
    in.clientGlowAmount = GlowScreenEffectRuns() ? Clamp01(info.m_bands.m_glow) : 0.0f;
    in.interiorBlend = Clamp01(info.m_interiorFogBlend);
    in.stormBlend = Clamp01(info.m_stormBlend);
    const int32_t slot = static_cast<int32_t>(info.m_overrideLightParamsId);
    in.screenEffectSlot = slot >= 0 && slot < kLightParamsSlots ? slot : -1;
    in.mapId = World::QueryMapId();
}

// coa engine.cpp: the visible sun or moon sprite, relative to the sky centre (DNInfo::m_cameraPos).
void ReadLightDirection(FrameInputs& in) {
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    in.lightIsMoon = !(info.m_dayProgression >= kSunDayStart && info.m_dayProgression <= kSunDayEnd);
    const C3Vector& body = in.lightIsMoon ? DayNight::g_planets.m_moon.m_position : DayNight::g_planets.m_sun.m_position;
    float d[3] = { body.x - info.m_cameraPos.x, body.y - info.m_cameraPos.y, body.z - info.m_cameraPos.z };
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-6f)) {
        d[0] = 0.0f;
        d[1] = 0.0f;
        d[2] = 1.0f;
    } else {
        d[0] /= len;
        d[1] /= len;
        d[2] /= len;
    }
    std::memcpy(in.toLight, d, sizeof(d));
}

// ---- point lights (coa engine_lights.cpp) -----------------------------------------------------------------------

bool ValidVector(const float v[3], float minimum, float maximum) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(v[i]) || v[i] < minimum || v[i] > maximum) {
            return false;
        }
    }
    return true;
}

double SquaredDistance(const float a[3], const float b[3]) {
    double result = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double delta = static_cast<double>(a[i]) - b[i];
        result += delta * delta;
    }
    return result;
}

float PeakColor(const float c[3]) {
    return std::max(c[0], std::max(c[1], c[2]));
}

float LinearChannel(float channel, float peak) {
    const float gammaChannel = std::max(channel, 0.0f);
    if (peak <= 1.0f) {
        return std::pow(gammaChannel, kLocalLightGamma);
    }
    return std::pow(gammaChannel / peak, kLocalLightGamma) * peak;
}

float PointLightCutoff(const float color[3], const float attenuation[3]) {
    if (!ValidVector(color, 0.0f, kMaxUploadedLightComponent) || !ValidVector(attenuation, 0.0f, kMaxAttenuationCoefficient)) {
        return 0.0f;
    }
    const double peak = PeakColor(color);
    const double target = peak / kLocalPointLightContributionCutoff;
    if (peak <= 0.0 || target <= std::max(static_cast<double>(attenuation[0]), 1.0)) {
        return 0.0f;
    }
    const double remaining = target - attenuation[0];
    double radius = kMaxLocalPointLightRadius;
    if (attenuation[2] > 0.0f) {
        const double linear = attenuation[1];
        radius = 2.0 * remaining / (linear + std::sqrt(linear * linear + 4.0 * attenuation[2] * remaining));
    } else if (attenuation[1] > 0.0f) {
        radius = remaining / attenuation[1];
    }
    return static_cast<float>(std::min(radius, static_cast<double>(kMaxLocalPointLightRadius)));
}

double LightPriority(const LocalLight& light, const float cam[3]) {
    const double d2 = SquaredDistance(light.position, cam);
    const double denominator = light.attenuation[0] + light.attenuation[1] * std::sqrt(d2) + light.attenuation[2] * d2;
    return PeakColor(light.color) / std::max(denominator, 1.0);
}

// coa Precedes: priority, then a stable order so equal lights do not swap between frames. `aRaw`/`bRaw` are the
// captured (pre-upload) colours coa breaks ties on.
bool Precedes(const LocalLight& a, const float aRaw[3], const LocalLight& b, const float bRaw[3], const float cam[3]) {
    const double ap = LightPriority(a, cam);
    const double bp = LightPriority(b, cam);
    if (ap != bp) {
        return ap > bp;
    }
    for (int i = 0; i < 3; ++i) {
        if (a.position[i] != b.position[i]) {
            return a.position[i] < b.position[i];
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (aRaw[i] != bRaw[i]) {
            return aRaw[i] > bRaw[i];
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (a.attenuation[i] != b.attenuation[i]) {
            return a.attenuation[i] < b.attenuation[i];
        }
    }
    return false;
}

// coa SelectLocalPointLight: keep the kMaxLocalLights strongest lights whose reach comes near the camera.
void SelectLight(FrameInputs& in, float raw[kMaxLocalLights][3], const CM2Light& native, const float cam[3], bool linear, float intensity) {
    const float position[3] = { native.m_pos.x, native.m_pos.y, native.m_pos.z };
    const float color[3] = { native.m_dirColor.x, native.m_dirColor.y, native.m_dirColor.z };
    if (!ValidVector(position, -kMaxWorldCoordinate, kMaxWorldCoordinate) || !ValidVector(color, 0.0f, kMaxLightComponent)) {
        return;
    }

    LocalLight candidate;
    std::memcpy(candidate.position, position, sizeof(position));
    candidate.attenuation[0] = native.m_constantAttenuation;
    candidate.attenuation[1] = native.m_linearAttenuation;
    candidate.attenuation[2] = native.m_quadraticAttenuation;
    const float peak = PeakColor(color);
    for (int c = 0; c < 3; ++c) {
        candidate.color[c] = (linear ? LinearChannel(color[c], peak) : std::max(color[c], 0.0f)) * intensity;
    }
    candidate.cutoff = PointLightCutoff(candidate.color, candidate.attenuation);
    const double limit = candidate.cutoff + kMaxLocalPointLightRadius;
    if (candidate.cutoff <= 0.0f || SquaredDistance(candidate.position, cam) > limit * limit) {
        return;
    }

    int index = 0;
    while (index < in.localLightCount && !Precedes(candidate, color, in.localLights[index], raw[index], cam)) {
        ++index;
    }
    if (index == kMaxLocalLights) {
        return;
    }
    const int count = std::min(in.localLightCount + 1, kMaxLocalLights);
    for (int i = count - 1; i > index; --i) {
        in.localLights[i] = in.localLights[i - 1];
        std::memcpy(raw[i], raw[i - 1], sizeof(raw[i]));
    }
    in.localLights[index] = candidate;
    std::memcpy(raw[index], color, sizeof(raw[index]));
    in.localLightCount = count;
}

// coa CapturePointLightsUnsafe on the real CM2Scene light grid: visible point lights only (CM2Light::Link links
// nothing else there, so the walk needs none of coa's table checks).
void CaptureLocalLights(FrameInputs& in, const Config& cfg) {
    in.localLightCount = 0;
    CM2Scene* scene = World::s_m2Scene;
    if (!cfg.localLights || !scene || !scene->m_lightGrid || !ValidVector(in.camPos, -kMaxWorldCoordinate, kMaxWorldCoordinate) ||
        !std::isfinite(cfg.localLightIntensity) || cfg.localLightIntensity < 0.0f) {
        return;
    }
    const bool linear = cfg.colorSpace == 1;
    float raw[kMaxLocalLights][3] = {};
    for (int cell = 0; cell < kLightGridCells; ++cell) {
        for (const CM2Light* light = scene->m_lightGrid[cell]; light; light = light->m_lightNext) {
            if (light->m_type == M2LIGHT_POINT && light->m_visible) {
                SelectLight(in, raw, *light, in.camPos, linear, cfg.localLightIntensity);
            }
        }
    }
}

// ---- batch fog (coa transparent_fog.cpp ApplyStockFogFit + wxl-vfog 0.6.13) --------------------------------------

const StockFogFit& FitFor(const float modelPos[3]) {
    const float dx = modelPos[0] - s_in.camPos[0];
    const float dy = modelPos[1] - s_in.camPos[1];
    const float dz = modelPos[2] - s_in.camPos[2];
    if (!Finite(modelPos, 3) || dx * dx + dy * dy + dz * dz <= kFitRange * kFitRange) {
        return s_fit;
    }
    // ponytail: one cached model per frame (batches of a model come in a row); a small map if far models interleave.
    if (!s_modelFitCached || std::memcmp(s_modelFitPos, modelPos, sizeof(s_modelFitPos)) != 0) {
        s_modelFit = FitStockFogAt(s_params, s_in, s_cfg, modelPos);
        std::memcpy(s_modelFitPos, modelPos, sizeof(s_modelFitPos));
        s_modelFitCached = true;
    }
    return s_modelFit;
}

bool FogParticles() {
    return s_armed && s_cfg.transparentFog;
}

}  // namespace

void RegisterCVars() {
    // VFOG section 8 defaults: coa CoAVolFog.ini plus the user's wxl-vfog look.
    s_cvars.enable = CVar::Register("vfog", "Volumetric fog (replaces the stock fog)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.quality = CVar::Register("vfogQuality", "Volumetric fog quality: 1 = quarter res 16 steps, 2 = half res 24, 3 = half res 32", 1, "2", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.temporal = CVar::Register("vfogTemporal", "Volumetric fog temporal blend (0 = off)", 1, "0.85", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.density = CVar::Register("vfogDensity", "Volumetric fog density scale", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.haze = CVar::Register("vfogHaze", "Volumetric fog haze scale", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.groundFog = CVar::Register("vfogGroundFog", "Volumetric fog ground fog scale", 1, "0.6", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.farFog = CVar::Register("vfogFarFog", "Volumetric fog distance layer scale", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.ambient = CVar::Register("vfogAmbient", "Volumetric fog ambient light scale", 1, "1.75", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.data = CVar::Register("vfogData", "Volumetric fog: 0 = derived layers only, 1 = authored fog data where it covers", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.localLights = CVar::Register("vfogLocalLights", "Volumetric fog lit by nearby point lights", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.transparent = CVar::Register("vfogTransparent", "Fit the volumetric fog onto transparents drawn after it", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.startOffset = CVar::Register("vfogStartOffset", "Volumetric fog start distance (yards)", 1, "10", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.maxDistance = CVar::Register("vfogMaxDistance", "Volumetric fog maximum march distance (yards)", 1, "5000", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.noise = CVar::Register("vfogNoise", "Volumetric fog density noise amount", 1, "0.15", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.debug = CVar::Register("vfogDebug", "Volumetric fog debug view (0..3)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);

    g_vfogM2BatchFog = &BatchFog;
    g_vfogM2FogParticles = &FogParticles;
}

bool Active() {
    return s_active;
}

void OnWorldUpdate() {
    // DayNight::UpdateFog rebuilt DNInfo fog this frame; a push left over from a frame that never rendered the world
    // (OnWorldDone not reached) must not be snapshotted as the stock fog.
    if (s_pushed) {
        RestoreStockFog();
    }
    s_armed = false;
    s_opaqueValid = false;
    s_cfg = ReadConfig();

    if (s_cfg.enable && !s_dataLoaded) {
        s_dataLoaded = true;
        LoadFogData();   // missing or invalid: the derived layers are used (coa)
    }

    s_active = s_cfg.enable && g_theGxDevicePtr->MasterEnable(GxMasterEnable_Fog) && !World::SceneCamLiquidStatus(nullptr) && RendererReady();

    if (!s_active) {
        if (s_wasActive) {
            InvalidateHistory();
        }
        s_wasActive = false;
        return;
    }

    s_wasActive = true;
    ReadDayNightInputs(s_in);
    PushStockFogOutOfRange();
}

void OnViewerLiquid() {
    // OnWorldUpdate decided on last frame's liquid status (UpdateViewerLiquid writes it later, in CWorldScene::Render,
    // which calls this right after it and before anything is drawn): the camera just went under, so the frame takes
    // the underwater branch, which has no composite. Give it the stock fog.
    if (!s_active || !World::SceneCamLiquidStatus(nullptr)) {
        return;
    }
    RestoreStockFog();
    InvalidateHistory();
    s_active = false;
    s_wasActive = false;
}

void OnOpaqueDone() {
    if (!s_active) {
        return;
    }

    FrameInputs& in = s_in;
    CGxDevice* device = g_theGxDevicePtr;

    // coa CaptureOpaqueState: the Gx view stack top is camera relative (CWorldScene::Update sets the camera at the
    // origin, World.cpp, then translates its own copy by -s_activeWorldView); the projection is the main one.
    const CGxMatrixStack& view = device->m_xforms[GxXform_View];
    std::memcpy(in.cameraRelativeView, &view.m_mtx[view.m_level], sizeof(in.cameraRelativeView));
    std::memcpy(in.glProjection, &device->m_projection, sizeof(in.glProjection));

    in.camPos[0] = CWorldScene::s_activeWorldView.x;
    in.camPos[1] = CWorldScene::s_activeWorldView.y;
    in.camPos[2] = CWorldScene::s_activeWorldView.z;
    in.camTarget[0] = CWorldScene::camTarget.x;
    in.camTarget[1] = CWorldScene::camTarget.y;
    in.camTarget[2] = CWorldScene::camTarget.z;

    // World viewport in pixels, top-left origin as the backends set it (CGxDeviceD3d::IXformSetViewport).
    float minX, maxX, minY, maxY, minZ, maxZ;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);
    const CRect& window = device->DeviceCurWindow();
    in.viewport[0] = std::floor(minX * window.maxX + 0.5f);
    in.viewport[1] = std::floor(maxX * window.maxX + 0.5f);
    in.viewport[2] = std::floor((1.0f - maxY) * window.maxY + 0.5f);
    in.viewport[3] = std::floor((1.0f - minY) * window.maxY + 0.5f);
    in.viewport[4] = minZ;
    in.viewport[5] = maxZ;

    // The stock horizon (CWorldScene::BuildHorizonProjection / RenderHorizon).
    CGCamera* camera = CGWorldFrame::GetActiveCamera();
    const float farClip = World::s_farClip;
    in.horizonBand[0] = kHorizonMinZ;
    in.horizonBand[1] = kHorizonMaxZ;
    in.horizonBand[2] = camera ? camera->m_farZ - 50.0f : farClip - 50.0f;
    in.horizonBand[3] = World::s_horizonFarClipScale * farClip;

    ReadLightDirection(in);
    CaptureLocalLights(in, s_cfg);

    s_opaqueValid = in.viewport[1] > in.viewport[0] && in.viewport[3] > in.viewport[2] && IsPerspective(in.glProjection) &&
                    Finite(in.cameraRelativeView, 16) && Finite(in.camPos, 3) && std::isfinite(in.fogStart) && std::isfinite(in.fogEnd);
}

void OnLiquidDone() {
    if (!s_active) {
        return;
    }

    const double seconds = FrameTime::s_curTimeMs * 0.001;
    bool drawn = false;
    if (s_opaqueValid) {
        s_params = BuildFogParams(s_in, s_cfg);
        drawn = RenderFog(s_params, s_in, s_cfg, s_frame++, seconds);
    }
    if (!drawn) {
        // Nothing composited: the late passes get the stock fog back, not the fitted one.
        RestoreStockFog();
        return;
    }

    s_fit = s_cfg.transparentFog ? FitStockFog(s_params, s_in, s_cfg) : StockFogFit{};
    s_modelFitCached = false;
    s_armed = true;
}

void OnWorldDone() {
    s_armed = false;
    RestoreStockFog();
}

bool LiquidDepthWrite() {
    return s_active;
}

bool BatchFog(const float modelPos[3], float& start, float& end, float& density, float color[3]) {
    if (!s_armed || !s_cfg.transparentFog) {
        return false;
    }
    const StockFogFit& fit = FitFor(modelPos);
    if (!fit.valid) {
        return false;
    }

    // wxl-vfog 0.6.13: what was drawn before the composite carries the client's own fog, so where the client line
    // has started the two lines are summed; where it has not (the stock fog is pushed out), the fit is the fog.
    const float stockSpan = end - start;
    const float fitSpan = fit.end - fit.start;
    if (!(stockSpan > 1.0f) || !(fitSpan > 0.0f) || start >= kFitRange) {
        start = fit.start;
        end = fit.end;
        density = fit.density;
        std::memcpy(color, fit.color, sizeof(fit.color));
        return true;
    }
    const float ours = stockSpan / (stockSpan + fitSpan);
    const float slope = 1.0f / stockSpan + 1.0f / fitSpan;
    start = (start / stockSpan + fit.start / fitSpan) / slope;
    end = start + 1.0f / slope;
    density = fit.density;
    for (int c = 0; c < 3; ++c) {
        color[c] += (fit.color[c] - color[c]) * ours;
    }
    return true;
}

}  // namespace VolumetricFog
