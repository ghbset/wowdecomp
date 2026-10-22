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
    GxLighting_PerPixel = 0x1,        // M2, WMO and terrain lit per pixel with their stock formulas (lit shader pack)
    GxLighting_Dynamic = 0x2,         // the point lights in GxLightingGpu
    GxLighting_Specular = 0x4,        // WMO specular shaders per pixel, a highlight on sphere-mapped M2s
    GxLighting_Hemisphere = 0x8,      // the ambient tinted by the sky above and the ground below
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

    // the lit shaders (GxLightingGpu)
    float m_lightWrap = 0.25f;
    float m_dynamicIntensity = 1.0f;
    float m_specularM2 = 0.3f;
    float m_specularWmo = 1.0f;
    C3Vector m_skyColor;             // 0 to 1, gamma as the engine stores them
    C3Vector m_groundColor;
    float m_hemisphere = 0.0f;

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
    uint32_t m_debug = 0;            // 1 the SSAO term alone, 2 lit surfaces by kind, 3 dynamic light alone, 4 and 5 the SSAO pass's inputs
};

// CUSTOM (Badgermilk0 port, the shader half): what the lit world shaders read. The translated shader pack
// (shader-xlate sm3hlsl.py for D3D11, Tools/VulkanShaders/bls/emitter.py for Vulkan) lets the M2, WMO and terrain
// vertex shaders hand their view-space surface to the pixel shader, which lights it again per pixel and adds these
// lights. Layout is std140 and HLSL cbuffer alike (every member a 16-byte row):
//   D3D11: cbuffer GxLighting : register(b3) in VS and PS; the vertex constants (VS cb0) also at PS b4
//   Vulkan: uniform block set 1 binding 1 (the pixel module reads the vertex constants from regs.v[])
// The game (WoW/Source/Ui/WorldLighting.cpp) fills g_gxLightingGpu at SetupView and zeroes m_flags[0] at EndWorld,
// bumping g_gxLightingGpuSerial each time; a device uploads it when the serial changes. m_flags[0] == 0 (the default,
// and every frame with the shader CVars off) = every lit shader takes its stock path: the frame is the stock one.
// Per draw, the device tells the pixel shader whether the bound vertex shader writes the surface (CGxShader::
// m_litSurface): D3D11 cb1 dev_alphaRef.y, Vulkan regs.misc.y, 1 or 0. A lit pixel shader relights only when it is 1.
constexpr uint32_t kGxLightingMaxLights = 32;

struct GxLightingLight {
    C3Vector m_position;  // world, render-relative (as the view matrix)
    float m_radius;       // no light from here on
    float m_start;        // full light up to here
    C3Vector m_color;     // linear, intensity included
    float m_flicker;      // 0 to 1
    float m_specular;     // 0 to 1
};

struct GxLightingGpuLight {
    float m_pos[3];       // view space
    float m_radius;
    float m_color[3];     // linear
    float m_start;
    float m_flicker;
    float m_seed;         // 0 to 2 pi, from the world position
    float m_specular;
    float m_pad;
};

struct GxLightingGpu {
    uint32_t m_flags[4];      // x GxLighting_PerPixel..Hemisphere (0 = off), y lights, z 0, w lightingDebug (2, 3)
    float m_view[16];         // row-major (row vector * matrix): render-relative world -> view, for the terrain
    float m_viewInv[16];
    float m_skyTint[4];       // rgb sky colour at luminance 1, a hemisphere strength
    float m_groundTint[4];    // rgb ground colour at luminance 1
    float m_upView[4];        // xyz world up in view space, w time in seconds (flicker)
    float m_lightParams[4];   // x wrap, y dynamic intensity, z M2 specular, w WMO specular
    GxLightingGpuLight m_lights[kGxLightingMaxLights];
};

extern GxLightingGpu g_gxLightingGpu;
extern uint32_t g_gxLightingGpuSerial;

class IGxLighting {
    public:
    virtual void BeginWorld(const GxLightingParams& params) = 0;
    virtual void SetupView(const GxLightingParams& params) = 0;
    virtual void OpaqueDone() = 0;
    virtual void EndWorld() = 0;
};

extern IGxLighting* g_gxLighting;

#endif
