// Cata water (waterStyle 1): see CataWater.h and docs/BACKLOG_PORT.md "Cata water". Not in the 3.3.5a binary.
#include "WoW/Source/WorldClient/CataWater.h"
#include <algorithm>
#include <cmath>
#include "Engine/Source/Base/Status.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/CMath.h"
#include "WoW/Source/DB/Rec/LightRec.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/DB/WowClientDB.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/WorldClient/CChunkLiquid.h"
#include "WoW/Source/WorldClient/CPortalView.h"
#include "WoW/Source/WorldClient/LiquidWaveManager.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include "WoW/Source/WorldClient/World.h"

namespace CataWater {

namespace {

using VolumetricFog::Bind;

// Cata 4.3.4 LiquidType floats for the rows 3.3.5a draws with the water material (3.3.5a leaves them zero):
// /root/wow434/water-assets/README.md, as wxl-water Water.cpp kCataRows. Every 3.3.5a water row exists in Cata under
// the same id and name. p7: foam, p15: ripple strength (piece B).
struct Params { float p3, p4, p5deg, p6, p7, p8, p9, p10, p14, p15; };
struct Row { int32_t id; Params p; };
constexpr Row kRows[] = {
    {   1, { 1.0f, 2.5f, 90.0f, 1.0f, 0.5f, 0.5f, 0.2f, 0.7f, 0.5f, 2.5f } },     // Water
    {   2, { 1.0f, 2.5f, 90.0f, 0.7f, 1.0f, 0.25f, 0.2f, 0.7f, 1.0f, 2.0f } },    // Ocean
    {   5, { 1.0f, 2.5f, 90.0f, 0.7f, 0.5f, 0.5f, 0.2f, 0.5f, 0.7f, 2.5f } },     // Slow Water
    {   6, { 1.0f, 2.5f, 0.0f, 0.5f, 1.0f, 0.25f, 0.2f, 0.7f, 1.0f, 2.0f } },     // Slow Ocean
    {   9, { 1.0f, 2.5f, 90.0f, 0.5f, 0.5f, 0.5f, 0.2f, 0.7f, 0.5f, 2.5f } },     // Fast Water
    {  10, { 1.0f, 2.5f, 0.0f, 0.5f, 1.0f, 0.25f, 0.2f, 0.7f, 1.0f, 2.0f } },     // Fast Ocean
    {  13, { 0.25f, 0.5f, 270.0f, 0.5f, 1.0f, 1.0f, 0.2f, 0.7f, 0.7f, 1.0f } },   // WMO Water
    {  14, { 0.25f, 0.625f, 270.0f, 0.5f, 1.0f, 0.25f, 0.2f, 0.7f, 1.0f, 2.0f } }, // WMO Ocean
    {  17, { 0.25f, 0.5f, 270.0f, 1.0f, 0.5f, 0.5f, 0.2f, 0.7f, 0.5f, 1.0f } },   // WMO Water - Interior
    {  41, { 0.25f, 0.5f, 270.0f, 1.0f, 0.5f, 0.5f, 0.2f, 0.7f, 0.5f, 2.5f } },   // Coilfang Raid - Water
    {  61, { 1.0f, 2.5f, 90.0f, 1.0f, 0.5f, 0.5f, 0.2f, 0.7f, 0.5f, 2.5f } },     // Hyjal Past - Water
    {  81, { 1.0f, 2.5f, 90.0f, 1.0f, 0.5f, 0.5f, 0.2f, 0.7f, 0.5f, 2.5f } },     // Lake Wintergrasp - Water
    { 100, { 0.1f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.0f, 0.1f, 0.2f } },      // Basic Procedural Water
    { 181, { 1.0f, 1.25f, 0.0f, 0.5f, 1.0f, 1.0f, 0.2f, 0.7f, 0.2f, 2.5f } },     // Orange Slime
};
constexpr Params kWater = kRows[0].p;   // unlisted water-material rows: Water, or Ocean on the ocean sound bank
constexpr Params kOcean = kRows[1].p;
constexpr uint32_t kMaterialWater = 1;
constexpr int32_t kSoundBankOcean = 1;
constexpr float kMinSize = 16.0f;

// Texture stages (CataWaterPs.hlsl); stage 0 is the material's own depth-colour ramp.
constexpr int kNormalStage = 9;
constexpr int kFoamStage = 10;
constexpr int kRippleStage = 11;
constexpr int kSceneStage = 12;
constexpr int kDepthStage = 13;
constexpr int kReflStage = 14;

CVar* s_style;
CVar* s_ripples;
CVar* s_reflMode;
CVar* s_reflScale;
CGxShader* s_vs;
CGxShader* s_psAbove;
CGxShader* s_psAboveP;     // PLANAR: Cata p = 1
CGxShader* s_psBelow;
CGxShader* s_depthCopy;     // VFogDepthCopy: the raw scene depth into R32F
CGxShader* s_psTint;        // CataWaterTint: FFXUnderWaterLow
CTexture* s_normalTex;
CTexture* s_foamTex;
CGxTex* s_scene;
CGxTex* s_depth;
CGxTex* s_refl;             // the planar reflection, mirrored left to right
bool s_callbackAdded;
bool s_failed;              // a shader is missing: stay off until the device is restored

struct Pass {
    bool armed;
    bool below;
    VolumetricFog::WorldViewport vp;
    float depthTex[4];      // c20
    float maxZ;
    VolumetricFog::RippleShading ripples;   // map nullptr: no ripples this pass
    float fogA[3];          // c24: Cata fog A from the zone's underwater fog
    float fogColour[3];     // c25
    bool fogValid;
};
Pass s_pass;

// ---- planar reflection (piece D) ---------------------------------------------------------------------------------
// Which water height the reflection mirrors through, from one frame's water draws: wxl-water src/PlanePick.hpp.
namespace planepick {
constexpr float kBucket = 2.0f;     // the shader holds the reflection to +/- 2 yd of the plane
constexpr float kHold = 1.5f;       // another body must look this much bigger to take over
struct Cand {
    float h, weight, hw;
    uint32_t n;
    float Height() const { return weight > 0.0f ? hw / weight : h; }
};
constexpr int kCands = 8;
Cand s_cands[kCands];

// One draw's liquid sphere seen from the camera, weighted r^2 / d^2 (a proxy for its screen area, capped at 1 inside it).
void Add(const CAaSphere& sphere, const C3Vector& cam) {
    const float dx = sphere.c.x - cam.x, dy = sphere.c.y - cam.y, dz = sphere.c.z - cam.z;
    const float r2 = sphere.r * sphere.r;
    const float w = r2 / std::max(dx * dx + dy * dy + dz * dz, std::max(r2, 1.0e-4f));
    Cand* slot = nullptr;
    for (int i = 0; i < kCands && !slot; i++) {
        if (s_cands[i].n && std::fabs(s_cands[i].Height() - sphere.c.z) < kBucket) slot = &s_cands[i];
    }
    for (int i = 0; i < kCands && !slot; i++) {
        if (!s_cands[i].n) { slot = &s_cands[i]; *slot = Cand{ sphere.c.z, 0.0f, 0.0f, 0 }; }
    }
    if (!slot) return;
    slot->n++;
    slot->weight += w;
    slot->hw += w * sphere.c.z;
}

// The heaviest body (bodies within a bucket merged); the current plane holds unless another is kHold times heavier.
bool Pick(bool curValid, float curH, float& h) {
    for (bool merged = true; merged;) {
        merged = false;
        for (int i = 0; i < kCands; i++) {
            for (int j = i + 1; j < kCands; j++) {
                Cand& a = s_cands[i];
                Cand& b = s_cands[j];
                if (a.n && b.n && std::fabs(a.Height() - b.Height()) < kBucket) {
                    a.n += b.n; a.weight += b.weight; a.hw += b.hw; b.n = 0;
                    merged = true;
                }
            }
        }
    }
    const Cand* best = nullptr;
    const Cand* cur = nullptr;
    for (const Cand& c : s_cands) {
        if (!c.n) continue;
        if (!best || c.weight > best->weight) best = &c;
        if (curValid && std::fabs(c.Height() - curH) < kBucket) cur = &c;
    }
    if (cur && best != cur && best->weight < cur->weight * kHold) best = cur;
    if (best) h = best->Height();
    for (Cand& c : s_cands) c.n = 0;
    return best != nullptr;
}
}  // namespace planepick

struct Reflection {
    bool planeValid;        // picked from last frame's water draws
    float planeH;           // world z
    bool drawn;             // this frame's target holds the plane's reflection
    bool reflecting;        // inside RenderReflection
    float h;                // the drawn plane, world z
};
Reflection s_reflection;

// Oblique near plane (Lengyel; wxl-water Water.cpp BuildOblique): the native projection (D3D form, z in [0, w], row
// vectors: what every backend draws with) with its z column replaced so z = 0 on `plane` (in the space the view takes)
// and < 0 on its negative side; x, y and w kept, the far plane through the old far corner.
bool BuildOblique(const C44Matrix& P, const C44Matrix& V, const float plane[4], C44Matrix& out, float& s) {
    const C44Matrix invV = V.Inverse();
    const C44Matrix invP = P.Inverse();
    const float* iv = &invV.a0;
    const float* ip = &invP.a0;
    float cu[4] = {}, cp[4] = {}, q[4] = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) cu[i] += iv[i * 4 + j] * plane[j];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) cp[i] += ip[i * 4 + j] * cu[j];
    const float k[4] = { static_cast<float>((cp[0] > 0.0f) - (cp[0] < 0.0f)), static_cast<float>((cp[1] > 0.0f) - (cp[1] < 0.0f)), 1.0f, 1.0f };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) q[i] += k[j] * ip[j * 4 + i];
    const float dot = cu[0] * q[0] + cu[1] * q[1] + cu[2] * q[2] + cu[3] * q[3];
    if (!(dot > 1.0e-6f) || !std::isfinite(dot)) return false;
    s = 1.0f / dot;
    out = P;
    float* o = &out.a0;
    for (int i = 0; i < 4; i++) o[i * 4 + 2] = s * cu[i];
    return true;
}

// wxl-water's worked example (water-stage5-design.md 3.2): n 0.2, f 500, 45 deg, 16:9, a level camera 10 yd above the
// water (its V with the D3D z flip folded in). Off on a mismatch.
bool ObliqueSelfTest() {
    const C44Matrix P(1.357995f, 0, 0, 0, 0, 2.414214f, 0, 0, 0, 0, 1.00040016f, 1, 0, 0, -0.20008003f, 0);
    const C44Matrix V(0, 0, 1, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 1);
    const float plane[4] = { 0.0f, 0.0f, 1.0f, -10.0f };
    C44Matrix out;
    float s = 0.0f;
    if (!BuildOblique(P, V, plane, out, s)) return false;
    auto ndc = [&](float z) {
        const C3Vector clipZ = C3Vector(30.0f, 0.0f, z) * V;
        const float cz = clipZ.x * out.a2 + clipZ.y * out.b2 + clipZ.z * out.c2 + out.d2;
        const float cw = clipZ.x * out.a3 + clipZ.y * out.b3 + clipZ.z * out.c3 + out.d3;
        return cz / cw;
    };
    return std::fabs(s - 2.53670f) < 1.0e-3f && std::fabs(ndc(10.0f)) < 1.0e-4f
        && std::fabs(ndc(11.0f) - 0.084557f) < 1.0e-3f && std::fabs(ndc(9.0f) + 0.084557f) < 1.0e-3f;
}

void DropTargets() {
    VolumetricFog::DestroyTex(s_scene);
    VolumetricFog::DestroyTex(s_depth);
    VolumetricFog::DestroyTex(s_refl);
    s_reflection.drawn = false;
}

void __fastcall OnDeviceRestored() {
    DropTargets();
    VolumetricFog::DropInvalidShaders(&s_vs, 1);
    VolumetricFog::DropInvalidShaders(&s_psAbove, 1);
    VolumetricFog::DropInvalidShaders(&s_psBelow, 1);
    VolumetricFog::DropInvalidShaders(&s_psAboveP, 1);
    VolumetricFog::DropInvalidShaders(&s_depthCopy, 1);
    VolumetricFog::DropInvalidShaders(&s_psTint, 1);
    VolumetricFog::RippleRelease();
    s_failed = false;
}

CGxTex* Target(CGxTex*& tex, uint32_t w, uint32_t h, EGxTexFormat format, EGxTexFilter filter, const char* name) {
    if (tex && (tex->m_width != w || tex->m_height != h)) {
        VolumetricFog::DestroyTex(tex);
    }
    if (!tex) {
        CGxTexFlags flags(filter, 0, 0, 0, 0, 1, 1, 0, 0, 0);
        g_theGxDevicePtr->TexCreate(GxTex_2d, w, h, 0, format, format, flags, nullptr, GxTexNullUserFunc, name, tex);
    }
    return tex;
}

bool EnsureShaders() {
    if (!s_vs) {
        g_theGxDevicePtr->ShaderCreate(&s_vs, GxSh_Vertex, "Shaders\\Vertex", "CataWater", 1);
    }
    return s_vs && s_vs->Valid() && VolumetricFog::Shader(s_psAbove, "CataWaterAbove") && VolumetricFog::Shader(s_psBelow, "CataWaterBelow")
        && VolumetricFog::Shader(s_psAboveP, "CataWaterAboveP")
        && VolumetricFog::Shader(s_depthCopy, "VFogDepthCopy");
}

CGxTex* Texture(CTexture*& tex, const char* file) {
    if (!tex) {
        CStatus status;
        tex = TextureCreate(file, CGxTexFlags(GxTex_LinearMipLinear, 1, 1, 0, 0, 0, 1, 0, 0, 0), &status, 0);
    }
    return tex ? TextureGetGxTex(tex, 0, nullptr) : nullptr;
}

// The LiquidType row a material's settings were made for (the settings bank is indexed by row id).
int32_t RowOf(const Liquid::CMaterialSettings* settings, const LiquidTypeRec** rec) {
    const uint32_t count = Liquid::CSettingsBank::s_settings.Count();
    for (uint32_t id = 0; id < count; id++) {
        if (Liquid::CSettingsBank::s_settings.m_data[id] == settings) {
            *rec = Liquid::s_liquidTypeDB ? Liquid::s_liquidTypeDB->GetRecord(static_cast<int32_t>(id)) : nullptr;
            return static_cast<int32_t>(id);
        }
    }
    *rec = nullptr;
    return -1;
}

const Params* ParamsOf(const Liquid::CMaterialSettings* settings) {
    const LiquidTypeRec* rec = nullptr;
    const int32_t id = RowOf(settings, &rec);
    if (!rec || static_cast<uint32_t>(rec->m_materialID) != kMaterialWater) {
        return nullptr;
    }
    for (const Row& r : kRows) {
        if (r.id == id) {
            return &r.p;
        }
    }
    return rec->m_soundBank == kSoundBankOcean ? &kOcean : &kWater;
}

// The raw scene depth, point for point, into s_depth (VFogDepthCopy); c20 = its texel size and the view depth decode
// (vfog BuildCommon's c3: view depth = B / (d - A) from the GL-form projection and the world depth range).
bool CopyDepth() {
    CGxDevice* device = g_theGxDevicePtr;
    bool transient = false;
    CGxTex* depth = device->DeviceCopySceneDepth(&transient);
    const VolumetricFog::WorldViewport& vp = s_pass.vp;
    if (!depth || vp.x + vp.w > depth->m_width || vp.y + vp.h > depth->m_height
        || !Target(s_depth, depth->m_width, depth->m_height, GxTex_R32F, GxTex_Nearest, "CataWaterDepth")) {
        return false;
    }
    static VolumetricFog::SavedPassState s_saved;
    VolumetricFog::BeginPasses(s_saved);
    Bind(0, depth);
    GxRenderTargetSet(GxBuffers_Color, s_depth, 0);
    device->RsSet(GxRs_PixelShader, s_depthCopy);
    device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
    VolumetricFog::DrawQuad(s_depth->m_width, s_depth->m_height);
    device->DeviceSceneDepthBound(true);
    GxRenderTargetSet(GxBuffers_Color, vp.target, 0);
    VolumetricFog::EndPasses(s_saved, vp);

    const C44Matrix& p = device->m_projection;
    const float extent = vp.maxZ - vp.minZ;
    s_pass.depthTex[0] = 1.0f / depth->m_width;
    s_pass.depthTex[1] = 1.0f / depth->m_height;
    s_pass.depthTex[2] = vp.minZ + extent * (1.0f + p.c2) * 0.5f;
    s_pass.depthTex[3] = extent * p.d2 * 0.5f;
    s_pass.maxZ = vp.maxZ + 1.0e-5f;
    return true;
}

float Channel(uint8_t c) {
    return c / 255.0f;
}

// Cata's fog A (wxl-water ResolveThicknessFog, water-stage2-fog-research.md 3.1): the underwater light params of the
// base light and of every area light around the light reference point, far to near, each by its falloff, at the
// current time with the storm blend (DayNight's own resolve, so its fog mode decides the far-clip rewrite), then Cata
// FUN_00648190: end at most the far clip, start = multiplier x end, exponent x 2. The override light, zone lights and
// WMO fog are left out, as wxl-water does. Under water the game's own fog is the underwater one: that is used as is.
bool ResolveFogA(bool below, float fogA[3], float colour[3]) {
    DayNight::DNInfo& info = DayNight::g_dnInfo;
    DayNight::CurrentLight light;
    if (below) {
        light = info.m_bands;
    } else {
        if (!DayNight::g_areaLights.m_count || !DayNight::g_areaLights.m_data[0]) {
            return false;
        }
        DayNight::BlendAreaLightColors(&light, DayNight::g_areaLights.m_data[0], 1);
        struct Near { const LightRec* light; float distanceSq; };
        Near lights[32];
        uint32_t count = 0;
        for (uint32_t i = 1; i < DayNight::g_areaLights.m_count && count < 32; i++) {
            const LightRec* rec = DayNight::g_areaLights.m_data[i];
            if (!rec || rec->m_gameFalloffEnd < 3.0f) {
                continue;
            }
            const float dx = info.m_lightRefPos.x - rec->m_gameCoords[0];
            const float dy = info.m_lightRefPos.y - rec->m_gameCoords[1];
            const float dz = info.m_lightRefPos.z - rec->m_gameCoords[2];
            const float distanceSq = dx * dx + dy * dy + dz * dz;
            if (distanceSq < rec->m_gameFalloffEnd * rec->m_gameFalloffEnd) {
                lights[count++] = { rec, distanceSq };
            }
        }
        std::sort(lights, lights + count, [](const Near& a, const Near& b) {
            return a.distanceSq != b.distanceSq ? a.distanceSq > b.distanceSq : a.light->m_gameFalloffStart > b.light->m_gameFalloffStart;
        });
        for (uint32_t i = 0; i < count; i++) {
            DayNight::BlendAreaLight(&light, lights[i].light, 1, 1.0f);
        }
    }
    const float farClip = std::max(info.m_farClip, 50.0f);
    const float end = std::min(light.m_fogEnd, farClip);
    const float start = std::max(light.m_fogStartMul, 0.0f) * end;
    const float span = std::max(end - start, 1.0e-3f);
    fogA[0] = -1.0f / span;
    fogA[1] = end / span;
    fogA[2] = light.m_fogRate * 2.0f;
    const CImVector& c = light.m_skyColors[5];     // LightIntBand 7: fog
    colour[0] = Channel(c.r);
    colour[1] = Channel(c.g);
    colour[2] = Channel(c.b);
    return std::isfinite(fogA[0] + fogA[1] + fogA[2]) && fogA[2] > 0.0f;
}

bool RipplesWanted() {
    return s_style && s_style->m_intValue == 1 && s_ripples && s_ripples->m_intValue != 0;
}

}  // namespace

void RegisterCVars() {
    s_style = CVar::Register("waterStyle", "Water look: 0 = modern (the volumetric fog's water, vfogWater), 1 = Cataclysm (its water surface, with or without the fog)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_ripples = CVar::Register("waterCataRipples", "Cataclysm water (waterStyle 1): ripples and wakes where units move through or jump into water (0 = off)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_reflMode = CVar::Register("waterCataReflection", "Cataclysm water (waterStyle 1) planar reflection, as Cata's reflectionMode: 0 = off (screen-space only), 1 = sky, 2 = + terrain, 3 = + buildings (WMOs)", 1, "3", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_reflScale = CVar::Register("waterCataReflScale", "Cataclysm water planar reflection target size against the world viewport (0.25 - 1)", 1, "0.6", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

bool Arm(Liquid::CInstance** instances, uint32_t count) {
    s_pass.armed = false;
    CGxDevice* device = g_theGxDevicePtr;
    if (!s_style || s_style->m_intValue != 1 || !count || !device || s_failed || !device->Caps().m_rttFormat[GxTex_R32F]) {
        return false;
    }
    bool any = false;
    for (uint32_t i = 0; i < count && !any; i++) {
        any = instances[i]->m_settings && ParamsOf(instances[i]->m_settings);
    }
    if (!any) {
        return false;
    }
    if (!s_callbackAdded) {
        device->AddDeviceRestoredCallback(OnDeviceRestored);
        s_callbackAdded = true;
    }
    if (!EnsureShaders()) {
        s_failed = true;
        return false;
    }
    // The reflection plane for the next frame: terrain water only (a WMO liquid's sphere is its group's, not its level).
    for (uint32_t i = 0; i < count; i++) {
        if (instances[i]->m_settings && instances[i]->m_waveManager == static_cast<Liquid::IWaveManager*>(CChunkLiquid::s_waveManager) && ParamsOf(instances[i]->m_settings)) {
            planepick::Add(instances[i]->m_bounds, CWorldScene::s_activeWorldView);
        }
    }
    s_reflection.planeValid = planepick::Pick(s_reflection.drawn, s_reflection.h, s_reflection.planeH);
    s_pass.vp = VolumetricFog::LiveWorldViewport();
    const VolumetricFog::WorldViewport& vp = s_pass.vp;
    // The copies read the back buffer: no Cata water when the world draws into a target.
    if (vp.target || vp.w < kMinSize || vp.h < kMinSize
        || !Target(s_scene, static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), GxTex_Argb8888, GxTex_Linear, "CataWaterScene")) {
        return false;
    }
    const C2iVector dest = { 0, 0 };
    const C2iVector source = { static_cast<int32_t>(vp.x), static_cast<int32_t>(vp.y) };
    const C2iVector size = { static_cast<int32_t>(vp.w), static_cast<int32_t>(vp.h) };
    if (!GxTexCopy(s_scene, dest, source, size, 0, 0) || !CopyDepth()) {
        return false;
    }
    s_pass.below = World::SceneCamLiquidStatus(nullptr) != 0;
    s_pass.fogValid = ResolveFogA(s_pass.below, s_pass.fogA, s_pass.fogColour);
    s_pass.ripples = {};
    if (RipplesWanted()) {
        // vfog's ripple simulation (Cata's map format: Gr1616F height / previous height, 0.125 yd texels), run here
        // when the Cata water draws; it needs only the camera target and the map from the frame inputs.
        static VolumetricFog::FrameInputs s_in;
        s_in.camTarget[0] = CWorldScene::camTarget.x;
        s_in.camTarget[1] = CWorldScene::camTarget.y;
        s_in.camTarget[2] = CWorldScene::camTarget.z;
        s_in.mapId = World::QueryMapId();
        VolumetricFog::RippleFrame(s_in, 1.0f, 2, FrameTime::s_curTimeMs * 0.001, vp, s_pass.ripples);
    }
    s_pass.armed = true;
    return true;
}

void Done() {
    s_pass.armed = false;
}

bool Bind(Liquid::CMaterialSettings* settings, bool specular, const C3Vector& cameraPos) {
    if (!s_pass.armed) {
        return false;
    }
    const Params* lp = ParamsOf(settings);
    CGxTex* normal = Texture(s_normalTex, "XTextures\\ocean\\newoceanbump.blp");
    CGxTex* foam = Texture(s_foamTex, "XTextures\\foam\\waterchop7bw.blp");
    if (!lp || !normal || !foam) {
        return false;
    }
    CGxDevice* device = g_theGxDevicePtr;

    // The view the material's modelview was built from: its rotation's inverse (the transpose) takes view space back
    // to camera-relative world axes, and the rotation with the projection takes a camera-relative point to clip.
    C44Matrix view;
    GxXformView(view);
    const float scale = 0.06f;
    const float wrap = 2048.0f / scale;     // a whole number of every normal map layer's tiles
    const float seconds = static_cast<float>(OsGetAsyncTimeMs() % 3600000u) * 0.001f;
    const float vc[5][4] = {
        { view.a0, view.b0, view.c0, 0.0f },
        { view.a1, view.b1, view.c1, 0.0f },
        { view.a2, view.b2, view.c2, 0.0f },
        { std::fmod(cameraPos.x, wrap), std::fmod(cameraPos.y, wrap), seconds, scale },
        { lp->p3, lp->p4, lp->p5deg * 3.14159265f / 180.0f, lp->p8 },
    };
    device->ShaderConstantsSet(GxSh_Vertex, 50, &vc[0][0], 5);
    if (VolumetricFog::StockFogReplaced()) {
        // The volumetric fog fogs the water after the liquid pass: no stock fog on it as well.
        const float noFog[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
        device->ShaderConstantsSet(GxSh_Vertex, 4, noFog, 1);
    }

    C44Matrix rotation = view;
    rotation.d0 = rotation.d1 = rotation.d2 = 0.0f;
    const C44Matrix vp = rotation * Liquid::s_vsConstants.m_projection;
    const C4Vector& sun = Liquid::s_vsConstants.m_sunDir;
    const float* d = &Liquid::s_vsConstants.m_sunDiffuse.x;
    const float* s = &Liquid::s_vsConstants.m_sunSpecular.x;
    const float pc[13][4] = {
        { vp.a0, vp.a1, vp.a2, vp.a3 },
        { vp.b0, vp.b1, vp.b2, vp.b3 },
        { vp.c0, vp.c1, vp.c2, vp.c3 },
        { vp.d0, vp.d1, vp.d2, vp.d3 },
        { s_pass.depthTex[0], s_pass.depthTex[1], s_pass.depthTex[2], s_pass.depthTex[3] },
        { s_pass.vp.x, s_pass.vp.y, s_pass.vp.w, s_pass.vp.h },
        { lp->p6, lp->p9, specular ? lp->p10 : 0.0f, lp->p14 },     // the NoSpec material has no highlight
        { s_pass.maxZ, s_pass.fogValid ? 1.0f : 0.0f, 0.0f, 0.0f },
        { s_pass.fogA[0], s_pass.fogA[1], s_pass.fogA[2], lp->p7 * 0.35f },
        { s_pass.fogColour[0], s_pass.fogColour[1], s_pass.fogColour[2], 0.0f },
        { -(sun.x * view.a0 + sun.y * view.a1 + sun.z * view.a2), -(sun.x * view.b0 + sun.y * view.b1 + sun.z * view.b2), -(sun.x * view.c0 + sun.y * view.c1 + sun.z * view.c2), 0.0f },
        { d[0], d[1], d[2], 1.0f },
        { s[0], s[1], s[2], 1.0f },
    };
    device->ShaderConstantsSet(GxSh_Pixel, 16, &pc[0][0], 13);
    // c29 / c30: the ripple window (origin - camera, 1 / extent) and (1 / texels, step fraction, P15; 0 = off).
    const VolumetricFog::RippleShading& rs = s_pass.ripples;
    const float rc[2][4] = {
        { static_cast<float>(static_cast<double>(rs.window[0]) - cameraPos.x), static_cast<float>(static_cast<double>(rs.window[1]) - cameraPos.y), rs.window[2], rs.window[2] },
        { rs.window[3], rs.window[3], rs.shape[0], rs.map ? lp->p15 : 0.0f },
    };
    device->ShaderConstantsSet(GxSh_Pixel, 29, &rc[0][0], 2);
    Bind(kRippleStage, rs.map ? rs.map : s_scene);  // off: any valid texture, its slope is scaled by 0

    Bind(kNormalStage, normal);
    Bind(kFoamStage, foam);
    Bind(kSceneStage, s_scene);
    Bind(kDepthStage, s_depth);
    const bool planar = s_reflection.drawn && !s_pass.below;
    if (planar) {
        // c31: the drawn plane's height against the camera, 1 / its tolerance (2 yd on, fading out to 4 yd).
        const float rc31[4] = { s_reflection.h - cameraPos.z, 0.5f, 0.0f, 0.0f };
        device->ShaderConstantsSet(GxSh_Pixel, 31, rc31, 1);
        Bind(kReflStage, s_refl);
    }
    // The back buffer's alpha is the glow mask: RGB only (the material's RsPop puts it back).
    GxRsSet(GxRs_ColorWrite, 7);
    device->RsSet(GxRs_VertexShader, s_vs);
    device->RsSet(GxRs_PixelShader, s_pass.below ? s_psBelow : planar ? s_psAboveP : s_psAbove);
    return true;
}

// Cata FFXUnderWaterLow (wxl-water's underwater pass, Water.cpp CausticsUnder): with the camera in water, the frame
// times the underwater fog's hue (brightness kept, 30 %) and darkened toward the corners (0.45). One copy of the world
// viewport and one opaque quad over it, only under water.
void OnWorldDone() {
    CGxDevice* device = g_theGxDevicePtr;
    const int32_t liquid = World::SceneCamLiquidStatus(nullptr);
    if (!s_style || s_style->m_intValue != 1 || !liquid || !device || s_failed || !Liquid::s_liquidTypeDB) {
        return;
    }
    const LiquidTypeRec* rec = Liquid::s_liquidTypeDB->GetRecord(liquid);
    if (!rec || static_cast<uint32_t>(rec->m_materialID) != kMaterialWater || !VolumetricFog::Shader(s_psTint, "CataWaterTint")) {
        return;
    }
    if (!s_callbackAdded) {
        device->AddDeviceRestoredCallback(OnDeviceRestored);
        s_callbackAdded = true;
    }
    const VolumetricFog::WorldViewport vp = VolumetricFog::LiveWorldViewport();
    float fogA[3];
    float colour[3];
    if (vp.target || vp.w < kMinSize || vp.h < kMinSize || !ResolveFogA(true, fogA, colour)
        || !Target(s_scene, static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), GxTex_Argb8888, GxTex_Linear, "CataWaterScene")) {
        return;
    }
    const C2iVector dest = { 0, 0 };
    const C2iVector source = { static_cast<int32_t>(vp.x), static_cast<int32_t>(vp.y) };
    const C2iVector size = { static_cast<int32_t>(vp.w), static_cast<int32_t>(vp.h) };
    if (!GxTexCopy(s_scene, dest, source, size, 0, 0)) {
        return;
    }
    const float brightest = std::max(std::max(colour[0], colour[1]), std::max(colour[2], 1.0e-3f));
    float tint[4];
    for (int i = 0; i < 3; i++) {
        tint[i] = 1.0f + (colour[i] / brightest - 1.0f) * 0.3f;
    }
    tint[3] = 0.45f;
    static VolumetricFog::SavedPassState s_saved;
    VolumetricFog::BeginPasses(s_saved);
    VolumetricFog::SetPs(0, tint, 1);
    Bind(0, s_scene);
    device->RsSet(GxRs_PixelShader, s_psTint);
    VolumetricFog::DrawQuad(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), vp.minX, vp.maxX, vp.minY, vp.maxY);
    VolumetricFog::EndPasses(s_saved, vp);
}

// Cata's p = 1 reflection (W1 section 4): the sky, then the terrain and the WMO groups this frame's culling found for
// the main view, re-drawn from the camera mirrored through the water plane into a waterCataReflScale target. The
// mirror is a reflection R (z -> 2h - z) before the view; the projection's x is negated as well, so the winding comes
// out as the main view's and no cull state changes (the shader reads the target mirrored left to right, as wxl-water).
// The terrain and WMOs get an oblique near plane on the water, so nothing under it shows; the sky does not need it.
// Only the stock chunk / WMO / sky draws run here: none of the vfog, sun shadow, AO, leaves, grass, tree sway or
// occlusion hooks (WorldFrame's OnOpaqueDone / OnSolidDone / OnLiquidDone / OnWorldDone, the M2 scene, the liquid
// pass, OcclusionCull::IssueQueries) is reached, so none of them sees the mirrored camera.
void RenderReflection() {
    s_reflection.drawn = false;
    CGxDevice* device = g_theGxDevicePtr;
    const int32_t mode = s_reflMode ? s_reflMode->m_intValue : 0;
    const bool planeValid = s_reflection.planeValid;
    s_reflection.planeValid = false;    // this frame's water pass picks the next one
    if (!s_style || s_style->m_intValue != 1 || mode <= 0 || !planeValid || !device || s_failed
        || CWorldScene::s_viewerMapObjDef || CWorldScene::s_unk_ADF570.m_maxViewDepth < 0.0f) {
        return;     // off, no water plane last frame, or indoors
    }
    const float hz = s_reflection.planeH - CWorldScene::s_activeWorldView.z;  // the plane in the view's input space
    static const bool s_obliqueOk = ObliqueSelfTest();
    if (hz > -0.1f || !s_obliqueOk || !s_psAboveP || !s_psAboveP->Valid()) {
        return;     // camera at or under the plane
    }
    const VolumetricFog::WorldViewport vp = VolumetricFog::LiveWorldViewport();
    const float scale = std::min(std::max(s_reflScale ? s_reflScale->m_floatValue : 0.6f, 0.25f), 1.0f);
    const uint32_t w = static_cast<uint32_t>(std::max(vp.w * scale, 1.0f));
    const uint32_t h = static_cast<uint32_t>(std::max(vp.h * scale, 1.0f));
    if (vp.target || vp.w < kMinSize || vp.h < kMinSize || !Target(s_refl, w, h, GxTex_Argb8888, GxTex_Linear, "CataWaterReflection")) {
        return;
    }

    CGxTex* savedColor = nullptr;
    CGxTex* savedDepth = nullptr;
    device->DeviceGetRenderTarget(GxBuffers_Color, savedColor);
    device->DeviceGetRenderTarget(GxBuffers_Depth, savedDepth);
    C44Matrix savedView;
    GxXformView(savedView);
    const C44Matrix savedProj = device->m_projection;
    const C44Matrix savedNative = device->m_projNative;
    device->RsPush();
    device->XformPush(GxXform_World);

    // A colour target alone: every backend pairs it with a depth buffer of its size.
    GxRenderTargetSet(GxBuffers_Depth, nullptr, 0);
    GxRenderTargetSet(GxBuffers_Color, s_refl, 0);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, vp.minZ, vp.maxZ);
    GxSceneClear(GxBufferBit_Color | GxBufferBit_Depth, DayNight::GetInfo()->m_fog.m_color);

    C44Matrix mirror;
    mirror.c2 = -1.0f;
    mirror.d2 = 2.0f * hz;
    const C44Matrix view = mirror * savedView;
    C44Matrix proj = savedProj;
    proj.a0 = -proj.a0;
    proj.b0 = -proj.b0;
    proj.c0 = -proj.c0;
    proj.d0 = -proj.d0;
    GxXformSetView(view);
    GxXformSetProjection(proj);
    CShaderEffect::UpdateProjMatrix();
    s_reflection.reflecting = true;
    DayNight::RenderSky(&CWorldScene::s_unk_ADF570);

    const float plane[4] = { 0.0f, 0.0f, 1.0f, -hz };
    C44Matrix oblique;
    float s = 0.0f;
    if (mode >= 2 && BuildOblique(device->m_projNative, view, plane, oblique, s)) {
        GxSceneClear(GxBufferBit_Depth, DayNight::GetInfo()->m_fog.m_color);
        device->m_projNative = oblique;
        device->m_xforms[GxXform_Projection].m_dirty = 1;
        CShaderEffect::UpdateProjMatrix();
        CWorldScene::RenderChunks();
        if (mode >= 3) {
            CWorldScene::RenderMapObjDefGroups();
        }
    }
    s_reflection.reflecting = false;

    GxRenderTargetSet(GxBuffers_Color, savedColor, 0);
    GxRenderTargetSet(GxBuffers_Depth, savedDepth, 0);
    GxXformSetViewport(vp.minX, vp.maxX, vp.minY, vp.maxY, vp.minZ, vp.maxZ);
    GxXformSetView(savedView);
    GxXformSetProjection(savedProj);
    device->m_projNative = savedNative;
    device->m_xforms[GxXform_Projection].m_dirty = 1;
    CShaderEffect::UpdateProjMatrix();
    GxXformPop(GxXform_World);
    device->RsPop();
    s_reflection.drawn = true;
    s_reflection.h = s_reflection.planeH;
}

bool Reflecting() {
    return s_reflection.reflecting;
}

bool DrawsRipples() {
    return RipplesWanted();
}

void Shutdown() {
    CGxDevice* device = g_theGxDevicePtr;
    if (device) {
        DropTargets();
        for (CGxShader** slot : { &s_vs, &s_psAbove, &s_psBelow, &s_psAboveP, &s_depthCopy, &s_psTint }) {
            if (*slot) {
                device->ShaderDestroy(*slot);
                *slot = nullptr;
            }
        }
        if (s_callbackAdded) {
            device->RemoveDeviceRestoredCallback(OnDeviceRestored);
        }
    }
    s_callbackAdded = false;
    s_failed = false;
    s_pass = {};
    s_reflection = {};
}

}  // namespace CataWater
