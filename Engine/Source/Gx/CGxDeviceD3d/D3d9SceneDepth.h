#ifndef ENGINE_GX_CGXDEVICED3D_D3D9SCENEDEPTH_H
#define ENGINE_GX_CGXDEVICED3D_D3D9SCENEDEPTH_H

// VFOG: INTZ/RESZ scene depth shared by CGxDeviceD3d and CGxDeviceD3d9Ex (same members, so one template each;
// both classes forward their members here). Not in 12340 (coa-vfog d3d9_wrap.cpp CreateDepth, msaa_depth.cpp).

#include "Engine/Source/Gx/CGxTex.h"

namespace d3d9scenedepth {

// Called from IStateSetD3DDefaults, so after device create and after every Reset (WM_SIZE, lost device), just before
// m_defDepthSurface is read back: with an INTZ texture bound, m_defDepthSurface is its surface and every place that
// rebinds the main depth (DeviceSetRenderTarget, DeviceSave/RestoreDepthSurface) keeps it. No INTZ: the auto
// depth-stencil stays bound.
// MSAA back buffer: the multisampled auto depth-stencil stays bound and INTZ is only a copy target, filled by the
// driver's RESZ resolve (AMD, Intel, DXVK; sample 0) in CopySceneDepth. No RESZ: nullptr.
// ponytail: the auto depth-stencil is still allocated (unused) while INTZ is bound; turn EnableAutoDepthStencil off in
// ISetPresentParms if the VRAM matters.
template <class Dev>
void Create(Dev* d) {
    const D3DFORMAT intz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    const D3DFORMAT resz = static_cast<D3DFORMAT>(MAKEFOURCC('R', 'E', 'S', 'Z'));
    D3DSURFACE_DESC desc;

    if (d->m_sceneDepthTexture || !d->m_defColorSurface || FAILED(d->m_defColorSurface->GetDesc(&desc))) {
        return;
    }

    bool msaa = desc.MultiSampleType != D3DMULTISAMPLE_NONE;

    if (FAILED(d->m_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d->m_devAdapterFormat, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, intz))
        || (msaa && FAILED(d->m_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d->m_devAdapterFormat, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, resz)))) {
        return;
    }

    if (FAILED(d->m_d3dDevice->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT, &d->m_sceneDepthTexture, nullptr))) {
        d->m_sceneDepthTexture = nullptr;
        return;
    }

    if (msaa) {
        d->m_sceneDepthResz = true;
        return;
    }

    LPDIRECT3DSURFACE9 surface = nullptr;

    if (FAILED(d->m_sceneDepthTexture->GetSurfaceLevel(0, &surface)) || FAILED(d->m_d3dDevice->SetDepthStencilSurface(surface))) {
        if (surface) {
            surface->Release();
        }

        d->IReleaseSceneDepth();
        return;
    }

    // The device keeps its own reference while it is bound; m_defDepthSurface takes one more in GetDepthStencilSurface.
    surface->Release();
}

// D3DPOOL_DEFAULT, so it goes with the other default-pool objects before a Reset (IReleaseD3dResources). That
// includes the INTZ surface reference SceneDepthBound(false) holds, or the Reset fails for good.
template <class Dev>
void Release(Dev* d) {
    d->m_sceneDepthResz = false;
    if (d->m_sceneDepthTex) {
        d->m_sceneDepthTex->m_apiSpecificData = nullptr;
    }

    if (d->m_sceneDepthUnbound) {
        d->m_sceneDepthUnbound->Release();
        d->m_sceneDepthUnbound = nullptr;
    }

    if (d->m_sceneDepthTexture) {
        d->m_sceneDepthTexture->Release();
        d->m_sceneDepthTexture = nullptr;
    }
}

// Reading a texture that is the bound depth-stencil is undefined in D3D9 even with depth test and write off:
// false unbinds INTZ before a draw that samples it, true puts it back.
template <class Dev>
void Bound(Dev* d, bool bound) {
    if (!d->m_d3dDevice) {
        return;
    }
    if (bound) {
        if (d->m_sceneDepthUnbound) {
            d->m_d3dDevice->SetDepthStencilSurface(d->m_sceneDepthUnbound);
            d->m_sceneDepthUnbound->Release();
            d->m_sceneDepthUnbound = nullptr;
        }
        return;
    }
    if (d->m_sceneDepthUnbound || !d->m_sceneDepthTexture) {
        return;
    }
    LPDIRECT3DSURFACE9 bound0 = nullptr;
    LPDIRECT3DSURFACE9 intz = nullptr;
    if (SUCCEEDED(d->m_d3dDevice->GetDepthStencilSurface(&bound0)) && bound0 && SUCCEEDED(d->m_sceneDepthTexture->GetSurfaceLevel(0, &intz))) {
        if (bound0 == intz) {
            d->m_d3dDevice->SetDepthStencilSurface(nullptr);
            d->m_sceneDepthUnbound = bound0;     // keeps the reference until it goes back
            bound0 = nullptr;
        }
    }
    if (intz) {
        intz->Release();
    }
    if (bound0) {
        bound0->Release();
    }
}

// The bound multisampled depth-stencil resolved into INTZ by the driver (sample 0). INTZ on stage 0, one point drawn
// so the binding reaches the driver, then the RESZ code written to D3DRS_POINTSIZE.
// The device is PUREDEVICE with hardware T&L, so Get* returns nothing: nothing is read back. Everything touched is
// re-sent from Gx's own caches instead (ZENABLE is always TRUE in Gx, IStateSetD3DDefaults).
template <class Dev>
bool ResolveResz(Dev* d) {
    auto* dev = d->m_d3dDevice;

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZ);
    dev->SetTexture(0, d->m_sceneDepthTexture);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
    const float point[3] = {};
    bool drawn = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_POINTLIST, 1, point, sizeof(point)));
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_POINTSIZE, 0x7FA05000);   // RESZ

    // Device-state cache: put the cached values straight back.
    using Ds = typename Dev::EDeviceState;
    for (Ds ds : { Dev::Ds_ZWriteEnable, Dev::Ds_ColorWriteEnable }) {
        uint32_t value = d->m_deviceStates[ds];
        d->m_deviceStates[ds] = 0xFFFFFFFF;
        if (value != 0xFFFFFFFF) {
            d->DsSet(ds, value);
        }
    }

    // Gx render states: re-sent at the next IRsSync (every draw, IStateSync).
    for (EGxRenderState rs : { GxRs_PointScale, GxRs_Texture0, GxRs_VertexShader, GxRs_PixelShader }) {
        d->IRsForceUpdate(rs);
    }

    // Declaration (SetFVF replaced it) and stream 0 (DrawPrimitiveUP nulls it): re-sent by IStateSyncVertexPtrs.
    d->m_d3dCurrentVertexDecl = nullptr;
    d->m_d3dVertexStreamBuf[0] = nullptr;
    d->m_d3dVertexStreamStride[0] = 0xFFFFFFFF;

    return drawn;
}

// CGxDevice.h contract. No copy: the INTZ texture is the bound depth-stencil itself, sampled (.r = depth 0..1, point
// filter) while the caller draws with depth test and write off, as coa-vfog does. MSAA: INTZ is a RESZ copy of the
// bound multisampled depth instead. nullptr when INTZ is not in use (MSAA without RESZ, unsupported); with
// *transient set when it is only missing this frame (device lost, a failed resolve).
template <class Dev>
CGxTex* Copy(Dev* d, bool* transient) {
    bool dummy;
    bool& retry = transient ? *transient : dummy;
    retry = true;

    if (!d->m_context) {
        return nullptr;
    }

    if (!d->m_sceneDepthTexture) {
        retry = false;
        return nullptr;
    }

    D3DSURFACE_DESC desc;

    if (FAILED(d->m_sceneDepthTexture->GetLevelDesc(0, &desc))) {
        return nullptr;
    }

    if (d->m_sceneDepthResz && !ResolveResz(d)) {
        return nullptr;
    }

    if (!d->m_sceneDepthTex) {
        CGxTexFlags flags(GxTex_Nearest);
        // Render target: 9Ex's ISetTexture/ITexTouch then never try to acquire it or put it in the texture cache.
        flags.m_renderTarget = 1;
        d->m_sceneDepthTex = new CGxTex(GxTex_2d, desc.Width, desc.Height, 0, GxTex_D24X8, GxTex_D24X8, flags, nullptr, nullptr, "VFogSceneDepth");
    }

    CGxTex* texture = d->m_sceneDepthTex;
    texture->m_width = desc.Width;
    texture->m_height = desc.Height;
    texture->m_apiSpecificData = d->m_sceneDepthTexture;
    texture->m_needsCreation = 0;
    texture->m_needsUpdate = 0;

    retry = false;
    return texture;
}

}  // namespace d3d9scenedepth

#endif
