#ifndef ENGINE_SOURCE_GX_CGXDEVICED3D11_D3D11LIGHTING_H
#define ENGINE_SOURCE_GX_CGXDEVICED3D11_D3D11LIGHTING_H

// CUSTOM (Badgermilk0 "modern lighting" port, post passes): GxLighting.h on the D3D11 device. SSAO and height fog /
// scattering at the end of the opaque pass; with hdr the world draws into an R16G16B16A16_FLOAT scene that stands in
// for the back buffer (its texture, view, resolve and SRV are swapped into the device's back-buffer members, so
// every frame copy, the normal target and IBindRenderTargets follow it), and at the end of the world bloom and the
// tonemap write it into the real back buffer. Shaders: Lighting/Lighting.hlsl -> LightingShaders.h.
//
// No new device member (the stock-host layout, README): the device calls these hooks instead.

class CGxDeviceD3d11;

void D3d11LightingCreate(CGxDeviceD3d11* device);   // sets g_gxLighting
void D3d11LightingDestroy();                        // device going away: every D3D object, g_gxLighting null
void D3d11LightingReleaseTargets();                 // the back buffer is about to be released (resize, recreate)

#endif
