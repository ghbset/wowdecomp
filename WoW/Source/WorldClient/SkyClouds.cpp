// Not stock: wxl-light's sky and volumetric clouds, ported from /root/wxl-light (src/Clouds.cpp, Atmosphere.cpp,
// CloudShaders.hpp, CloudField.hpp, CloudNoise.bin) per SKY-CLOUDS-PLAN.md section 4 and the user's decisions:
// the sky colours are wxl-light's (while gfxSunArc is on), the clouds sit where the zone's DNClouds sheet has
// cloud, on by default, in the water reflection too.
//
// DayNight::RenderSky calls Draw in the stock sheet's slot (stars, sun, moon, dome, then this): one screen quad at
// depth 1 with the depth test on, so only pixels nothing has drawn on take it. The constants are wxl-light's
// DrawSky / AtmospherePalette, fed from g_dnInfo in place of the plugin's raw addresses (0x00D38B00 = g_dnInfo).
// The light: g_lightBody ? g_moonToward : g_sunToward. With gfxSunArc on, our disc (the sun, the game's moon
// picture by night) stands in for the stock sun and moon and their glares, as wxl-light's hooks did.
// ponytail: full resolution, 20 steps, as wxl-light; half resolution + upsample if a frame capture ever shows it.
#include "WoW/Source/WorldClient/SkyClouds.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "WoW/Source/WorldClient/CataWater.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/VolumetricFogWater.h"

namespace SkyClouds {

namespace {

CVar* s_cvEnable;
CVar* s_cvCover;
CVar* s_cvSpeed;
CVar* s_cvReflect;

// wxl-light's fixed settings (light.ini defaults): opacity 0.9, sunset 0.85, depth_colour 1, disc 1, zone 1.
constexpr float kOpacity = 0.9f;
constexpr float kSunsetWarmth = 0.85f;
constexpr float kDepthColour = 1.0f;
constexpr uint32_t kAliveMs = 500;

constexpr uint32_t kNoiseSize = 64;
constexpr uint32_t kAtlasW = 528, kAtlasH = 720;   // SkyClouds.hlsl kAtlas
constexpr int kLevels = 6;

CGxShader* s_shader;
bool s_shaderFailed;
CGxTex* s_noise;
bool s_noiseTried;
std::vector<uint32_t> s_atlas;
float s_drift;
uint32_t s_lastMs;
uint32_t s_discMs;
bool s_discDrawn;

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float Clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float Ramp(float a, float b, float x) { const float t = Clamp01((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float Disp(float v) { return std::pow(Clamp01(v), 1.0f / 2.2f); }
void Rgb(const CImVector& c, float* out) { out[0] = c.r / 255.0f; out[1] = c.g / 255.0f; out[2] = c.b / 255.0f; }

// The 64^3 B G R A volume (tools/clouds/cloudnoise.py) and its mips as a 2D atlas: level L (n = 64 >> L) is a grid
// of min(n, 8) columns of (n + 2)^2 tiles, each slice with a one-texel wrap border, the levels stacked at
// y = 0, 528, 664, 700, 710, 716 (SkyClouds.hlsl NoiseLevel). Each mip texel is the mean of the eight under it,
// rounded, as wxl-light's volume mips.
bool BuildAtlas(const uint8_t* volume) {
    s_atlas.assign(kAtlasW * kAtlasH, 0);
    std::vector<uint8_t> cur(volume, volume + size_t(kNoiseSize) * kNoiseSize * kNoiseSize * 4), next;
    uint32_t y0 = 0;
    for (int level = 0, n = kNoiseSize; level < kLevels; ++level, n /= 2) {
        const uint32_t cols = std::min<uint32_t>(n, 8), tile = n + 2;
        for (uint32_t z = 0; z < uint32_t(n); ++z) {
            const uint32_t ox = (z % cols) * tile, oy = y0 + (z / cols) * tile;
            for (uint32_t j = 0; j < tile; ++j) {
                const uint32_t y = (j + n - 1) % n;
                for (uint32_t i = 0; i < tile; ++i) {
                    const uint32_t x = (i + n - 1) % n;
                    uint32_t texel;
                    std::memcpy(&texel, &cur[((size_t(z) * n + y) * n + x) * 4], 4);
                    s_atlas[(oy + j) * kAtlasW + ox + i] = texel;
                }
            }
        }
        y0 += (n / cols) * tile;
        const uint32_t h = n / 2;
        next.assign(size_t(h) * h * h * 4, 0);
        for (uint32_t z = 0; z < h; ++z)
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < h; ++x)
                    for (uint32_t ch = 0; ch < 4; ++ch) {
                        uint32_t sum = 0;
                        for (uint32_t k = 0; k < 8; ++k)
                            sum += cur[(((2 * z + (k >> 2)) * n + 2 * y + ((k >> 1) & 1)) * n + 2 * x + (k & 1)) * 4 + ch];
                        next[((size_t(z) * h + y) * h + x) * 4 + ch] = uint8_t((sum + 4) / 8);
                    }
        cur.swap(next);
    }
    return y0 == kAtlasH;
}

void AtlasCallback(EGxTexCommand command, uint32_t, uint32_t, uint32_t, uint32_t, void*, uint32_t& stride, const void*& texels) {
    if (command == GxTex_Latch) {
        texels = s_atlas.data();
        stride = kAtlasW * 4;
    }
}

CGxTex* Noise() {
    if (!s_noiseTried) {
        s_noiseTried = true;
        SFileImpl* file = nullptr;
        SFileOpenFile("VFog\\cloudnoise.bin", &file);
        std::vector<uint8_t> data(file ? SFileGetFileSize(file, nullptr) : 0);
        const bool read = file && data.size() == size_t(kNoiseSize) * kNoiseSize * kNoiseSize * 4
            && SFileReadFile(file, data.data(), static_cast<uint32_t>(data.size()), nullptr, nullptr, 0);
        if (file) {
            SFileCloseFile(file);
        }
        if (read && BuildAtlas(data.data())) {
            CGxTexFlags flags(GxTex_Linear, 1, 1, 0, 0, 0, 1, 0, 0, 0);
            g_theGxDevicePtr->TexCreate(GxTex_2d, kAtlasW, kAtlasH, 0, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, AtlasCallback, "SkyCloudNoise", s_noise);
        }
        if (!s_noise) {
            ConsolePrintf("skyClouds: no VFog\\cloudnoise.bin (64^3 BGRA) in custom.MPQ; stock clouds");
        }
    }
    return s_noise;
}

CGxShader* Shader() {
    if (s_shader && !s_shader->Valid()) {   // device lost or recreated: made again once
        g_theGxDevicePtr->ShaderDestroy(s_shader);
        s_shader = nullptr;
    }
    if (!s_shader && !s_shaderFailed) {
        g_theGxDevicePtr->ShaderCreate(&s_shader, GxSh_Pixel, "Shaders\\Pixel", "SkyClouds", 1);
        s_shaderFailed = !s_shader || !s_shader->Valid();
        if (s_shaderFailed) {
            ConsolePrintf("skyClouds: no Shaders\\Pixel\\SkyClouds for this device; stock clouds");
        }
    }
    return s_shader && s_shader->Valid() ? s_shader : nullptr;
}

// ---- Atmosphere.cpp, with g_dnInfo for the plugin's addresses. z = the sun's own elevation sine (its disc).
struct Frame {
    float light[3];   // toward the sun or moon, whichever lights the world
    float sunZ;       // the sun's elevation sine
    float day;        // m_dayProgression
};

float DayShare(const Frame& f) { return Ramp(-0.165f, -0.09f, f.sunZ); }
float SunUnder(const Frame& f) { return Ramp(0.0f, -0.12f, f.sunZ); }
float LowSun(const Frame& f) { return kSunsetWarmth * Ramp(0.34f, 0.03f, f.sunZ) * Ramp(-0.16f, -0.07f, f.sunZ) * DayShare(f); }

void Palette(const Frame& f, float share, float c[5][4]) {
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    float top[3], mid[3], hor[3], sun[3];
    Rgb(info.m_bands.m_skyColors[0], top);
    Rgb(info.m_bands.m_skyColors[1], mid);
    Rgb(info.m_bands.m_skyColors[5], hor);
    Rgb(info.m_bands.m_sunCloudColors[0], sun);
    const float storm = std::isfinite(info.m_stormBlend) ? Clamp01(info.m_stormBlend) : 0.0f;
    const float* own[3] = { hor, mid, top };
    float* band[3] = { c[0], c[1], c[4] };
    for (int b = 0; b < 3; ++b)
        for (int i = 0; i < 3; ++i) band[b][i] = std::pow(own[b][i], 2.2f);
    c[4][3] = 0.0f;
    const float smax = std::max({ sun[0], sun[1], sun[2], 1e-3f });
    const float up = std::max(Clamp01((f.light[2] + 0.10f) / 0.25f), 0.45f * Ramp(-0.165f, -0.07f, f.sunZ) * Ramp(0.10f, 0.0f, f.sunZ));
    static const float kLowEve[3] = { 1.0f, 0.40f, 0.13f }, kLowMorn[3] = { 1.0f, 0.52f, 0.30f };
    const float* kLow = f.day < 0.5f ? kLowMorn : kLowEve;
    const float lowSun = LowSun(f);
    for (int i = 0; i < 3; ++i)
        c[2][i] = std::pow(sun[i] / smax, 2.2f) * (1.0f + (kLow[i] - 1.0f) * lowSun) * 0.9f * (1.0f + 0.5f * lowSun) * up * (1.0f - 0.5f * storm);
    const bool morning = f.day < 0.5f;
    static const float kEve[4][3]  = { { 0.62f, 0.34f, 0.20f }, { 0.50f, 0.28f, 0.28f }, { 0.07f, 0.10f, 0.25f }, { 0.44f, 0.22f, 0.28f } };
    static const float kMorn[4][3] = { { 0.66f, 0.44f, 0.34f }, { 0.54f, 0.38f, 0.40f }, { 0.09f, 0.15f, 0.30f }, { 0.48f, 0.30f, 0.36f } };
    const float (*tw)[3] = morning ? kMorn : kEve;
    const float under = SunUnder(f);
    const float show = kSunsetWarmth * Ramp(0.34f, 0.03f, f.sunZ) * share * (1.0f - 0.5f * storm);
    static const float kDusk[3][3] = { { 0.17f, 0.14f, 0.26f }, { 0.07f, 0.09f, 0.24f }, { 0.02f, 0.035f, 0.12f } };
    float goal[3][3];
    for (int b = 0; b < 3; ++b) {
        float ours[3], z[3];
        for (int i = 0; i < 3; ++i) {
            ours[i] = tw[b][i] + (kDusk[b][i] - tw[b][i]) * under;
            z[i] = std::pow(own[b][i], 2.2f);
        }
        const float hi = std::max({ own[b][0], own[b][1], own[b][2], 1e-3f }), lo = std::min({ own[b][0], own[b][1], own[b][2] });
        const float w = Clamp01((hi - lo) / hi / 0.35f);
        const float k = Luma(ours) / std::max(Luma(z), 1e-3f);
        for (int i = 0; i < 3; ++i) goal[b][i] = ours[i] + (std::min(z[i] * k, 1.0f) - ours[i]) * w;
    }
    float belt[3];
    for (int i = 0; i < 3; ++i) {
        c[0][i] += (goal[0][i] - c[0][i]) * show;
        c[1][i] += (goal[1][i] - c[1][i]) * show;
        c[4][i] += (goal[2][i] - c[4][i]) * show * (0.8f + 0.2f * under);
        belt[i] = tw[3][i] * show * (1.0f - under);
    }
    c[0][3] = share; c[1][3] = 0.0f; c[2][3] = belt[0]; c[3][3] = belt[1]; c[4][3] = belt[2];
    c[3][0] = f.light[0]; c[3][1] = f.light[1]; c[3][2] = f.light[2];
}

// The quad at depth 1 (viewport z 1..1) over the current viewport: only the sky's pixels pass LEQUAL.
void DrawFarQuad(const VolumetricFog::WorldViewport& vp) {
    static uint16_t s_indices[4] = { 0, 3, 1, 2 };
    static CGxBatch s_batch = { GxPrim_TriangleStrip, 0, 4, 0, 3 };
    const float w = vp.w, h = vp.h;
    C44Matrix projection;
    GxuXformCreateOrtho(0.0f, w, 0.0f, h, -1.0f, 1.0f, projection);
    GxXformSetProjection(projection);
    GxXformSetViewport(vp.minX, vp.maxX, vp.minY, vp.maxY, 1.0f, 1.0f);
    const EGxApi api = g_theGxDevicePtr->DeviceApi();
    const float offset = api == GxApi_D3d9 || api == GxApi_D3d9Ex ? 0.5f : 0.0f;
    const float minU = offset / w, maxU = (w + offset) / w, minV = offset / h, maxV = (h + offset) / h;
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

}  // namespace

void RegisterCVars() {
    s_cvEnable = CVar::Register("skyClouds", "Volumetric clouds in place of the stock cloud sheet, and (with gfxSunArc) wxl-light's sky (0 = stock)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvCover = CVar::Register("skyCloudCover", "skyClouds: cloud cover in fair weather where the zone has no cloud sheet (0 = none, 1 = overcast)", 1, "0.35", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvSpeed = CVar::Register("skyCloudSpeed", "skyClouds: how fast the cloud shapes drift (1 = wxl-light)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_cvReflect = CVar::Register("skyCloudsReflect", "skyClouds in the water reflection too (0 = the stock sheet there)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

bool Active() {
    if (!s_cvEnable || !s_cvEnable->m_intValue || !g_theGxDevicePtr) {
        return false;
    }
    if (CataWater::Reflecting() && !(s_cvReflect && s_cvReflect->m_intValue)) {
        return false;
    }
    return Shader() && Noise();
}

bool Disc() {
    return DayNight::SunArc();
}

bool HidesGlares() {
    return s_discDrawn && OsGetAsyncTimeMs() - s_discMs < kAliveMs && Active() && Disc();
}

void Draw() {
    CGxShader* shader = Shader();
    CGxTex* noise = Noise();
    if (!shader || !noise) {
        return;
    }
    const DayNight::DNInfo& info = DayNight::g_dnInfo;
    const bool arc = DayNight::SunArc();
    const bool reflecting = CataWater::Reflecting();

    Frame f;
    const bool moon = arc ? DayNight::g_lightBody == 1 : (info.m_dayProgression < 0.2013889f || info.m_dayProgression > 0.9236111f);
    const C3Vector& L = moon ? DayNight::g_moonToward : DayNight::g_sunToward;
    f.light[0] = L.x; f.light[1] = L.y; f.light[2] = L.z;
    f.sunZ = DayNight::g_sunToward.z;
    f.day = info.m_dayProgression;
    const float share = DayShare(f);
    const float paint = arc ? share : 0.0f;     // wxl-light's sky only under the arc (the user's decision 2)

    const uint32_t now = OsGetAsyncTimeMs();
    if (!reflecting) {
        const float dt = s_lastMs ? std::min(float(now - s_lastMs) * 0.001f, 0.25f) : 0.0f;
        s_lastMs = now;
        const float speed = s_cvSpeed ? s_cvSpeed->m_floatValue : 1.0f;
        s_drift = std::fmod(s_drift + dt * speed * 0.004f, 4096.0f);
    }

    // The game's cloud sheet (alpha = DNClouds density, cut at the zone's cloud density) says where the cloud is.
    DayNight::DNClouds& clouds = DayNight::g_clouds;
    CGxTex* stock = clouds.m_enabled ? TextureGetGxTex(clouds.m_textures[clouds.m_texIndex & 1], 0, nullptr) : nullptr;
    CGxTex* moonTex = arc && share < 1.0f ? TextureGetGxTex(DayNight::g_planets.m_moon.m_texture, 0, nullptr) : nullptr;

    const VolumetricFog::WorldViewport vp = VolumetricFog::LiveWorldViewport();
    if (vp.w < 1.0f || vp.h < 1.0f) {
        return;
    }
    // The view rays from the live view and projection (D3D convention, +z forward; the reflection's mirrored ones too).
    CGxDevice* device = g_theGxDevicePtr;
    const CGxMatrixStack& viewStack = device->m_xforms[GxXform_View];
    float v[16], p[16];
    std::memcpy(v, &viewStack.m_mtx[viewStack.m_level], sizeof(v));
    std::memcpy(p, &device->m_projection, sizeof(p));
    const float xs = p[0] != 0.0f ? p[0] : 1.0f, ys = p[5] != 0.0f ? p[5] : 1.0f;

    float c[13][4] = {};
    c[0][0] = v[0] / xs; c[0][1] = v[4] / xs; c[0][2] = v[8] / xs;
    c[1][0] = v[1] / ys; c[1][1] = v[5] / ys; c[1][2] = v[9] / ys; c[1][3] = stock ? 1.0f : 0.0f;
    c[2][0] = v[2];      c[2][1] = v[6];      c[2][2] = v[10];     c[2][3] = moonTex ? 1.0f : 0.0f;
    float (*atmo)[4] = c + 8;
    Palette(f, share, atmo);
    atmo[0][3] = paint;

    // ---- wxl-light Clouds.cpp DrawSky: the clouds' light and shade, cover, drift, the disc.
    const float storm = std::isfinite(info.m_stormBlend) ? Clamp01(info.m_stormBlend) : 0.0f;
    float sky[3];
    Rgb(info.m_bands.m_skyColors[5], sky);
    const float night = 1.0f - share;
    const float skyLum = Luma(sky);
    const float moonUp = Clamp01(f.light[2] / 0.15f);
    static const float kMoonTint[3] = { 0.80f, 0.86f, 0.95f };
    const float low = LowSun(f), under = SunUnder(f);
    const float* glow = atmo[2];
    const float glowTop = std::max(std::max(glow[0], glow[1]), std::max(glow[2], 1.0e-4f));
    const float eyeLum = Luma(atmo[0]);
    for (int i = 0; i < 3; ++i) {
        const float fire = Disp(glow[i] / glowTop);
        const float white = std::min(atmo[0][i] / std::max(eyeLum, 1.0e-3f), 1.5f) * Clampf(eyeLum / 0.6f, 0.25f, 1.0f);
        const float dayLit = Disp(0.6f * atmo[0][i] + 0.6f * atmo[2][i] + 0.5f * white) * (1.0f - under) + Disp(1.15f * atmo[0][i]) * under;
        c[3][i] = (dayLit * (1.0f - 0.85f * low) + fire * 1.05f * 0.85f * low) * (1.0f - 0.35f * storm) * share
                + kMoonTint[i] * 0.16f * moonUp * night;
        const float fogTint = 1.0f + (std::min(sky[i] / std::max(skyLum, 1.0e-3f), 1.6f) - 1.0f) * 0.5f * kDepthColour;
        c[4][i] = Disp(0.85f * atmo[0][i] + 0.10f * atmo[1][i]) * fogTint * (1.0f - 0.33f * low) * (1.0f - 0.25f * under) * (1.0f - 0.40f * storm) * share
                + (sky[i] * 0.55f + skyLum * 0.45f) * 0.9f * night;
    }
    const float zoneDensity = 1.0f - float(clouds.m_density) / 255.0f;
    const float zoneFactor = stock ? 1.0f : Clampf(1.0f + (zoneDensity - 0.75f), 0.6f, 1.25f);
    const float cover = s_cvCover ? Clamp01(s_cvCover->m_floatValue) : 0.35f;
    c[3][3] = Clamp01(cover * zoneFactor + 0.30f * storm);
    // A zone with a short fog is closed in: up to 0.85 cover by Tirisfal's (fog end 7700, on a cubic).
    const float fogEnd = info.m_bands.m_fogEnd;
    const float closed = std::isfinite(fogEnd) ? Clamp01((11000.0f - fogEnd) / 3300.0f) : 0.0f;
    c[3][3] += std::max(0.85f - c[3][3], 0.0f) * closed * closed * closed;
    c[4][3] = kOpacity;
    c[5][0] = s_drift; c[5][1] = s_drift * 0.35f; c[5][2] = vp.w; c[5][3] = vp.h;
    c[6][0] = f.light[0]; c[6][1] = f.light[1]; c[6][2] = f.light[2];
    if (arc) {
        float direct[3];
        Rgb(info.m_bands.m_diffuse, direct);
        const float top = std::max(std::max(direct[0], direct[1]), std::max(direct[2], 1.0e-3f));
        const float lowDisc = 1.0f - Clamp01((f.light[2] - 0.05f) / 0.25f);
        static const float kMoon[3] = { 0.80f, 0.86f, 0.95f };
        static const float kLowSunDisc[3] = { 1.0f, 0.52f, 0.20f };
        for (int i = 0; i < 3; ++i)
            c[7][i] = (0.6f + 0.4f * direct[i] / top) * (1.0f + (kLowSunDisc[i] - 1.0f) * low) * share + kMoon[i] * (1.0f - share);
        c[7][3] = 0.30f * share + 0.45f * (1.0f - share);
        c[6][3] = (0.026f * share + 0.035f * (1.0f - share)) * (1.0f + 0.6f * lowDisc * share);
    }

    VolumetricFog::SavedPassState saved;
    VolumetricFog::BeginPasses(saved);
    GxRsSet(GxRs_ScissorTest, 1);     // RenderSky's portal rect
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthFunc, 0);       // LEQUAL (s_cmpFunc[0])
    GxRsSet(GxRs_ColorWrite, 7);      // rgb: the frame's alpha is the glow mask
    GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
    device->RsSet(GxRs_PixelShader, shader);
    VolumetricFog::SetPs(0, &c[0][0], 13);
    VolumetricFog::Bind(0, noise);
    VolumetricFog::Bind(3, moonTex);
    VolumetricFog::Bind(4, stock);
    DrawFarQuad(vp);
    VolumetricFog::EndPasses(saved, vp);

    if (!reflecting) {
        s_discDrawn = arc;
        s_discMs = now;
    }
}

}  // namespace SkyClouds
