// VFOG WP7: frame driver, CVars and engine inputs (port of coa-vfog src/hooks.cpp, src/engine.cpp,
// src/engine_lights.cpp onto the decomp's own symbols). Not in the 3.3.5a binary; docs/VFOG_PORT.md sections 1, 7, 8.
#include "WoW/Source/WorldClient/TerrainSunShadow.h"
#include "WoW/Source/WorldClient/VolumetricFog.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/CM2SceneRender.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Shared.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/CataWater.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/VolumetricFogDriver.h"
#include "WoW/Source/WorldClient/VolumetricFogModel.h"
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
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
    CVar* godRays;
    CVar* shadowedSun;
    CVar* canopy;
    CVar* grade;
    CVar* gradeStrength;
    CVar* dither;
    CVar* hdrTargets;
    CVar* fadeEarly;
    CVar* fadeStencil;
    CVar* msaaSplit;
    // coa settings that had no CVar (vfog13)
    CVar* sunScatter;
    CVar* exposure;
    CVar* classicExposure;
    CVar* classicPhase;
    CVar* colorSpace;
    CVar* localLightIntensity;
    CVar* localLightPhase;
    CVar* interior;
    CVar* interiorDensity;
    CVar* interiorFarFog;
    CVar* noiseScale;
    CVar* noiseWind;
    CVar* classicNoise;
    CVar* glowCompensation;
    CVar* underwater;
    CVar* stockFog;
    CVar* foreverGlow;
    CVar* sunMarker;
    CVar* liquidDepth;
};

// coa forever_look.cpp GradingFrame: decided at the end of the world, drawn after the glow.
struct GradeFrame {
    bool pending;
    float strength;
    float curve[kGradingCurveEntries];
    WorldViewport world;
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
// Per-model fits for this frame, keyed on the model position (batches of many models interleave back to front).
struct ModelFit {
    uint32_t stamp;
    float pos[3];
    StockFogFit fit;
};
constexpr uint32_t kModelFitSlots = 128;
ModelFit s_modelFits[kModelFitSlots] = {};
uint32_t s_fitStamp = 1;      // bumped per composite; a slot from an older stamp is empty
bool s_glareDrawn = false;    // RenderGlares ran before the composite this frame (coa DrawGlareBeforeTheFog)
SavedFog s_saved = {};
bool s_pushed = false;
bool s_active = false;
bool s_wasActive = false;
bool s_opaqueValid = false;
bool s_maskPending = false;      // shadowMask: OnOpaqueDone captured this frame's inputs for OnSolidDone
bool s_armed = false;          // the composite is on the frame: later batches take the fitted fog
GradeFrame s_grade = {};
uint32_t s_frame = 0;
bool s_late = false;         // the composite runs once after the world this frame (coa's TransparentFog=0 path)
bool s_glowFed = false;      // vfogForeverGlow replaces the glow amount this frame
float s_glowAmount = 0.0f;

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
    cfg.fadeEarly = CVarInt(s_cvars.fadeEarly, 1) != 0;
    cfg.fadeStencil = CVarInt(s_cvars.fadeStencil, 1) != 0;
    cfg.msaaSplit = CVarInt(s_cvars.msaaSplit, 1) != 0;
    cfg.startOffset = std::max(CVarFloat(s_cvars.startOffset, cfg.startOffset), 0.0f);
    cfg.maxDistance = std::max(CVarFloat(s_cvars.maxDistance, cfg.maxDistance), 100.0f);
    cfg.noiseAmount = std::clamp(CVarFloat(s_cvars.noise, cfg.noiseAmount), 0.0f, 1.0f);
    cfg.debugView = std::clamp(CVarInt(s_cvars.debug, 0), 0, 3);
    cfg.godRays = std::clamp(CVarFloat(s_cvars.godRays, cfg.godRays), 0.0f, 4.0f);   // coa config.cpp:75
    cfg.shadowedSun = CVarInt(s_cvars.shadowedSun, 1) != 0;
    cfg.canopy = std::clamp(CVarFloat(s_cvars.canopy, cfg.canopy), 0.0f, 1.0f);
    cfg.grade = CVarInt(s_cvars.grade, 0) != 0;
    cfg.gradeStrength = std::clamp(CVarFloat(s_cvars.gradeStrength, cfg.gradeStrength), 0.0f, 1.0f);   // coa config.cpp:52
    cfg.dither = std::clamp(CVarInt(s_cvars.dither, 1), 0, 2);
    cfg.hdrTargets = CVarInt(s_cvars.hdrTargets, 1) != 0;
    // coa config.cpp ranges
    cfg.sunScatter = std::clamp(CVarFloat(s_cvars.sunScatter, cfg.sunScatter), 0.0f, 10.0f);
    cfg.exposure = std::clamp(CVarFloat(s_cvars.exposure, cfg.exposure), 0.0f, 10.0f);
    cfg.classicExposure = std::clamp(CVarFloat(s_cvars.classicExposure, cfg.classicExposure), 0.0f, 10.0f);
    cfg.classicPhase = std::clamp(CVarInt(s_cvars.classicPhase, cfg.classicPhase), 0, 1);
    cfg.colorSpace = std::clamp(CVarInt(s_cvars.colorSpace, cfg.colorSpace), 0, 1);
    cfg.localLightIntensity = std::clamp(CVarFloat(s_cvars.localLightIntensity, cfg.localLightIntensity), 0.0f, 8.0f);
    cfg.localLightPhase = std::clamp(CVarFloat(s_cvars.localLightPhase, cfg.localLightPhase), -0.9f, 0.9f);
    cfg.interiorAware = CVarInt(s_cvars.interior, 1) != 0;
    cfg.interiorDensity = std::clamp(CVarFloat(s_cvars.interiorDensity, cfg.interiorDensity), 0.0f, 1.0f);
    cfg.interiorFarFog = std::clamp(CVarFloat(s_cvars.interiorFarFog, cfg.interiorFarFog), 0.0f, 1.0f);
    cfg.noiseScale = std::clamp(CVarFloat(s_cvars.noiseScale, cfg.noiseScale), 0.001f, 1.0f);
    cfg.noiseWindSpeed = std::clamp(CVarFloat(s_cvars.noiseWind, cfg.noiseWindSpeed), 0.0f, 10.0f);
    cfg.classicNoise = CVarInt(s_cvars.classicNoise, 1) != 0;
    cfg.glowCompensation = CVarInt(s_cvars.glowCompensation, 1) != 0;
    cfg.underwater = CVarInt(s_cvars.underwater, 0) != 0;
    cfg.stockFog = CVarInt(s_cvars.stockFog, 1) != 0;
    cfg.foreverGlow = CVarInt(s_cvars.foreverGlow, 0) != 0;
    cfg.sunMarker = CVarInt(s_cvars.sunMarker, 0) != 0;
    cfg.liquidDepth = CVarInt(s_cvars.liquidDepth, 1) != 0;
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

// coa engine_lights.cpp CaptureInteriorUnsafe: m_interiorFogBlend is also set inside exterior WMO fog volumes
// (DayNight::UpdateFog), so it counts only when one of the camera's loaded groups lacks the exterior flags 0x48.
bool CameraInInteriorGroup() {
    CMapObjDef* def = CWorldScene::s_viewerMapObjDef;
    CMapObj* owner = def ? def->m_owner : nullptr;
    if (!owner || !owner->m_isGroupLoaded) {
        return false;
    }
    const TSGrowableArray<uint32_t>& ids = CWorldScene::s_viewerMapObjGroups;
    for (uint32_t i = 0; i < ids.m_count; ++i) {
        if (ids.m_data[i] >= static_cast<uint32_t>(owner->m_groupInfoCount)) {
            continue;
        }
        const CMapObjGroup* group = owner->GetGroup(static_cast<int32_t>(ids.m_data[i]), 0);
        if (group && !(group->m_flags & 0x48)) {
            return true;
        }
    }
    return false;
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
    in.interiorBlend = CameraInInteriorGroup() ? Clamp01(info.m_interiorFogBlend) : 0.0f;
    in.stormBlend = Clamp01(info.m_stormBlend);
    const int32_t slot = static_cast<int32_t>(info.m_overrideLightParamsId);
    in.screenEffectSlot = slot >= 0 && slot < kLightParamsSlots ? slot : -1;
    in.mapId = World::QueryMapId();
}

// coa engine.cpp: the visible sun or moon sprite, relative to the sky centre (DNInfo::m_cameraPos).
void ReadLightDirection(FrameInputs& in) {
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    // gfxSunArc: the body the world's light and the shadows come from (DayNight::g_lightBody); else coa's hours.
    in.lightIsMoon = DayNight::SunArc() ? DayNight::g_lightBody == 1 : !(info.m_dayProgression >= kSunDayStart && info.m_dayProgression <= kSunDayEnd);
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
    // ponytail: direct-mapped, a colliding model just refits; go set-associative if collisions show in a profile.
    uint32_t bits[3];
    std::memcpy(bits, modelPos, sizeof(bits));
    const uint32_t hash = (bits[0] * 73856093u) ^ (bits[1] * 19349663u) ^ (bits[2] * 83492791u);
    ModelFit& slot = s_modelFits[(hash ^ (hash >> 16)) % kModelFitSlots];
    if (slot.stamp != s_fitStamp || std::memcmp(slot.pos, modelPos, sizeof(slot.pos)) != 0) {
        slot.fit = FitStockFogAt(s_params, s_in, s_cfg, modelPos);
        std::memcpy(slot.pos, modelPos, sizeof(slot.pos));
        slot.stamp = s_fitStamp;
    }
    return slot.fit;
}

// coa forever_look.cpp PrepareGrading + ForeverLookWeight. Only while vfog draws this frame, which already rules
// out the view under water (coa kCameraUnderWater); the ghost view is skipped as coa kGhostEffect.
void PrepareGrade() {
    s_grade.pending = false;
    if (!s_cfg.grade || !(s_cfg.gradeStrength > 0.0f) || !s_active || !s_opaqueValid || s_in.inLiquid ||
        (CGWorldFrame::ffxEffectDeath && FFX::s_effect == CGWorldFrame::ffxEffectDeath)) {
        return;
    }
    AuthoredFog look;
    ResolveFogData(s_in.mapId, s_in.camPos, s_in.dayFraction, SelectionFor(s_in), look);
    if (!std::isfinite(look.coverage) || !(look.coverage > kMinimumClassicCoverage) || !look.hasGradingCurve) {
        return;     // coa kNoClassicLight / kNoGradedClassicLight
    }
    const float weight = std::min((look.coverage - kMinimumClassicCoverage) / (1.0f - kMinimumClassicCoverage), 1.0f);
    s_grade.strength = s_cfg.gradeStrength * weight;
    std::memcpy(s_grade.curve, look.gradingCurve, sizeof(s_grade.curve));
    s_grade.world = LiveWorldViewport();
    s_grade.pending = s_grade.strength > 0.0f && Finite(s_grade.curve, kGradingCurveEntries);
}

// coa forever_look.cpp FeedGlow + ForeverGlowByte: the glow amount the FFX glow gets (UpdateScreenEffectParams reads it
// through GlowAmount) becomes lerp(client, Forever glow, saturate((coverage - 0.5) / 0.5)); never the ghost view's
// (that is not the glow effect) and not under water. The fog's glow compensation uses the amount delivered (coa
// UseDeliveredGlow). The camera position is last frame's: OnOpaqueDone captures this frame's later.
void FeedForeverGlow() {
    if (!s_cfg.foreverGlow || s_in.inLiquid || !GlowScreenEffectRuns()) {
        return;
    }
    AuthoredFog look;
    ResolveFogData(s_in.mapId, s_in.camPos, s_in.dayFraction, SelectionFor(s_in), look);
    if (!look.hasGlow || !std::isfinite(look.glow) || !std::isfinite(look.coverage) || !(look.coverage > kMinimumClassicCoverage)) {
        return;
    }
    const float weight = std::min((look.coverage - kMinimumClassicCoverage) / (1.0f - kMinimumClassicCoverage), 1.0f);
    const float client = Clamp01(DayNight::g_dnInfo.m_bands.m_glow);
    s_glowAmount = Clamp01(client + (look.glow - client) * weight);
    s_glowFed = true;
    s_in.clientGlowAmount = s_glowAmount;
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
    s_cvars.transparent = CVar::Register("vfogTransparent", "Volumetric fog: 1 = composite before transparents and fit the fog onto them, 0 = one composite after the whole world", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.fadeEarly = CVar::Register("vfogFadeEarly", "Volumetric fog: 1 = M2s fading at their draw distance are drawn before the early composite, so it fogs them at their own depth (default), 0 = stock pass 1 with the fitted fog", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.fadeStencil = CVar::Register("vfogFadeStencil", "Volumetric fog: 1 = fading M2s drawn early cross-fade by their alpha between the fog behind them and their own (wxl-vfog 0.6.3, needs a stencil), 0 = they take their own fog at any alpha", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.msaaSplit = CVar::Register("vfogMsaaSplit", "Volumetric fog with MSAA: 1 = the near and far samples of an edge pixel each fogged at their own depth (coa sample split), 0 = the fog of one sample", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.startOffset = CVar::Register("vfogStartOffset", "Volumetric fog start distance (yards)", 1, "10", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.maxDistance = CVar::Register("vfogMaxDistance", "Volumetric fog maximum march distance (yards)", 1, "5000", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.noise = CVar::Register("vfogNoise", "Volumetric fog density noise amount", 1, "0.15", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.debug = CVar::Register("vfogDebug", "Volumetric fog debug view (0..3)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.grade = CVar::Register("vfogGrade", "Volumetric fog colour grading: the modern client's colour curve for the lights around the camera, over the 3D view after the glow (not names or the UI): 1 = on, 0 = off (default, as coa)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.gradeStrength = CVar::Register("vfogGradeStrength", "Volumetric fog colour grading strength while vfogGrade is on, 0..1 (coa ColorGrading)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.dither = CVar::Register("vfogDither", "Volumetric fog dither against banding on the 8-bit outputs (composite, grade, water, and the fog targets when 8 bit): 0 = off, 1 = +-1/255 triangular (default), 2 = twice that", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.hdrTargets = CVar::Register("vfogHdrTargets", "Volumetric fog: 1 = half-float fog and god ray targets where the device has them (default), 0 = 8 bit (bands)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.godRays = CVar::Register("vfogGodRays", "Volumetric fog god rays from the bright sky around the sun: 0 = off, 1 = on (default), up to 4 = strength", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.shadowedSun = CVar::Register("vfogShadowedSun", "Volumetric fog sun shafts through the scene: the fog's sunlight is shadowed by trees, buildings and terrain casters (needs shadows above low): 1 = on (default), 0 = off", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvars.canopy = CVar::Register("vfogCanopy", "Volumetric fog darkening of the haze glow in sun shadow (under trees, beside buildings) while vfogShadowedSun is on, 0..1 (default 0.5)", 1, "0.5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    TerrainSunShadow::RegisterCVars();

    auto reg = [](const char* name, const char* help, const char* value) {
        return CVar::Register(name, help, 1, value, nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    };
    // coa CoAVolFog.ini keys that were constants until vfog13 (defaults as coa, the user's look where VFOG_PORT 8 says).
    s_cvars.sunScatter = reg("vfogSunScatter", "Volumetric fog in-scattered sun or moon light (coa SunScatter), 0..10", "1");
    s_cvars.exposure = reg("vfogExposure", "Volumetric fog brightness of the derived layers (coa Exposure), 0..10", "1");
    s_cvars.classicExposure = reg("vfogClassicExposure", "Volumetric fog brightness of the authored layers (coa ClassicExposure, 1 = as authored), 0..10", "0.5");
    s_cvars.classicPhase = reg("vfogClassicPhase", "Volumetric fog authored sun scattering: 0 = phase peaks at 1, 1 = energy-normalised (coa ClassicPhase)", "0");
    s_cvars.colorSpace = reg("vfogColorSpace", "Volumetric fog: 1 = scatter and blend in linear light (default), 0 = gamma (coa ColorSpace)", "1");
    s_cvars.localLightIntensity = reg("vfogLocalLightIntensity", "Volumetric fog point light intensity, 0..8 (coa LocalLightIntensity)", "2.5");
    s_cvars.localLightPhase = reg("vfogLocalLightPhase", "Volumetric fog point light phase g, -0.9..0.9 (coa LocalLightPhase)", "0.3");
    s_cvars.interior = reg("vfogInterior", "Volumetric fog fades outdoor layers and sunlight indoors (coa InteriorAware)", "1");
    s_cvars.interiorDensity = reg("vfogInteriorDensity", "Volumetric fog outdoor layer density kept indoors, 0..1 (coa InteriorDensity)", "0.15");
    s_cvars.interiorFarFog = reg("vfogInteriorFarFog", "Volumetric fog distance fog density kept indoors, 0..1; lower clears dungeon haze (coa InteriorFarFog)", "1");
    s_cvars.noiseScale = reg("vfogNoiseScale", "Volumetric fog density variation scale, 0.001..1, larger is smaller patches (coa NoiseScale)", "0.025");
    s_cvars.noiseWind = reg("vfogNoiseWind", "Volumetric fog density variation drift in yd/s, 0..10 (coa NoiseWindSpeed)", "0.5");
    s_cvars.classicNoise = reg("vfogClassicNoise", "Volumetric fog drifting banks from the authored noise (coa ClassicNoise)", "1");
    s_cvars.glowCompensation = reg("vfogGlowCompensation", "Volumetric fog pre-compensated for the client's glow (coa GlowCompensation)", "1");
    s_cvars.underwater = reg("vfogUnderwater", "Volumetric fog also with the camera under water, over the stock fog (coa Underwater)", "0");
    s_cvars.stockFog = reg("vfogStockFog", "Volumetric fog: 1 = replaces the stock fog (default), 0 = keeps it and adds no distance fog (coa StockFog)", "1");
    s_cvars.foreverGlow = reg("vfogForeverGlow", "Volumetric fog: the modern client's glow amount where its lights cover the camera (coa ForeverGlow)", "0");
    s_cvars.sunMarker = reg("vfogSunMarker", "Volumetric fog debug: red dot where the light direction projects (coa SunMarker)", "0");
    s_cvars.liquidDepth = reg("vfogLiquidDepth", "Volumetric fog: water writes depth so it is fogged at its own distance; always on while modern water draws (coa LiquidDepth)", "1");

    RegisterWaterCVars();
    CataWater::RegisterCVars();

    g_vfogM2BatchFog = &BatchFog;
    g_vfogM2FogParticles = &FogParticles;
}

bool Active() {
    return s_active;
}

bool s_decidedInLiquid = false;    // the liquid status this frame's DecideFrame ran on
void DecideFrame(bool inLiquid);

void OnWorldUpdate() {
    // DayNight::UpdateFog rebuilt DNInfo fog this frame; a push left over from a frame that never rendered the world
    // (OnWorldDone not reached) must not be snapshotted as the stock fog.
    if (s_pushed) {
        RestoreStockFog();
    }
    s_armed = false;
    s_opaqueValid = false;
    s_glareDrawn = false;
    s_cfg = ReadConfig();

    if (s_cfg.enable) {
        EnsureFogData();
    }

    s_glowFed = false;
    DecideFrame(World::SceneCamLiquidStatus(nullptr) != 0);
}

// The per-frame activation from the camera's liquid status (OnWorldUpdate, and OnViewerLiquid when it changed).
void DecideFrame(bool inLiquid) {
    s_decidedInLiquid = inLiquid;
    s_active = s_cfg.enable && g_theGxDevicePtr->MasterEnable(GxMasterEnable_Fog) && (!inLiquid || s_cfg.underwater) && RendererReady();

    if (!s_active) {
        if (s_wasActive) {
            InvalidateHistory();
        }
        s_wasActive = false;
        return;
    }

    s_wasActive = true;
    ReadDayNightInputs(s_in);
    s_in.inLiquid = inLiquid;
    if (inLiquid) {
        s_in.clientGlowAmount = 0.0f;   // coa engine.cpp:409
    }
    FeedForeverGlow();
    // coa FogDrawsInPlaceOfStockFog / EarlyCompositeUnready: the stock fog is replaced only above water with StockFog=1;
    // under water, with StockFog=0, a debug view or the sun marker the fog is drawn once after the world.
    s_late = !s_cfg.transparentFog || !s_cfg.stockFog || inLiquid || s_cfg.debugView != 0 || s_cfg.sunMarker;
    if (s_cfg.stockFog && !inLiquid) {
        PushStockFogOutOfRange();
    }
}

void OnViewerLiquid() {
    // OnWorldUpdate decided on last frame's liquid status (UpdateViewerLiquid writes it later, in CWorldScene::Render,
    // which calls this right after it and before anything is drawn): the camera just went under, so the frame takes
    // the underwater branch, which has no composite. Give it the stock fog.
    const bool inLiquid = World::SceneCamLiquidStatus(nullptr) != 0;
    if (inLiquid == s_decidedInLiquid) {
        return;
    }
    if (!inLiquid) {
        // Just surfaced: decide again for above water, so this frame is not left on the stock fog.
        if (s_pushed) {
            RestoreStockFog();
        }
        InvalidateHistory();
        DecideFrame(false);
        return;
    }
    s_decidedInLiquid = true;
    if (!s_active) {
        return;
    }
    RestoreStockFog();
    InvalidateHistory();
    if (s_cfg.underwater) {   // coa Underwater=1: the fog after the world, over the stock fog
        s_in.inLiquid = true;
        s_in.clientGlowAmount = 0.0f;
        s_glowFed = false;
        s_late = true;
        return;
    }
    s_active = false;
    s_wasActive = false;
}

void OnOpaqueDone() {
    // shadowMask needs the same frame inputs with the fog off (its DNInfo half is read here then: no push-out to undo).
    s_maskPending = false;
    const bool mask = TerrainSunShadow::MaskWanted() && RendererReady();
    if (!s_active && !mask) {
        return;
    }
    if (!s_active) {
        ReadDayNightInputs(s_in);
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
    if (s_active) {
        CaptureLocalLights(in, s_cfg);
    }

    s_opaqueValid = in.viewport[1] > in.viewport[0] && in.viewport[3] > in.viewport[2] && IsPerspective(in.glProjection) &&
                    Finite(in.cameraRelativeView, 16) && Finite(in.camPos, 3) && std::isfinite(in.fogStart) && std::isfinite(in.fogEnd);
    s_maskPending = mask && s_opaqueValid;
}

void OnSolidDone() {
    if (s_maskPending) {
        s_maskPending = false;
        RenderShadowMask(s_in, TerrainSunShadow::MaskSoftYards(), !s_pushed);
    }
}

namespace {

// wxl-vfog 0.6.0 DrawFadingModelsBeforeFog (RE /root/RE/render-path/doodad-fade-frame.md 5b, doodad-fade-route.md):
// a model fading at its draw distance is routed by the client from pass 0 to the blended pass 1, which draws after
// the early composite, so it missed the fog every solid model gets. Its depth-writing batches (type 0, base
// material blend <= 1, material without the no-depth-write flag 0x10) are taken out of the pass-1 list in order
// and drawn here as CM2Scene::Draw does, so the composite fogs them at their own depth.
// vfogFadeStencil (wxl-vfog 0.6.3): the depth behind them is kept first and each batch writes its alpha into the stencil,
// so RenderFog fogs the frame from behind them and lays their own fog over them by that alpha. Without it (no stencil,
// CVar 0) the background seen through a fading model takes the model's fog (doodad-fade-frame.md 5d).
bool FadingOpaqueElement(const M2Element& e) {
    if (e.m_type != 0 || !e.m_model || !e.m_batch || !(e.m_model->m_alpha < 0.99999f)) {
        return false;
    }
    const CM2Shared* shared = e.m_model->m_shared;
    const M2Data* data = shared ? shared->m_data : nullptr;
    const M2Batch* b = e.m_batch;
    if (!data || b->m_materialIndex < b->m_materialLayer || b->m_materialIndex >= data->m_materials.m_count) {
        return false;
    }
    return data->m_materials[b->m_materialIndex - b->m_materialLayer].m_blendMode <= 1 &&
           !(data->m_materials[b->m_materialIndex].m_flags & 0x10);
}

void DrawFadingEarly() {
    CM2Scene* scene = World::s_m2Scene;
    if (!s_cfg.fadeEarly || !s_opaqueValid || !scene || !(scene->m_passMask & (1 << M2PASS_1))) {
        return;
    }
    TSGrowableArray<uint32_t>& list = scene->m_passElements[M2PASS_1];
    static std::vector<uint32_t> early;
    early.clear();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < list.m_count; ++i) {
        const uint32_t index = list.m_data[i];
        if (FadingOpaqueElement(scene->m_elements.m_data[index])) {
            early.push_back(index);
        } else {
            list.m_data[kept++] = index;
        }
    }
    if (early.empty()) {
        return;
    }
    list.m_count = kept;   // compacted in place; the list is rebuilt next frame
    CM2SceneRender render(scene);
    M2Element* elements = scene->m_elements.m_data;
    if (!s_cfg.fadeStencil || !KeepDepthBehindFading()) {
        render.Draw(M2PASS_1, elements, early.data(), static_cast<uint32_t>(early.size()));
        return;
    }
    // wxl-vfog MarkLateDepth: stencil = alpha * 255 where each batch passes the depth test; runs of one alpha per draw.
    GxSceneClear(GxBufferBit_Stencil, CImVector { 0, 0, 0, 0 });
    g_theGxDevicePtr->RsPush();
    SetStencil(GxStencilCmp_Always, 0, 0xFF, kFadeBits, GxStencilOp_Replace);   // top three bits only: the split owns the rest
    const auto ref = [&](uint32_t index) { return static_cast<int32_t>(std::clamp(elements[index].m_alpha, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (size_t start = 0, end; start < early.size(); start = end) {
        const int32_t r = ref(early[start]);
        for (end = start + 1; end < early.size() && ref(early[end]) == r; ++end) {
        }
        GxRsSet(GxRs_StencilRef, r);
        render.Draw(M2PASS_1, elements, early.data() + start, static_cast<uint32_t>(end - start));
    }
    g_theGxDevicePtr->RsPop();
}

}  // namespace

bool Composite() {
    if (!s_opaqueValid) {
        return false;
    }
    s_params = BuildFogParams(s_in, s_cfg);
    return RenderFog(s_params, s_in, s_cfg, s_frame++, FrameTime::s_curTimeMs * 0.001);
}

void OnLiquidDone() {
    // vfogTransparent 0 is coa's TransparentFog=0: one composite after the whole world (OnWorldDone), so
    // transparents take the fog of the scene behind them.
    if (!s_active || s_late) {
        return;
    }

    // coa DrawGlareBeforeTheFog: the sun and moon glare goes under the fog with the scene; the stock call is skipped.
    DrawFadingEarly();
    if (s_opaqueValid) {
        DayNight::RenderGlares();
        s_glareDrawn = true;
    }
    if (!Composite()) {
        // Nothing composited: the late passes get the stock fog back, not the fitted one.
        RestoreStockFog();
        return;
    }

    s_fit = FitStockFog(s_params, s_in, s_cfg);
    s_fitStamp++;
    s_armed = true;
}

bool GlareDrawn() {
    return s_glareDrawn;
}

void OnWorldDone() {
    if (s_active && s_late) {
        Composite();
    }
    // coa RenderGodRaysOverTheWorld: after the whole world, before FFX glow. Only a RenderFog this frame arms it.
    RenderGodRays();
    PrepareGrade();
    s_armed = false;
    s_glareDrawn = false;
    RestoreStockFog();
}

void OnWorldRendered() {
    if (!s_grade.pending) {
        return;
    }
    s_grade.pending = false;
    RenderGrade(s_grade.curve, s_grade.strength, s_cfg.dither, s_frame, s_grade.world);
}

bool WaterFrame(const FrameInputs*& in, const Config*& cfg) {
    in = &s_in;
    cfg = &s_cfg;
    return s_active && s_opaqueValid && !s_in.inLiquid;   // coa ArmWaterPass: no modern water under water
}

bool StockFogReplaced() {
    return s_pushed;
}

bool LiquidDepthWrite() {
    // coa LiquidDepth; modern water needs the depth whatever it says (WaterArmed: this frame's liquid pass).
    return s_active && (s_cfg.liquidDepth || WaterArmed());
}

float GlowAmount(float client) {
    return s_glowFed ? s_glowAmount : client;
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
