#include "WoW/Source/Ui/WorldLighting.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/GxLighting.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2ModelLight.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/World.h"

// CUSTOM (Badgermilk0 port of AwesomeLighting.cpp): the CVars and what the passes need of the zone, and the shader
// half: per-pixel relighting, dynamic point lights (M2 light grid, WMO MOLT lights), specular and sky ambient, which
// the lit world shaders read from g_gxLightingGpu (GxLighting.h). Badgermilk's translator rewrote the world shaders at
// run time; here the rewrite is offline (shader-xlate sm3hlsl.py for D3D11, Tools/VulkanShaders for Vulkan), so the
// lit shaders come with custom.MPQ. D3D9 keeps the stock shaders (g_gxLighting is null there).

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
    // the shader half
    CVar* master;
    CVar* perPixel;
    CVar* dynamic;
    CVar* dynamicIntensity;
    CVar* dynamicM2;
    CVar* dynamicWmo;
    CVar* dynamicDistance;
    CVar* flicker;
    CVar* wrap;
    CVar* specular;
    CVar* specularM2;
    CVar* specularWmo;
    CVar* skyAmbient;
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

    // shaderLighting 0: every light* feature off, whatever its own CVar says
    if (s_cv.master->m_intValue <= 0) {
        return features;
    }

    if (s_cv.perPixel->m_intValue > 0) {
        features |= GxLighting_PerPixel;
    }

    if (s_cv.dynamic->m_intValue > 0) {
        features |= GxLighting_Dynamic;
    }

    if (s_cv.specular->m_intValue > 0) {
        features |= GxLighting_Specular;
    }

    if (Positive(s_cv.skyAmbient) > 0.0f) {
        features |= GxLighting_Hemisphere;
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
    p.m_lightWrap = std::min(Positive(s_cv.wrap), 1.0f);
    p.m_dynamicIntensity = std::min(Positive(s_cv.dynamicIntensity), 8.0f);
    p.m_specularM2 = std::min(Positive(s_cv.specularM2), 4.0f);
    p.m_specularWmo = std::min(Positive(s_cv.specularWmo), 4.0f);
    p.m_hemisphere = std::min(Positive(s_cv.skyAmbient), 1.0f);
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
    // the sky's top and middle; the ground: the fog lit by the sun
    p.m_skyColor = Lerp(ColorOf(info->m_bands.m_skyColors[0]), ColorOf(info->m_bands.m_skyColors[1]), 0.5f);
    p.m_groundColor = Lerp(p.m_fogColor, p.m_sunColor, 0.6f);
    p.m_sunDirection = info->m_light0.m_dir;
}


// ---- the shader half: lights (Badgermilk0 AwesomeLighting.cpp, fires-as-lights left out) ----

std::vector<GxLightingLight> s_lights;

float Decode(float c) {
    return c > 0.0f ? powf(c, 2.2f) : 0.0f;
}

C3Vector Transform(const C3Vector& p, const C44Matrix& m) {
    return C3Vector(p.x * m.a0 + p.y * m.b0 + p.z * m.c0 + m.d0, p.x * m.a1 + p.y * m.b1 + p.z * m.c1 + m.d1, p.x * m.a2 + p.y * m.b2 + p.z * m.c2 + m.d2);
}

float DistanceSquared(const C3Vector& a, const C3Vector& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

// a warm light (a flame) flickers; a cold or white one (magic, a lamp) does not
float FlickerFor(const C3Vector& linear) {
    return linear.x > linear.y * 1.1f && linear.y > linear.z ? std::min(Positive(s_cv.flicker), 1.0f) : 0.0f;
}

// flame: a model's light (a torch), which may flicker; a WMO's lights are its rooms' steady light
void AddLight(const C3Vector& position, const C3Vector& gamma, float scale, float start, float radius, const C3Vector& camera, float reach, bool flame) {
    if (radius <= 0.1f || scale <= 0.0f || DistanceSquared(position, camera) > (reach + radius) * (reach + radius)) {
        return;
    }

    GxLightingLight light;
    light.m_position = position;
    light.m_radius = radius;
    light.m_start = start < radius ? (start > 0.0f ? start : 0.0f) : radius * 0.25f;
    // a model's light can be animated far past 1; a few times white is as bright as a light gets here
    light.m_color = C3Vector(fminf(Decode(gamma.x), 4.0f) * scale, fminf(Decode(gamma.y), 4.0f) * scale, fminf(Decode(gamma.z), 4.0f) * scale);
    // a large light flickers less: a room lit as a whole should not pulse
    light.m_flicker = flame ? FlickerFor(light.m_color) * fminf(1.0f, 8.0f / radius) : 0.0f;
    light.m_specular = 1.0f;

    if (light.m_color.x + light.m_color.y + light.m_color.z >= 0.003f) {
        s_lights.push_back(light);
    }
}

// the point lights of the animated models: CM2Scene's light grid (20-yard cells, 64 x 64, wrapping)
void GatherM2Lights(CM2Scene* scene, const C3Vector& camera, float reach) {
    float scale = Positive(s_cv.dynamicM2);

    if (!scene || !scene->m_lightGrid || scale <= 0.0f) {
        return;
    }

    const float cellScale = 0.05f;
    int32_t cells = std::min(static_cast<int32_t>(ceilf(reach * cellScale)) + 1, 30);
    int32_t cx = static_cast<int32_t>(floorf(camera.x * cellScale));
    int32_t cy = static_cast<int32_t>(floorf(camera.y * cellScale));

    for (int32_t dy = -cells; dy <= cells; dy++) {
        for (int32_t dx = -cells; dx <= cells; dx++) {
            for (CM2Light* light = scene->m_lightGrid[((cy + dy) & 63) * 64 + ((cx + dx) & 63)]; light; light = light->m_lightNext) {
                // CM2Scene::SelectLights' test: a light its model did not animate this frame is gone
                if (!light->m_visible || light->m_type != M2LIGHT_POINT || (light->m_scene && light->m_stamp != scene->m_frameStamp)) {
                    continue;
                }

                // every light in the grid is a model's (M2ModelLight), so its attenuation tracks (M2Model AnimateMT) are beside it
                const M2ModelLight* owner = reinterpret_cast<const M2ModelLight*>(reinterpret_cast<const char*>(light) - offsetof(M2ModelLight, m_light));
                float end = owner->m_attenuationEndTrack.m_currentValue;
                float start = owner->m_attenuationStartTrack.m_currentValue;

                if (!(end > 0.25f && end < 80.0f)) {
                    end = 8.0f;
                    start = 1.0f;
                }

                AddLight(light->m_pos, light->m_dirColor, scale, start, end, camera, reach, true);
            }
        }
    }
}

// the MOLT lights of the placed WMOs, which the stock client only baked into vertex colours
void GatherWmoLights(const C3Vector& camera, float reach) {
    float scale = Positive(s_cv.dynamicWmo);

    if (scale <= 0.0f) {
        return;
    }

    for (CMapObjDef* def = CMap::mapObjDefHash.m_fulllist.Head(); def; def = CMap::mapObjDefHash.Next(def)) {
        CMapObj* owner = def->m_owner;

        if ((def->m_flags & 0x20) || !owner || !owner->m_lightList || owner->m_ligtsCount <= 0) {
            continue;
        }

        // the placement's bounds within reach of the camera
        const CAaBox& box = def->m_bbox;
        C3Vector nearest(std::clamp(camera.x, box.b.x, box.t.x), std::clamp(camera.y, box.b.y, box.t.y), std::clamp(camera.z, box.b.z, box.t.z));

        if (DistanceSquared(nearest, camera) > (reach + 30.0f) * (reach + 30.0f)) {
            continue;
        }

        for (int32_t i = 0; i < owner->m_ligtsCount; i++) {
            const SMOLight& molt = owner->m_lightList[i];

            // omni and spot lights (a spot lights all round here); directional and ambient ones are the stock lighting
            if (molt.m_type > 1 || molt.m_attenEnd <= 0.25f) {
                continue;
            }

            float intensity = molt.m_intensity > 0.0f ? molt.m_intensity : 1.0f;
            AddLight(Transform(molt.m_position, def->m_mat), ColorOf(molt.m_color), scale * intensity, molt.m_attenStart, molt.m_attenEnd, camera, reach, false);
        }
    }
}

// a light kept from frame to frame: the client animates a model (and so places its lights) only on the frames it
// draws or updates it, so a light found one frame may be missing the next. Matched by position (world space), each
// fades in over 0.3 s and out over 0.6 s instead of switching
struct TrackedLight {
    GxLightingLight m_light;
    float m_weight;
    float m_unseen;       // seconds held off screen
    bool m_seen;
};

std::vector<TrackedLight> s_tracked;
uint32_t s_trackedTime = 0;

// true when a world position (relative to the render origin) is inside the view: a model there is drawn and animated,
// so a light of it that was not found this frame is really gone
bool OnScreen(const C3Vector& relative) {
    CGxMatrixStack& viewStack = g_theGxDevicePtr->m_xforms[GxXform_View];
    const C44Matrix& view = viewStack.m_mtx[viewStack.m_level];
    const C44Matrix& proj = g_theGxDevicePtr->m_projNative;
    C3Vector v = Transform(relative, view);
    float x = v.x * proj.a0 + v.y * proj.b0 + v.z * proj.c0 + proj.d0;
    float y = v.x * proj.a1 + v.y * proj.b1 + v.z * proj.c1 + proj.d1;
    float w = v.x * proj.a3 + v.y * proj.b3 + v.z * proj.c3 + proj.d3;
    return w > 0.5f && fabsf(x) < w * 0.9f && fabsf(y) < w * 0.9f;
}

void SmoothLights(const C3Vector& origin) {
    uint32_t now = OsGetAsyncTimeMs();
    float dt = s_trackedTime ? std::min(static_cast<float>(now - s_trackedTime) * 0.001f, 0.25f) : 0.0f;
    s_trackedTime = now;

    for (TrackedLight& tracked : s_tracked) {
        tracked.m_seen = false;
    }

    // a found light's colour, radius and place eased toward over about a quarter second
    float ease = 1.0f - expf(-dt / 0.25f);

    for (const GxLightingLight& light : s_lights) {
        TrackedLight* match = nullptr;
        float best = 3.0f * 3.0f;

        for (TrackedLight& tracked : s_tracked) {
            float d = DistanceSquared(tracked.m_light.m_position, light.m_position);

            if (!tracked.m_seen && d < best) {
                best = d;
                match = &tracked;
            }
        }

        if (match) {
            GxLightingLight& kept = match->m_light;
            kept.m_position = kept.m_position + (light.m_position - kept.m_position) * ease;
            kept.m_color = kept.m_color + (light.m_color - kept.m_color) * ease;
            kept.m_radius += (light.m_radius - kept.m_radius) * ease;
            kept.m_start += (light.m_start - kept.m_start) * ease;
            kept.m_flicker = light.m_flicker;
            kept.m_specular = light.m_specular;
            match->m_seen = true;
        } else {
            s_tracked.push_back(TrackedLight { light, 0.0f, 0.0f, true });
        }
    }

    s_lights.clear();

    for (size_t i = 0; i < s_tracked.size();) {
        TrackedLight& tracked = s_tracked[i];

        if (tracked.m_seen) {
            tracked.m_weight += dt / 0.3f;
            tracked.m_unseen = 0.0f;
        } else if (!OnScreen(tracked.m_light.m_position - origin) && tracked.m_unseen < 20.0f) {
            // off screen its model is not animated, so it is not found: it still lights what is on screen (20 s)
            tracked.m_unseen += dt;
        } else {
            tracked.m_weight -= dt / 0.6f;
        }

        tracked.m_weight = std::min(tracked.m_weight, 1.0f);

        if (tracked.m_weight <= 0.0f && !tracked.m_seen) {
            s_tracked[i] = s_tracked.back();
            s_tracked.pop_back();
            continue;
        }

        float w = std::max(tracked.m_weight, 0.0f);
        GxLightingLight light = tracked.m_light;
        light.m_color = light.m_color * (w * w * (3.0f - 2.0f * w));
        s_lights.push_back(light);
        i++;
    }
}

float Luminance(const C3Vector& c) {
    return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
}

// a colour scaled to luminance 1 (a tint), white when it is too dark to say
void Tint(const C3Vector& c, float* out) {
    float lum = Luminance(c);

    if (lum < 0.02f) {
        out[0] = out[1] = out[2] = 1.0f;
        return;
    }

    out[0] = fminf(c.x / lum, 3.0f);
    out[1] = fminf(c.y / lum, 3.0f);
    out[2] = fminf(c.z / lum, 3.0f);
}

// g_gxLightingGpu from the params and the lights (render-relative), in the view the world draws with
void WriteGpu(const GxLightingParams& p) {
    GxLightingGpu& g = g_gxLightingGpu;
    std::memset(&g, 0, sizeof(g));
    uint32_t features = p.m_features & (GxLighting_PerPixel | GxLighting_Dynamic | GxLighting_Specular | GxLighting_Hemisphere);

    // ponytail: the nearest kGxLightingMaxLights lights, looped per pixel; Badgermilk bins up to 256 into view
    // clusters with a compute pass, add that if 32 shows its limit
    if (s_lights.size() > kGxLightingMaxLights) {
        const C3Vector& camera = p.m_cameraPosition;
        std::partial_sort(s_lights.begin(), s_lights.begin() + kGxLightingMaxLights, s_lights.end(), [&camera](const GxLightingLight& a, const GxLightingLight& b) {
            return DistanceSquared(a.m_position, camera) - a.m_radius * a.m_radius < DistanceSquared(b.m_position, camera) - b.m_radius * b.m_radius;
        });
        s_lights.resize(kGxLightingMaxLights);
    }

    if (s_lights.empty()) {
        features &= ~GxLighting_Dynamic;
    }

    if (features) {
        CGxMatrixStack& viewStack = g_theGxDevicePtr->m_xforms[GxXform_View];
        const C44Matrix& view = viewStack.m_mtx[viewStack.m_level];
        C44Matrix viewInv = view.Inverse();
        std::memcpy(g.m_view, &view, sizeof(g.m_view));
        std::memcpy(g.m_viewInv, &viewInv, sizeof(g.m_viewInv));
        g.m_flags[0] = features;
        g.m_flags[1] = (features & GxLighting_Dynamic) ? static_cast<uint32_t>(s_lights.size()) : 0u;
        g.m_flags[3] = p.m_debug;
        Tint(p.m_skyColor, g.m_skyTint);
        Tint(p.m_groundColor, g.m_groundTint);
        g.m_skyTint[3] = (features & GxLighting_Hemisphere) ? p.m_hemisphere : 0.0f;

        // world up (z) in view space; the time for the flicker
        C3Vector up(view.c0, view.c1, view.c2);
        float len = sqrtf(up.x * up.x + up.y * up.y + up.z * up.z);
        len = len > 0.0f ? 1.0f / len : 0.0f;
        g.m_upView[0] = up.x * len;
        g.m_upView[1] = up.y * len;
        g.m_upView[2] = up.z * len;
        g.m_upView[3] = p.m_time;
        g.m_lightParams[0] = p.m_lightWrap;
        g.m_lightParams[1] = p.m_dynamicIntensity;
        g.m_lightParams[2] = p.m_specularM2;
        g.m_lightParams[3] = p.m_specularWmo;

        for (uint32_t i = 0; i < g.m_flags[1]; i++) {
            const GxLightingLight& light = s_lights[i];
            GxLightingGpuLight& gpu = g.m_lights[i];
            C3Vector v = Transform(light.m_position, view);
            gpu.m_pos[0] = v.x;
            gpu.m_pos[1] = v.y;
            gpu.m_pos[2] = v.z;
            gpu.m_radius = light.m_radius;
            gpu.m_color[0] = light.m_color.x;
            gpu.m_color[1] = light.m_color.y;
            gpu.m_color[2] = light.m_color.z;
            gpu.m_start = light.m_start;
            gpu.m_flicker = light.m_flicker;
            // a seed from the position, so a light flickers the same way every frame
            gpu.m_seed = fmodf(fabsf(light.m_position.x * 12.9898f + light.m_position.y * 78.233f + light.m_position.z * 37.719f), 6.2831853f);
            gpu.m_specular = light.m_specular;
        }
    }

    g_gxLightingGpuSerial++;
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
    s_cv.debug = Reg("lightingDebug", "1 shows the SSAO term alone; 2 lit surfaces by kind, 3 dynamic light alone; 4, 5 the SSAO inputs", "0");
    // the shader half (lit shader pack in custom.MPQ; D3D11 and Vulkan)
    s_cv.master = Reg("shaderLighting", "Master switch for the light* shader lighting, 0 = all off (D3D11/Vulkan)", "0");
    s_cv.perPixel = Reg("lightPerPixel", "M2, WMO and terrain lit per pixel with their stock formulas (D3D11/Vulkan)", "0");
    s_cv.dynamic = Reg("lightDynamic", "Point lights of models and WMOs light the world per pixel (D3D11/Vulkan)", "0");
    s_cv.dynamicIntensity = Reg("lightDynamicIntensity", "Dynamic light intensity (0-8)", "1");
    s_cv.dynamicM2 = Reg("lightDynamicM2", "Model (torch, brazier) light strength (0-8)", "0.6");
    s_cv.dynamicWmo = Reg("lightDynamicWmo", "WMO (building) light strength (0-8)", "0.5");
    s_cv.dynamicDistance = Reg("lightDynamicDistance", "Dynamic lights reach this far from the camera, yards (20-500)", "100");
    s_cv.flicker = Reg("lightFlicker", "Flame lights flicker this much (0-1)", "0");
    s_cv.wrap = Reg("lightWrap", "Dynamic light wraps round the terminator (0-1)", "0.25");
    s_cv.specular = Reg("lightSpecular", "Specular on WMO specular shaders and shiny models (D3D11/Vulkan)", "0");
    s_cv.specularM2 = Reg("lightSpecularM2", "Model specular strength (0-4)", "0.3");
    s_cv.specularWmo = Reg("lightSpecularWmo", "WMO specular strength (0-4)", "1");
    s_cv.skyAmbient = Reg("lightSkyAmbient", "Ambient tinted by the sky above and the ground below, 0 = off (0-1)", "0");
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

    // the shader half: the lights in reach (world space), smoothed across frames, moved into the render origin
    s_lights.clear();

    if (s_params.m_features & GxLighting_Dynamic) {
        float reach = std::clamp(s_cv.dynamicDistance->m_floatValue, 20.0f, 500.0f);
        GatherM2Lights(World::s_m2Scene, camera, reach);
        GatherWmoLights(camera, reach);
        SmoothLights(origin);

        for (GxLightingLight& light : s_lights) {
            light.m_position = light.m_position - origin;
        }
    } else {
        s_tracked.clear();
    }

    WriteGpu(s_params);
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

    // the lit shaders take their stock path outside the world pass
    if (g_gxLightingGpu.m_flags[0]) {
        g_gxLightingGpu.m_flags[0] = 0;
        g_gxLightingGpuSerial++;
    }

    s_inWorld = false;
}
