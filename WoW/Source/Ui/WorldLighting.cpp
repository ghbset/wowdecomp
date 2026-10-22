#include "WoW/Source/Ui/WorldLighting.h"
#include <cmath>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/GxLighting.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/World.h"

// CUSTOM (Badgermilk0 port of AwesomeLighting.cpp, the post-pass half): the CVars and what the passes need of the
// zone. Dynamic / per-pixel lights, specular and sky ambient were left out: they rewrite the translated world shaders,
// which here come precompiled from the shader-xlate pack, not from a runtime translator.

namespace {

struct LightingCVars {
    CVar* hdr;
    CVar* exposure;
    CVar* tonemap;
    CVar* bloom;
    CVar* bloomThreshold;
    CVar* ssao;
    CVar* ssaoRadius;
    CVar* ssaoDistance;
    CVar* fogScattering;
    CVar* debug;
};

LightingCVars s_cv;
bool s_inWorld = false;
GxLightingParams s_params;

CVar* Reg(const char* name, const char* help, const char* value) {
    return CVar::Register(name, help, 0x10, value, nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

float Positive(CVar* cv) {
    return cv && cv->m_floatValue > 0.0f ? cv->m_floatValue : 0.0f;
}

C3Vector ColorOf(const CImVector& c) {
    return C3Vector(c.r * (1.0f / 255.0f), c.g * (1.0f / 255.0f), c.b * (1.0f / 255.0f));
}

C3Vector Lerp(const C3Vector& a, const C3Vector& b, float t) {
    return C3Vector(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

uint32_t Features() {
    if (!s_cv.hdr) {
        return 0;
    }

    uint32_t features = 0;

    // bloom rides the hdr pass; without hdr the tonemap is the stock clamp
    if (s_cv.hdr->m_intValue || Positive(s_cv.bloom) > 0.0f) {
        features |= GxLighting_Hdr;
    }

    if (Positive(s_cv.bloom) > 0.0f) {
        features |= GxLighting_Bloom;
    }

    if (Positive(s_cv.ssao) > 0.0f) {
        features |= GxLighting_Ssao;
    }

    if (Positive(s_cv.fogScattering) > 0.0f) {
        features |= GxLighting_Scattering;
    }

    return features;
}

void BuildParams(GxLightingParams& p, uint32_t features, const C3Vector& camera) {
    p.m_features = features;
    p.m_exposure = s_cv.exposure->m_floatValue > 0.0f ? s_cv.exposure->m_floatValue : 1.0f;
    p.m_tonemap = !s_cv.hdr->m_intValue || s_cv.tonemap->m_intValue < 0 ? 0u : static_cast<uint32_t>(s_cv.tonemap->m_intValue);
    p.m_bloomIntensity = Positive(s_cv.bloom);
    p.m_bloomThreshold = s_cv.bloomThreshold->m_floatValue > 0.01f ? s_cv.bloomThreshold->m_floatValue : 0.01f;
    p.m_ssaoIntensity = Positive(s_cv.ssao);
    p.m_ssaoRadius = s_cv.ssaoRadius->m_floatValue > 0.1f ? s_cv.ssaoRadius->m_floatValue : 0.1f;
    p.m_ssaoDistance = s_cv.ssaoDistance->m_floatValue > 10.0f ? s_cv.ssaoDistance->m_floatValue : 10.0f;
    p.m_scattering = Positive(s_cv.fogScattering);
    p.m_cameraPosition = camera;
    p.m_time = static_cast<float>(OsGetAsyncTimeMs() % 3600000u) * 0.001f;
    p.m_debug = s_cv.debug->m_intValue < 0 ? 0u : static_cast<uint32_t>(s_cv.debug->m_intValue);

    // the zone's fog and sun this moment (Light.dbc through DayNight)
    DayNight::DNInfo* info = DayNight::GetInfo();

    if (!info) {
        return;
    }

    float interior = info->m_interiorFogBlend < 0.0f ? 0.0f : (info->m_interiorFogBlend > 1.0f ? 1.0f : info->m_interiorFogBlend);
    p.m_fogColor = Lerp(ColorOf(info->m_fog.m_color), ColorOf(info->m_fogInterior.m_color), interior);
    p.m_fogStart = info->m_fog.m_start + (info->m_fogInterior.m_start - info->m_fog.m_start) * interior;
    p.m_fogEnd = info->m_fog.m_end + (info->m_fogInterior.m_end - info->m_fog.m_end) * interior;
    p.m_fogDensity = info->m_fog.m_density > 0.0f ? info->m_fog.m_density : 1.0f;
    p.m_sunColor = ColorOf(info->m_bands.m_diffuse);
    p.m_sunDirection = info->m_light0.m_dir;
}

}

void WorldLightingRegisterCVars() {
    s_cv.exposure = Reg("hdrExposure", "HDR exposure (D3D11/Vulkan)", "1");
    s_cv.tonemap = Reg("hdrTonemap", "HDR tonemap: 0 clamp (stock), 1 neutral, 2 ACES", "1");
    s_cv.bloom = Reg("bloom", "Bloom intensity, 0 = off (D3D11/Vulkan)", "0");
    s_cv.bloomThreshold = Reg("bloomThreshold", "Bloom threshold (linear light; the stock shaders never pass 1)", "0.6");
    s_cv.ssao = Reg("lightingSsao", "Post-pass ambient occlusion intensity, 0 = off (D3D11/Vulkan)", "0");
    s_cv.ssaoRadius = Reg("lightingSsaoRadius", "Post-pass SSAO radius in yards", "1.5");
    s_cv.ssaoDistance = Reg("lightingSsaoDistance", "Post-pass SSAO fades out by this distance in yards", "200");
    s_cv.fogScattering = Reg("fogScattering", "Fog tinted toward the sun's colour around the sun, 0 = off", "0");
    s_cv.debug = Reg("lightingDebug", "1 shows the SSAO term alone; 4, 5 the SSAO inputs", "0");
    // last: Features() takes a registered hdr to mean they all are
    s_cv.hdr = Reg("hdr", "HDR world target with a tonemap (D3D11; Vulkan tonemaps the 8-bit scene)", "0");
}

void WorldLightingBeginWorld() {
    s_inWorld = false;
    uint32_t features = g_gxLighting ? Features() : 0;

    // all off: the device is never called, the frame is the stock one
    if (!features) {
        return;
    }

    DayNight::DNInfo* info = DayNight::GetInfo();
    BuildParams(s_params, features, info ? info->m_cameraPos : C3Vector(0.0f, 0.0f, 0.0f));
    g_gxLighting->BeginWorld(s_params);
    s_inWorld = true;
}

void WorldLightingSetupView(const C3Vector& camera) {
    if (!s_inWorld || !g_gxLighting) {
        return;
    }

    // the view matrix is render-relative (CWorldScene::s_activeWorldView): so is the camera
    const C3Vector& origin = CWorldScene::s_activeWorldView;
    BuildParams(s_params, s_params.m_features, camera);
    s_params.m_cameraPosition = camera - origin;
    g_gxLighting->SetupView(s_params);
}

void WorldLightingOpaqueDone() {
    if (s_inWorld && g_gxLighting) {
        g_gxLighting->OpaqueDone();
    }
}

void WorldLightingEndWorld() {
    if (s_inWorld && g_gxLighting) {
        g_gxLighting->EndWorld();
    }

    s_inWorld = false;
}
