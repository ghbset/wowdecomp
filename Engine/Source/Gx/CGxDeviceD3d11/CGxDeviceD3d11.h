#ifndef ENGINE_SOURCE_GX_CGXDEVICED3D11_CGXDEVICED3D11_H
#define ENGINE_SOURCE_GX_CGXDEVICED3D11_CGXDEVICED3D11_H

#include <cstddef>
#include <cstdint>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <unordered_map>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Storm/H/stpl.h"

// D3D11 backend, modelled on 4.3.4 (15595) CGxDeviceD3d11 (vtable 0x00BD5710). Not part of 12340: no OFFSET tags.
// Selected with `SET gxApi "D3D11"`. See README.md in this folder for what is real and what is stubbed.
class CGxDeviceD3d11 : public CGxDevice {
    public:
    // CGxPool::m_apiSpecific. Every pool keeps a CPU copy: locks write into it, unlocks push the locked range to
    // the GPU buffer (Map for Dynamic/Stream pools, UpdateSubresource for Static). The copy also lets indexed
    // triangle fans be rewritten as lists.
    struct PoolApi {
        ID3D11Buffer* m_buffer;
        char* m_shadow;
        int32_t m_discard;
    };

    // CGxTex::m_apiSpecificData. Textures are created typed except depth (typeless + DSV + SRV, like 4.3.4).
    struct TexApi {
        ID3D11Texture2D* m_texture;
        ID3D11ShaderResourceView* m_srv;
        ID3D11RenderTargetView* m_rtv[6];
        ID3D11DepthStencilView* m_dsv;
        uint32_t m_levels;
    };

    HWND m_hwnd;
    ATOM m_hwndClass;
    int32_t m_ownhwnd;
    HMODULE m_d3dLib;
    HMODULE m_dxgiLib;
    // Always null. wxl add-ons read the D3D9 device pointer at gx +0x397C; when the stock 12340 exe hosts this class
    // (stock-host/DESIGN.md) they must find null there, not one of ours. Asserted below for x86.
    void* m_reserved397C;
    // Always null too: wxl-core reads the D3D9 cached back-buffer / depth surfaces at gx +0x3B3C / +0x3B40.
    void* m_reservedD3d9Tail[(0x3B44 - 0x3980) / 4];
    PFN_D3D11_CREATE_DEVICE m_d3dCreateDevice;
    IDXGIFactory1* m_dxgiFactory;
    ID3D11Device* m_d3dDevice;
    ID3D11DeviceContext* m_d3dContext;
    IDXGISwapChain* m_swapChain;
    D3D_FEATURE_LEVEL m_featureLevel;
    ID3D11Texture2D* m_backBuffer;
    ID3D11RenderTargetView* m_backBufferRtv;
    ID3D11Texture2D* m_depthTexture;
    ID3D11DepthStencilView* m_depthDsv;
    ID3D11ShaderResourceView* m_depthSrv;
    ID3D11Texture2D* m_rtDepthTexture;
    ID3D11DepthStencilView* m_rtDepthDsv;
    ID3D11Texture2D* m_resolveTexture;
    ID3D11Buffer* m_constantBuffers[2]; // [0] VS b0, [1] PS b0: app register files (s_shadowConstants)
    ID3D11Buffer* m_ffConstantBuffers[2]; // [0] VS b0, [1] PS b0 while a fixed-function stage is bound
    ID3D11Buffer* m_devConstantBuffer; // b1 in both stages (shader-xlate README)
    ID3D11Buffer* m_fanIndexBuffer;
    uint32_t m_fanIndexBufferSize;
    ID3D11RenderTargetView* m_curRtv;
    ID3D11DepthStencilView* m_curDsv;
    ID3D11DepthStencilView* m_savedDsv;
    ID3D11Buffer* m_curVertexBuf[8];
    uint32_t m_curVertexOfs[8];
    uint32_t m_curVertexStride[8];
    ID3D11Buffer* m_curIndexBuf;
    D3D11_PRIMITIVE_TOPOLOGY m_curTopology;

    // Phase 2 state. IRsSendToHw edits these descs and sets m_stateDirty; IStateSync resolves them into state objects
    // from m_stateCache (one map for the 5 kinds, keyed by a hash salted with the kind).
    D3D11_RASTERIZER_DESC m_rsDesc;
    D3D11_BLEND_DESC m_blendDesc;
    D3D11_DEPTH_STENCIL_DESC m_dsDesc;
    float m_blendFactor[4];
    uint32_t m_stateDirty;
    uint32_t m_samplerDirty;
    std::unordered_map<uint64_t, ID3D11DeviceChild*> m_stateCache;
    ID3D11ShaderResourceView* m_curSrv[16];
    ID3D11Buffer* m_curCb[2];
    void* m_curVs;
    void* m_curPs;
    const void* m_curVsCode;
    uint32_t m_curVsCodeSize;
    uint64_t m_curVsSig;
    uint64_t m_curLayoutKey;
    float m_devConstants[23 * 4];
    float m_ffConstants[2][53 * 4];
    ID3D11VertexShader* m_ffVs[48];
    ID3D11PixelShader* m_ffPs[3];
    // d3d11Debug 2: shaders created from the SM2 (ps_2_0/vs_2_0) families, by family name.
    std::unordered_map<const CGxShader*, std::string> m_sm2Shaders;

    virtual void ITexMarkAsUpdated(CGxTex* texId);
    virtual void IRsSendToHw(EGxRenderState which);
    virtual int32_t DeviceCreate(GxWindowProc windowProc, const CGxFormat& format);
    virtual int32_t DeviceCreate(void* window, const CGxFormat& format);
    virtual void DeviceDestroy();
    virtual void DeviceEvictResources();
    virtual int32_t DeviceSetFormat(const CGxFormat& format);
    virtual void DeviceSetBaseMipLevel(uint32_t level);
    virtual void* DeviceWindow();
    virtual void DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels);
    virtual void DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths);
    virtual int32_t DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2);
    virtual void DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane);
    virtual void DeviceSaveDepthSurface();
    virtual void DeviceRestoreDepthSurface();
    virtual void DeviceResolveDepthBuffer(CGxTex* texture);
    virtual void DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane);
    virtual void DeviceOverride(EGxOverride override, uint32_t value);
    virtual void CapsWindowSize(CRect& rect);
    virtual void CapsWindowSizeInScreenCoords(CRect& rect);
    virtual void LogCrashInfo(char* buffer, uint32_t size);
    virtual void ScenePresent();
    virtual void SceneClear(uint32_t mask, CImVector color);
    virtual void XformSetProjection(const C44Matrix& matrix);
    virtual void Draw(CGxBatch* batch, int32_t count);
    virtual void PoolSizeSet(CGxPool* pool, uint32_t size);
    virtual void PoolDestroy(CGxPool* pool);
    virtual char* BufLock(CGxBuf* buf);
    virtual int32_t BufUnlock(CGxBuf* buf, uint32_t size);
    virtual void BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset);
    virtual void TexDestroy(CGxTex* texId);
    virtual int32_t TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane);
    virtual int32_t TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face);
    virtual void QueryCreate(CGxQuery*& query, EGxQueryType type);
    virtual void QueryDestroy(CGxQuery*& query);
    virtual bool QueryBegin(CGxQuery* query);
    virtual bool QueryEnd(CGxQuery* query);
    virtual bool QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data);
    virtual bool QueryGetData(CGxQuery* query, uint32_t& data);
    virtual void ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations);
    virtual void ShaderDestroy(CGxShader*& shader);
    virtual void IShaderReload(CGxShader* shader, const char* path, const char* name);
    virtual void IShaderCreate(CGxShader* shader);
    virtual void StereoSetConvergence(float convergence);
    virtual float StereoGetConvergence();
    virtual void StereoSetSeparation(float separation);
    virtual float StereoGetSeparation();
    virtual bool StereoEnabled();
    virtual void DeviceUnk81(uint32_t a, uint32_t b);
    virtual void DeviceUnk82();
    virtual void DeviceUnk83(uint32_t a, uint32_t b);

    static const DXGI_FORMAT s_GxFormatToDxgiColor[CGxFormat::Formats_Last];
    static const DXGI_FORMAT s_GxFormatToDxgiDepth[CGxFormat::Formats_Last][3];
    static const DXGI_FORMAT s_GxTexFmtToDxgiFmt[GxTexFormats_Last];
    static EGxTexFormat s_GxTexFormatToUse[GxTexFormats_Last];
    static const EGxTexFormat s_tolerableTexFmtMapping[GxTexFormats_Last];
    static const D3D11_PRIMITIVE_TOPOLOGY s_primitiveConversion[GxPrims_Last];

    static int32_t ILoadD3dLib(HMODULE& d3dLib, HMODULE& dxgiLib, PFN_D3D11_CREATE_DEVICE& createDevice, IDXGIFactory1*& factory);
    static void IUnloadD3dLib(HMODULE& d3dLib, HMODULE& dxgiLib, IDXGIFactory1*& factory);
    static LRESULT CALLBACK WindowProcD3d11(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    CGxDeviceD3d11();
    virtual ~CGxDeviceD3d11();
    char* IBufLock(CGxBuf* buf);
    void IBufUnlock(CGxBuf* buf);
    int32_t CreatePoolAPI(CGxPool* pool);
    int32_t ICreateD3d();
    int32_t ICreateD3dDevice(const CGxFormat& format);
    int32_t ICreateBackBuffer();
    int32_t ICreateDepth(uint32_t width, uint32_t height, uint32_t samples, ID3D11Texture2D*& texture, ID3D11DepthStencilView*& dsv, ID3D11ShaderResourceView** srv);
    void IReleaseBackBuffer();
    int32_t ICreateWindow(CGxFormat& format);
    void IDestroyD3d();
    void IRecreateD3dDevice();
    void ISceneClearQuad(uint32_t mask, CImVector color);
    void IReleaseD3dPool(CGxPool* pool);
    void IReleaseD3dPools(int32_t freePools);
    void IReleaseD3dQueries();
    void ISetCaps(const CGxFormat& format);
    void ISetWindowFocus(int32_t focus);
    void IBindRenderTargets();
    void IShaderConstantsFlush();
    void IShaderForget(CGxShader* shader);
    void IStateReset();
    void IStateRelease();
    ID3D11DeviceChild* IStateGet(uint32_t kind, const void* desc, uint32_t size);
    bool IStateSyncShaders();
    void IStateSyncFixedFunction(bool vs, bool ps, bool lighting, bool ambientOnly);
    void IStateSyncDeviceConstants(bool ffVs);
    void IStateSyncTextures();
    void IStateSyncScissorRect();
    void ISetTexGen(uint32_t tmu, int32_t texGen);
    void IStateSync();
    void IStateSyncInputLayout();
    void IStateSyncVertexPtrs();
    void ISetVertexBuffer(uint32_t stream, CGxBuf* buf, uint32_t stride);
    void ITexCreate(CGxTex* texId);
    void ITexForceRecreation(int32_t all);
    void ITexRelease(CGxTex* texId);
    void ITexUpload(CGxTex* texId);
    ID3D11Texture2D* IResolvedBackBuffer();
    void IXformSetViewport();
};

#if defined(_M_IX86)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(CGxDeviceD3d11, m_reserved397C) == 0x397C, "wxl reads the D3D9 device pointer at gx +0x397C");
static_assert(offsetof(CGxDeviceD3d11, m_reservedD3d9Tail) + sizeof(CGxDeviceD3d11::m_reservedD3d9Tail) >= 0x3B44,
              "wxl reads D3D9 surfaces at gx +0x3B3C/+0x3B40");
#pragma clang diagnostic pop
#endif

CGxDevice* CreateCGxDeviceD3d11();

#endif
