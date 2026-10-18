// Cata water (waterStyle 1): see CataWater.h and docs/BACKLOG_PORT.md "Cata water". Not in the 3.3.5a binary.
#include "WoW/Source/WorldClient/CataWater.h"
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
#include "Engine/Source/Tempest/C2iVector.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"
#include "WoW/Source/DB/WowClientDB.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
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
constexpr int kSceneStage = 12;
constexpr int kDepthStage = 13;

CVar* s_style;
CGxShader* s_vs;
CGxShader* s_psAbove;
CGxShader* s_psBelow;
CGxShader* s_depthCopy;     // VFogDepthCopy: the raw scene depth into R32F
CTexture* s_normalTex;
CTexture* s_foamTex;
CGxTex* s_scene;
CGxTex* s_depth;
bool s_callbackAdded;
bool s_failed;              // a shader is missing: stay off until the device is restored

struct Pass {
    bool armed;
    bool below;
    VolumetricFog::WorldViewport vp;
    float depthTex[4];      // c20
    float maxZ;
};
Pass s_pass;

void DropTargets() {
    VolumetricFog::DestroyTex(s_scene);
    VolumetricFog::DestroyTex(s_depth);
}

void __fastcall OnDeviceRestored() {
    DropTargets();
    VolumetricFog::DropInvalidShaders(&s_vs, 1);
    VolumetricFog::DropInvalidShaders(&s_psAbove, 1);
    VolumetricFog::DropInvalidShaders(&s_psBelow, 1);
    VolumetricFog::DropInvalidShaders(&s_depthCopy, 1);
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

}  // namespace

void RegisterCVars() {
    s_style = CVar::Register("waterStyle", "Water look: 0 = modern (the volumetric fog's water, vfogWater), 1 = Cataclysm (its water surface, with or without the fog)", 1, "0", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
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
        { s_pass.maxZ, 0.0f, 0.0f, 0.0f },                          // piece C: thickness fog strength in y
        { 0.0f, 0.0f, 1.0f, lp->p7 * 0.35f },                       // piece C: the zone's fog A in xyz
        { 0.0f, 0.0f, 0.0f, 0.0f },                                 // piece C: fog A colour
        { -(sun.x * view.a0 + sun.y * view.a1 + sun.z * view.a2), -(sun.x * view.b0 + sun.y * view.b1 + sun.z * view.b2), -(sun.x * view.c0 + sun.y * view.c1 + sun.z * view.c2), 0.0f },
        { d[0], d[1], d[2], 1.0f },
        { s[0], s[1], s[2], 1.0f },
    };
    device->ShaderConstantsSet(GxSh_Pixel, 16, &pc[0][0], 13);

    Bind(kNormalStage, normal);
    Bind(kFoamStage, foam);
    Bind(kSceneStage, s_scene);
    Bind(kDepthStage, s_depth);
    // The back buffer's alpha is the glow mask: RGB only (the material's RsPop puts it back).
    GxRsSet(GxRs_ColorWrite, 7);
    device->RsSet(GxRs_VertexShader, s_vs);
    device->RsSet(GxRs_PixelShader, s_pass.below ? s_psBelow : s_psAbove);
    return true;
}

void Shutdown() {
    CGxDevice* device = g_theGxDevicePtr;
    if (device) {
        DropTargets();
        for (CGxShader** slot : { &s_vs, &s_psAbove, &s_psBelow, &s_depthCopy }) {
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
}

}  // namespace CataWater
