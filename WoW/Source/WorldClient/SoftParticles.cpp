// SOFTPARTICLES: not in 12340. See SoftParticles.h. The stock particle vertex shaders stay; for a soft draw the
// pixel shader becomes SoftParticle0/1 (custom.MPQ, Tools/CustomMpq/ShaderSrc/SoftParticle.hlsl) and the vertex
// fog row c30 is set so FOG carries the view depth (1 - z / kFogDepth); the shader redoes the stock fog from the
// real c30. The scene depth is DeviceCopySceneDepth at the start of the pass (deduplicated by the backends when
// nothing wrote depth since the last copy: shadowMask's before pass 2, vfog's after the liquid before pass 1).
#include "WoW/Source/WorldClient/SoftParticles.h"

#include <cstring>

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/ParticleSystem2.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"

namespace SoftParticles {

int32_t s_enabled = 1;
float s_distance = 1.0f;

namespace {

constexpr float kFogDepth = 16384.0f;   // yards at FOG = 0; view depth precision ~0.002 yd
CGxShader* s_ps[2];                     // [0] Combiners_Mod:0, [1] Combiners_Mod:8 (texkill on c2.w)
bool s_failed;
CGxTex* s_depth;
float s_decode[4];                      // c42: 1 / w, 1 / h, depth at infinite view z, depth per inverse view z
float s_deepest;                        // the world viewport's maxZ: deeper is the horizon band or sky
float s_savedFog[4];
bool s_applied;

bool Shaders() {
    if (s_failed) return false;
    if (!s_ps[0]) {
        g_theGxDevicePtr->ShaderCreate(&s_ps[0], GxSh_Pixel, "Shaders\\Pixel", "SoftParticle0", 1);
        g_theGxDevicePtr->ShaderCreate(&s_ps[1], GxSh_Pixel, "Shaders\\Pixel", "SoftParticle1", 1);
    }
    if (!s_ps[0] || !s_ps[0]->Valid() || !s_ps[1] || !s_ps[1]->Valid()) {
        s_failed = true;   // no SoftParticle shaders in custom.MPQ: particles stay hard
        return false;
    }
    return true;
}

// The plugin's SoftMaterial, keyed by the draw's Gx blend (M2 blend 2..6 -> Gx 2, 10, 3, 4, 5 in passes 1 and 2).
void Draw(bool before) {
    if (!before) {
        if (s_applied) {
            s_applied = false;
            GxShaderConstantsSet(GxSh_Vertex, 30, s_savedFog, 1);
            g_theGxDevicePtr->RsSet(GxRs_Texture12, static_cast<CGxTex*>(nullptr));
        }
        return;
    }
    if (CShaderEffect::s_shadowValue) {
        return;   // a shadowed permutation (Combiners_Mod 1..3) keeps the stock shader and stays hard
    }
    float mode[4] = { 1.0f / s_distance, 0.0f, 0.0f, 0.0f };   // (1 / fade yd, rgb fade, alpha fade)
    float neutral[4] = { 0.0f, 0.0f, 0.0f, kFogDepth };
    switch (g_theGxDevicePtr->m_appRenderStates[GxRs_BlendingMode].m_value.m_data.i[0]) {
        case GxBlend_Alpha:       // SA / ISA
        case GxBlend_Add:         // SA / ONE
            mode[2] = 1.0f; break;
        case GxBlend_NoAlphaAdd:  // ONE / ONE: the colour goes to 0
            mode[1] = 1.0f; break;
        case GxBlend_Mod:         // DC / ZERO: the colour goes to 1
            mode[1] = 1.0f; neutral[0] = neutral[1] = neutral[2] = 1.0f; break;
        case GxBlend_Mod2x:       // DC / SC: the colour goes to 0.5
            mode[1] = 1.0f; neutral[0] = neutral[1] = neutral[2] = 0.5f; break;
        default:                  // opaque, alpha-keyed: hard
            return;
    }
    std::memcpy(s_savedFog, &CGxDevice::s_shadowConstants[1].constants[30 * 4], sizeof(s_savedFog));   // vertex c30
    const float fog[4] = { s_savedFog[0], s_savedFog[1], s_savedFog[2], s_deepest };
    const float encode[4] = { -1.0f / kFogDepth, 1.0f, 1.0f, 0.0f };
    GxShaderConstantsSet(GxSh_Pixel, 40, mode, 1);
    GxShaderConstantsSet(GxSh_Pixel, 41, neutral, 1);
    GxShaderConstantsSet(GxSh_Pixel, 42, s_decode, 1);
    GxShaderConstantsSet(GxSh_Pixel, 43, fog, 1);
    GxShaderConstantsSet(GxSh_Vertex, 30, encode, 1);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_ps[(CShaderEffect::SelectShadowShader() & 8) ? 1 : 0]);
    g_theGxDevicePtr->RsSet(GxRs_Texture12, s_depth);
    s_applied = true;
}

} // namespace

void BeginPass() {
    const bool want = s_enabled && Shaders();
    CGxDevice::SetDepthUser(CGxDevice::DepthUser_SoftParticles, want);   // LEGION SetDepthUser
    g_softParticleDraw = nullptr;
    CGxDevice* device = g_theGxDevicePtr;
    // D3D9 without MSAA samples the live INTZ depth-stencil the particles also test against: no soft particles there.
    // ponytail: an R32F copy (VFogDepthCopy) would cover it, add if D3D9 ever matters for this.
    if (!want || !device->DeviceSceneDepthIsCopy()) {
        return;
    }
    CGxTex* depth = device->DeviceCopySceneDepth();
    if (!depth) {
        return;
    }
    CGxTex* target = nullptr;
    device->DeviceGetRenderTarget(GxBuffers_Color, target);
    const CRect& window = device->DeviceCurWindow();
    const float w = target ? static_cast<float>(target->m_width) : window.maxX;
    const float h = target ? static_cast<float>(target->m_height) : window.maxY;
    if (w != static_cast<float>(depth->m_width) || h != static_cast<float>(depth->m_height)) {
        return;   // not the frame the depth is of (the plugin's size check)
    }
    float minX, maxX, minY, maxY, minZ, maxZ;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);
    const float* proj = reinterpret_cast<const float*>(&device->m_projection);
    const float extent = maxZ - minZ;
    // viewZ = c42.w / (d - c42.z) inside [minZ, maxZ], as VolumetricFogRenderer's c3 (coa MapWorldDepth).
    s_decode[0] = 1.0f / w;
    s_decode[1] = 1.0f / h;
    s_decode[2] = minZ + extent * (1.0f + proj[10]) * 0.5f;
    s_decode[3] = extent * proj[14] * 0.5f;
    s_deepest = maxZ;
    s_depth = depth;
    g_softParticleDraw = &Draw;
}

void EndPass() {
    g_softParticleDraw = nullptr;
    s_depth = nullptr;
}

} // namespace SoftParticles
