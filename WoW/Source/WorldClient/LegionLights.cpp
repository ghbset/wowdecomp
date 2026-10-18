// LEGION lights: not in 12340. Legion 7.3.5 lightMode / maxLightCount / maxLightDist (survey 2.8) as a v1 screen-space
// lamp pass: the CM2Scene light grid's visible point lights (the same list vfog's fog scattering walks, without its
// 8-light cap) added onto the frame from the scene depth, colour += colour * light (GxBlend_ModAdd). Shader
// LegionLights.hlsl; falloff after northlight (MIT). docs/LEGION_PORT.md "Legion lights".
// ponytail: screen space after the receivers, not a light pre-pass read by them; see the docs for the next phase.
#include "WoW/Source/WorldClient/LegionLights.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/VolumetricFog.h"
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include "WoW/Source/WorldClient/World.h"

namespace LegionLights {

namespace {

using namespace VolumetricFog;

constexpr int kLightsPerPass = 16;          // LegionLights.hlsl kLights
constexpr int kMaxLights = 128;             // Legion maxLightCount's range
constexpr uint32_t kControlRegister = 10;   // c10 control, c11.. three registers a light
constexpr float kMaxRadius = 40.0f;         // yd
constexpr float kCutoff = 1.0f / 32.0f;     // the radius: where the stock attenuation leaves 1/32 of the colour
constexpr float kMaxColour = 4.0f;
constexpr float kMinLightLevel = 0.2f;

CVar* s_mode;
CVar* s_count;
CVar* s_dist;
CVar* s_intensity;
CGxShader* s_shader;    // ponytail: not destroyed at device shutdown (one shader, the device goes with the process)

float Var(CVar* v, float fallback, float lo, float hi) {
    return v && std::isfinite(v->m_floatValue) ? std::clamp(v->m_floatValue, lo, hi) : fallback;
}

float Luma(const CImVector& c) {
    return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f;
}

struct Light {
    float view[3];
    float radius;
    float colour[3];
    float att[3];
    float distance;
};

// The stock M2 point light (CM2Light::SetupLighting: attenuation 0 / 0.7 / 0.03 for every M2 light) falls to kCutoff
// of its peak at this radius.
float Radius(float peak, const float att[3]) {
    const double target = peak / kCutoff - att[0];
    if (!(peak > 0.0f) || target <= 0.0) {
        return 0.0f;
    }
    double r = kMaxRadius;
    if (att[2] > 0.0f) {
        r = 2.0 * target / (att[1] + std::sqrt(static_cast<double>(att[1]) * att[1] + 4.0 * att[2] * target));
    } else if (att[1] > 0.0f) {
        r = target / att[1];
    }
    return static_cast<float>(std::min(r, static_cast<double>(kMaxRadius)));
}

void Draw(const std::vector<Light>& lights, float maxDist, float intensity) {
    CGxDevice* device = g_theGxDevicePtr;
    if (!Shader(s_shader, "LegionLights")) {
        return;
    }
    const WorldViewport vp = LiveWorldViewport();
    CGxDevice::SetDepthUser(CGxDevice::DepthUser_Lights, true);
    CGxTex* depth = device->DeviceCopySceneDepth();
    if (!depth || vp.w < 1.0f || vp.h < 1.0f || vp.x < 0.0f || vp.y < 0.0f || vp.x + vp.w > depth->m_width || vp.y + vp.h > depth->m_height) {
        return;
    }

    // vf_common c0-c3 as RenderShadowMask sets them; c3.z (the distance clamp) = lightMaxDist.
    const float* proj = reinterpret_cast<const float*>(&device->m_projection);
    const float extent = vp.maxZ - vp.minZ;
    const float common[4][4] = {
        { vp.x, vp.y, vp.w, vp.h },
        { 1.0f, 0.0f, 1.0f / depth->m_width, 1.0f / depth->m_height },
        { proj[0], proj[5], proj[8], proj[9] },
        { vp.minZ + extent * (1.0f + proj[10]) * 0.5f, extent * proj[14] * 0.5f, maxDist, vp.maxZ },
    };
    // The frame's colour is albedo x the stock light level: divide by it so a lamp adds albedo x lamp, as Legion's
    // light buffer does (night: lamps count for more; noon: about their own colour).
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    const float level = std::max(Luma(info.m_bands.m_ambient) + 0.5f * Luma(info.m_bands.m_diffuse), kMinLightLevel);
    const float fadeSpan = std::max(maxDist * 0.25f, 1.0f);
    const float control[4] = { intensity / level, 1.0f / fadeSpan, maxDist, 0.0f };

    static SavedPassState s_saved;
    BeginPasses(s_saved);
    GxRsSet(GxRs_BlendingMode, GxBlend_ModAdd);     // dst * src + dst
    GxRsSet(GxRs_ColorWrite, 7);
    SetPs(0, &common[0][0], 4);
    SetPs(kControlRegister, control, 1);
    Bind(0, depth);
    device->RsSet(GxRs_PixelShader, s_shader);
    device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
    // ponytail: every batch over the whole viewport; scissor each to its lights' screen rect if the cost shows.
    for (size_t first = 0; first < lights.size(); first += kLightsPerPass) {
        float regs[kLightsPerPass * 3][4] = {};
        for (size_t i = first; i < std::min(lights.size(), first + kLightsPerPass); ++i) {
            const Light& l = lights[i];
            float* r = regs[(i - first) * 3];
            std::memcpy(r, l.view, sizeof(l.view));
            r[3] = 1.0f / (l.radius * l.radius);
            std::memcpy(r + 4, l.colour, sizeof(l.colour));
            std::memcpy(r + 8, l.att, sizeof(l.att));
        }
        SetPs(kControlRegister + 1, &regs[0][0], kLightsPerPass * 3);
        DrawQuad(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), vp.minX, vp.maxX, vp.minY, vp.maxY);
    }
    device->DeviceSceneDepthBound(true);
    EndPasses(s_saved, vp);
}

}  // namespace

void RegisterCVars() {
    const auto reg = [](const char* name, const char* help, const char* value) {
        return CVar::Register(name, help, 1, value, nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    };
    s_mode = reg("lightMode", "Lamps and torches light the world around them: 0 = off, 1 = ground and buildings, 2 = also creatures, characters and grass", "0");
    s_count = reg("maxLightCount", "Most lamps that light the world at once, nearest first, 1..128", "32");
    s_dist = reg("maxLightDist", "Distance in yards out to which lamps light the world (fades out over the last quarter), 20..2048", "200");
    s_intensity = reg("lightIntensity", "Brightness of the lamp light on the world, 0..4", "1");
}

void OnWorld(bool solidDone) {
    const int mode = s_mode ? s_mode->m_intValue : 0;
    if (!solidDone) {
        CGxDevice::SetDepthUser(CGxDevice::DepthUser_Lights, false);    // set again by Draw
    }
    CM2Scene* scene = World::s_m2Scene;
    CGxDevice* device = g_theGxDevicePtr;
    if (mode < 1 || mode > 2 || (mode == 2) != solidDone || !scene || !scene->m_lightGrid || !device) {
        return;
    }
    const float maxDist = Var(s_dist, 200.0f, 20.0f, 2048.0f);
    const float intensity = Var(s_intensity, 1.0f, 0.0f, 4.0f);
    const int maxCount = static_cast<int>(Var(s_count, 32.0f, 1.0f, static_cast<float>(kMaxLights)));
    if (intensity <= 0.0f) {
        return;
    }

    // The Gx view top is camera relative (CWorldScene::Update); rows are view axes (row vectors, as vf_common).
    const CGxMatrixStack& viewStack = device->m_xforms[GxXform_View];
    const float* m = reinterpret_cast<const float*>(&viewStack.m_mtx[viewStack.m_level]);
    const C3Vector& cam = CWorldScene::s_activeWorldView;

    static std::vector<Light> s_lights;
    s_lights.clear();
    for (int cell = 0; cell < 64 * 64; ++cell) {
        for (const CM2Light* native = scene->m_lightGrid[cell]; native; native = native->m_lightNext) {
            if (native->m_type != M2LIGHT_POINT || !native->m_visible) {
                continue;
            }
            Light l;
            const float rel[3] = { native->m_pos.x - cam.x, native->m_pos.y - cam.y, native->m_pos.z - cam.z };
            const float c[3] = { native->m_dirColor.x, native->m_dirColor.y, native->m_dirColor.z };
            l.att[0] = native->m_constantAttenuation;
            l.att[1] = native->m_linearAttenuation;
            l.att[2] = native->m_quadraticAttenuation;
            float peak = 0.0f;
            for (int i = 0; i < 3; ++i) {
                l.view[i] = rel[0] * m[i] + rel[1] * m[4 + i] + rel[2] * m[8 + i];
                l.colour[i] = std::isfinite(c[i]) ? std::clamp(c[i], 0.0f, kMaxColour) : 0.0f;
                peak = std::max(peak, l.colour[i]);
            }
            l.radius = Radius(peak, l.att);
            l.distance = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
            if (l.radius < 0.5f || !std::isfinite(l.distance) || l.distance > maxDist + l.radius || l.view[2] < -l.radius) {
                continue;   // too weak, out of reach, or behind the camera
            }
            s_lights.push_back(l);
        }
    }
    if (s_lights.empty()) {
        return;
    }
    if (static_cast<int>(s_lights.size()) > maxCount) {
        std::nth_element(s_lights.begin(), s_lights.begin() + maxCount, s_lights.end(),
                         [](const Light& a, const Light& b) { return a.distance < b.distance; });
        s_lights.resize(maxCount);
    }
    Draw(s_lights, maxDist, intensity);
}

}  // namespace LegionLights
