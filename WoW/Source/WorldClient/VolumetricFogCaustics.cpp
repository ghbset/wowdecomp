// Caustics and the underwater screen tint: wxl-water (branch caustics @ 17cf9af) Water.cpp CausticsUnder and the
// "3a caustics" term of its surface shader (Shaders.hpp Caustic / SunVis / kCausticUnder), as two full-screen
// passes (VFogCausticsAbove / VFogCausticsUnder, ShaderSrc/VFogCaustics.hlsl). docs/BACKLOG_PORT.md "Caustics".
//   above water: WaterFlushDone, over the shaded modern water, on the bed its pre-water depth copy holds;
//   under water: OnWorldDone, on what the scene depth shows below the surface, plus Cata's FFXUnderWaterLow tint.
// The sun shadow is the engine's bands 1 and 2 (vf_shadow.hlsli, as the shadowed fog reads them). There is no
// planar water reflection pass in this client (the modern water reflects in screen space), so nothing to skip.
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/World.h"

namespace VolumetricFog {
namespace {

constexpr uint32_t kCausticRegister = 9;        // VFogCaustics.hlsl c9-c12
constexpr uint32_t kShadowRegister = 56;        // vf_shadow.hlsli c56-c62
constexpr uint32_t kHorizonRegister = 99;       // vf_common cHorizonBand
constexpr int kShadowStage = 5;                 // vf_shadow.hlsli s5, s6

CVar* s_caustics = nullptr;
CVar* s_tint = nullptr;
CGxShader* s_above = nullptr;
CGxShader* s_under = nullptr;

float Knob(CVar* var, float high) {
    return var && std::isfinite(var->m_floatValue) ? std::clamp(var->m_floatValue, 0.0f, high) : 1.0f;
}

// c9-c12. False: no caustic (moon, sun under the horizon, camera indoors, strength 0); rows[1].rgb is then zero.
bool CausticRows(const FrameInputs& in, float surface, float rows[4][4]) {
    const float k = Knob(s_caustics, 3.0f);
    const bool on = k > 0.0f && !in.lightIsMoon && in.toLight[2] > 0.0f && !(in.interiorBlend > 0.0f);
    const float span = in.fogEnd - in.fogStart;
    const float c[4][4] = {
        { in.toLight[0], in.toLight[1], in.toLight[2], surface },
        { on ? ((in.directColor >> 16) & 0xFF) / 255.0f * k : 0.0f, on ? ((in.directColor >> 8) & 0xFF) / 255.0f * k : 0.0f,
          on ? (in.directColor & 0xFF) / 255.0f * k : 0.0f, (FrameTime::s_curTimeMs % 3600000u) * 0.001f },
        { static_cast<float>(std::fmod(static_cast<double>(in.camPos[0]), 3.0)), static_cast<float>(std::fmod(static_cast<double>(in.camPos[1]), 3.0)),
          in.fogEnd, span > 0.01f ? 1.0f / span : 0.0f },
        { 1.0f, 1.0f, 1.0f, 0.0f },
    };
    std::memcpy(rows, c, sizeof(c));
    return on;
}

void Draw(CGxShader* shader, const float common[9][4], const float rows[4][4], const float viewToWorld[16], const WorldViewport& vp) {
    float shadow[7][4];
    CGxTex* maps[2];
    SunShadowRegisters(true, 0.0f, viewToWorld, shadow, maps);
    SetPs(0, &common[0][0], 9);
    SetPs(kCausticRegister, &rows[0][0], 4);
    SetPs(kShadowRegister, &shadow[0][0], 7);
    Bind(kShadowStage, maps[0]);
    Bind(kShadowStage + 1, maps[1]);
    GxRsSet(GxRs_BlendingMode, GxBlend_Mod2x);   // 2 src dest: dest (1 + caustic) tint
    GxRsSet(GxRs_ColorWrite, 7);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, shader);
    DrawQuad(static_cast<uint32_t>(vp.w), static_cast<uint32_t>(vp.h), vp.minX, vp.maxX, vp.minY, vp.maxY);
    Bind(kShadowStage, nullptr);
    Bind(kShadowStage + 1, nullptr);
}

}  // namespace

void RegisterCausticCVars() {
    // wxl-water caustics / uwtint, default 1 as there.
    s_caustics = CVar::Register("waterCaustics", "Sunlight rippling on the ground under water, seen from above and from below, 0..3 (0 = off); none at night or in sun shadow", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_tint = CVar::Register("waterUnderwaterTint", "Underwater screen tint while the camera is submerged (Cata): the zone's underwater colour and darker corners, 0..2 (0 = off)", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

bool CausticsUnderWanted() {
    return Knob(s_caustics, 3.0f) > 0.0f || Knob(s_tint, 2.0f) > 0.0f;
}

void CausticsAbove(const FrameInputs& in, const float common[9][4], const WorldViewport& vp, CGxTex* bed, CGxTex* water, bool stencil) {
    float rows[4][4];
    if (!bed || !water || !CausticRows(in, 0.0f, rows) || !Shader(s_above, "VFogCausticsAbove")) {
        return;
    }
    static SavedPassState s_saved;
    BeginPasses(s_saved);
    if (stencil) {
        SetStencil(GxStencilCmp_NotEqual, 0, 3, 0, GxStencilOp_Keep);   // classes 1-3 (stencil & 3 != 0): not interior (4), not dry (0)
    }
    Bind(0, bed);
    Bind(1, water);
    Draw(s_above, common, rows, &common[4][0], vp);
    EndPasses(s_saved, vp);
}

void CausticsUnder(const FrameInputs& in) {
    float surface = 0.0f;
    World::SceneCamLiquidStatus(&surface);   // liquid level - eye z
    if (!(surface > 0.0f) || !Shader(s_under, "VFogCausticsUnder")) {
        return;
    }
    float rows[4][4];
    const bool caustic = CausticRows(in, surface, rows);
    const float tint = Knob(s_tint, 2.0f);
    if (!caustic && !(tint > 0.0f)) {
        return;
    }
    // Cata FFXUnderWaterLow: the underwater fog's hue (brightness kept) and darker corners.
    const float fog[3] = { ((in.fogColor >> 16) & 0xFF) / 255.0f, ((in.fogColor >> 8) & 0xFF) / 255.0f, (in.fogColor & 0xFF) / 255.0f };
    const float peak = std::max(std::max(fog[0], fog[1]), std::max(fog[2], 1e-3f));
    for (int i = 0; i < 3; i++) {
        rows[3][i] = 1.0f + (fog[i] / peak - 1.0f) * 0.3f * tint;
    }
    rows[3][3] = std::min(0.45f * tint, 0.9f);

    const WorldViewport vp = LiveWorldViewport();
    CGxDevice* device = g_theGxDevicePtr;
    CGxTex* depth = vp.target ? nullptr : device->DeviceCopySceneDepth();
    float common[9][4];
    if (!depth || vp.w < 1.0f || vp.h < 1.0f || vp.x + vp.w > depth->m_width || vp.y + vp.h > depth->m_height || !CommonRows(in, vp, depth, common)) {
        return;
    }
    static SavedPassState s_saved;
    BeginPasses(s_saved);
    SetPs(kHorizonRegister, in.horizonBand, 1);
    Bind(0, depth);
    device->DeviceSceneDepthBound(false);   // D3D9: s0 is the INTZ depth-stencil the back buffer uses
    Draw(s_under, common, rows, &common[4][0], vp);
    device->DeviceSceneDepthBound(true);
    EndPasses(s_saved, vp);
}

}  // namespace VolumetricFog
