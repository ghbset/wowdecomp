#ifndef ENGINE_SOURCE_GX_GXLIGHTING_H
#define ENGINE_SOURCE_GX_GXLIGHTING_H

// CUSTOM (Badgermilk0 "modern lighting" port, post passes only): not in the 3.3.5a binary. The game
// (WoW/Source/Ui/WorldLighting.cpp) describes each world frame; the D3D11 and Vulkan devices draw it and set
// g_gxLighting while they are up. D3D9 leaves it null and nothing changes.
//
// The calls bracket the world pass in CGWorldFrame::OnWorldRender:
//   BeginWorld   before CWorld::Render: the world draws into the HDR scene target (hdr on, D3D11)
//   SetupView    in CWorldScene::Render once the camera is set: the view and projection the passes use
//   OpaqueDone   after the opaque models and the grass, before liquids and everything transparent: SSAO and fog
//   EndWorld     before FFX::Finish: bloom and the tonemap into the frame

#include <cstdint>
#include "Engine/Source/Tempest/C3Vector.h"

enum EGxLightingFeature : uint32_t {
    GxLighting_Ssao = 0x10,
    GxLighting_HeightFog = 0x20,
    GxLighting_Scattering = 0x40,     // the fog tinted toward the sun's colour around the sun
    GxLighting_Hdr = 0x80,            // the world into a 16-bit float target, tonemapped
    GxLighting_Bloom = 0x100,         // needs Hdr
};

struct GxLightingParams {
    uint32_t m_features = 0;

    // HDR and tonemap
    float m_exposure = 1.0f;
    uint32_t m_tonemap = 1;          // 0 clamp (stock), 1 neutral (stock below a shoulder), 2 ACES
    float m_bloomIntensity = 0.0f;
    float m_bloomThreshold = 1.0f;   // linear

    // SSAO
    float m_ssaoIntensity = 0.0f;
    float m_ssaoRadius = 1.5f;       // yards
    float m_ssaoDistance = 200.0f;   // fades out to here

    // fog, as the engine has it this frame (DayNight)
    C3Vector m_fogColor;
    float m_fogStart = 0.0f;
    float m_fogEnd = 0.0f;
    float m_fogDensity = 1.0f;
    C3Vector m_sunColor;
    C3Vector m_sunDirection;         // world, the way the light travels
    float m_scattering = 0.0f;
    float m_heightFogDensity = 0.0f; // per yard at the base height
    float m_heightFogFalloff = 0.05f;
    float m_heightFogBase = 0.0f;    // world z (render-relative once SetupView has it)
    float m_heightFogMax = 0.9f;
    C3Vector m_cameraPosition;       // world (render-relative once SetupView has it)
    float m_time = 0.0f;             // seconds
    uint32_t m_debug = 0;            // 1 the SSAO term alone, 4 and 5 the SSAO pass's inputs
};

class IGxLighting {
    public:
    virtual void BeginWorld(const GxLightingParams& params) = 0;
    virtual void SetupView(const GxLightingParams& params) = 0;
    virtual void OpaqueDone() = 0;
    virtual void EndWorld() = 0;
};

extern IGxLighting* g_gxLighting;

#endif
