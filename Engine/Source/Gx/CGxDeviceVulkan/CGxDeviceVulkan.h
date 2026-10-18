#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_CGXDEVICEVULKAN_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_CGXDEVICEVULKAN_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Storm/H/stpl.h"

// Not in the original: the Vulkan backend, a CGxDevice beside the D3D9 / D3D9Ex / OpenGL ones,
// selected with the gxApi CVar ("Vulkan") or "-vulkan" on the command line. The engine above the
// device is untouched; the Vulkan objects live in the engine-agnostic tier under Vulkan/ (the
// loader, the context, device memory, the frame, the textures, the pipelines).
//
// What this device does today: its own window with the instance, device, surface and swapchain on
// it; the engine's pools and buffers on VkBuffers (CGxDeviceVulkanPool.cpp); the engine's textures
// as VkImages filled through the engine's own update protocol (CGxDeviceVulkanTexture.cpp); the
// engine's "UI" and "Desaturate" shaders as the backend's own programs and every other stock
// shader as the SPIR-V module Tools/VulkanShaders translated from its bytecode, found by the md5 of
// the code through the manifest beside the executable -- a shader without one stays invalid so the
// engine takes its fixed-function paths (CGxDeviceVulkanShader.cpp); every Draw recorded into the
// frame's render pass with its texture stages, constant registers, the blend mode, the alpha test,
// the stage-0 colour ops, the depth test and write, the culling, the colour mask, the fill mode and
// the depth bias (CGxDeviceVulkanDraw.cpp); the engine's SceneClear as the pass's clear; the
// engine's render targets as the frame's targets, with the back-buffer copies and the screenshot
// read-back off the scene image (CGxDeviceVulkanTarget.cpp); occlusion queries on a query pool
// (CGxDeviceVulkanQuery.cpp); the gamma ramp applied by the present pass; the hardware cursor as a
// Win32 cursor built from the engine's cursor image (CGxDeviceVulkanCursor.cpp). Fixed-function
// lighting and fog, user clip planes, multisampling and stereo are not offered (the caps say so),
// and their virtuals keep the engine's bookkeeping only (CGxDeviceVulkanStubs.cpp).
class CGxDeviceVulkan : public CGxDevice {
    public:
    HWND m_hwnd;
    ATOM m_hwndClass;
    int32_t m_ownhwnd;
    // The bring-up answered Retry (the window had no client area when the device was created, e.g.
    // started minimised): finish it at the first present that finds a usable window.
    int32_t m_vulkanPending;
    // Device memory, the textures and the pipelines are up: buffers get VkBuffers, textures get
    // images and draws are recorded. Without it the pools take the engine's emergency memory and
    // the frame presents the clear colour only.
    int32_t m_drawPath;
    // The texture whose update protocol is running: its callback may bind textures and draw (the
    // minimap renders into its texture from inside it), and must not re-enter its own update.
    CGxTex* m_filling;
    // The translated-shader manifest was looked for once (found or not) for this device.
    int32_t m_manifestTried;
    // The device's constant registers (D3D's SetVertexShaderConstantF / SetPixelShaderConstantF
    // targets): the engine's shadow constants are moved here by their dirty ranges at each draw,
    // and persist across the shadow's per-present clear as D3D's registers do.
    float m_vertexRegs[256 * 4];
    float m_pixelRegs[256 * 4];
    // The hardware cursor: a Win32 cursor built from the engine's 32x32 cursor image, remade when
    // the image changes (CursorUnlock), shown over the client area by WM_SETCURSOR.
    HCURSOR m_hcursor;
    int32_t m_hwCursorDirty;

    virtual void ITexMarkAsUpdated(CGxTex* texId);
    virtual void ICursorCreate(const CGxFormat& format);
    virtual void ICursorDestroy();
    virtual void ICursorDraw();
    virtual void CursorSetVisible(int32_t visible);
    virtual void CursorUnlock(uint32_t x, uint32_t y);
    virtual void IRsSendToHw(EGxRenderState which);
    virtual ~CGxDeviceVulkan();
    virtual int32_t DeviceCreate(GxWindowProc windowProc, const CGxFormat& format);
    virtual int32_t DeviceCreate(void* window, const CGxFormat& format);
    virtual void DeviceDestroy();
    virtual int32_t DeviceSetFormat(const CGxFormat& format);
    virtual void DeviceSetBaseMipLevel(uint32_t level);
    virtual void DeviceSetGamma(float gamma);
    virtual void DeviceSetGamma(const CGxGammaRamp& ramp);
    virtual void* DeviceWindow();
    virtual void DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels);
    virtual void DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths);
    virtual int32_t DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2);
    virtual void DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane);
    virtual void DeviceOverride(EGxOverride override, uint32_t value);
    virtual void CapsWindowSize(CRect& rect);
    virtual void CapsWindowSizeInScreenCoords(CRect& rect);
    virtual void LogCrashInfo(char* buffer, uint32_t size);
    virtual void ScenePresent();
    virtual void SceneClear(uint32_t mask, CImVector color);
    virtual void XformSetProjection(const C44Matrix& matrix);
    virtual void XformSetView(const C44Matrix& matrix);
    virtual void Draw(CGxBatch* batch, int32_t count);
    virtual void PrimPointSize(float size);
    virtual void PrimLineWidth(float width);
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
    virtual void ShaderConstantsSet(EGxShTarget target, uint32_t index, const float* constants, uint32_t count);
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
    // VFOG: the scene depth snapshot (CGxDeviceVulkanTexture.cpp, frame::CopyDepthFromScene).
    virtual CGxTex* DeviceCopySceneDepth(bool* transient = nullptr);
    virtual bool DeviceSceneDepthIsCopy() { return true; }

    static LRESULT CALLBACK WindowProcVulkan(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    CGxDeviceVulkan();

    // CGxDeviceVulkan.cpp: the Vulkan side of the lifecycle
    int32_t ICreateVulkan(const CGxFormat& format);
    void ICreateDrawPath();
    void IBringUpIfPending();
    void IReleaseVulkanWindow();
    void IDestroyVulkan();
    void IStateSetDefaults();
    // CGxDeviceVulkanWindow.cpp: the window
    int32_t ICreateWindow(CGxFormat& format);
    void IDestroyWindow();
    int32_t IWindowVisible();
    void ISetWindowFocus(int32_t focus);
    // CGxDeviceVulkanCaps.cpp
    void ISetCaps(const CGxFormat& format);
    // CGxDeviceVulkanPool.cpp: the pools on VkBuffers
    char* IBufLock(CGxBuf* buf);
    void IBufUnlock(CGxBuf* buf, uint32_t begin, uint32_t length);
    int32_t IPoolBuffer(CGxPool* pool, VkBuffer& buffer);
    void INoteBufDrawn(CGxBuf* buf);
    void IReleasePools();
    // CGxDeviceVulkanTexture.cpp: the textures as images
    void ITexCreate(CGxTex* texId);
    void ITexUpload(CGxTex* texId);
    void ITexForceRecreation(int32_t all);
    int32_t ITexResolve(CGxTex* texId, uint32_t stage, gxvk::vk::pipelines::Draw& draw);
    gxvk::vk::texture::Image* ITexImage(CGxTex* texId);
    void ITexTouch(CGxTex* texId);
    void IReleaseTextures();
    // CGxDeviceVulkanTarget.cpp: the render targets, the back-buffer copies, the read-back
    int32_t ITargetImage(CGxTex* texId, gxvk::vk::texture::Image*& image, uint32_t plane, uint32_t& layer);
    // CGxDeviceVulkanCursor.cpp: the hardware cursor
    void ICursorApply();
    int32_t ICursorInClient();
    // CGxDeviceVulkanShader.cpp: the engine's shaders as the backend's own or translated programs
    void IShaderFolder(char* folder, size_t size);
    void IShaderConstantsFlush();
    uint32_t IShaderPrograms(gxvk::vk::pipelines::Draw& draw);
    // CGxDeviceVulkanDraw.cpp: the draw
    int32_t IPlaceD3d(VkViewport& viewport, VkRect2D& scissor);
    void IModelViewProj(float out[16]);
    void IRenderStates(gxvk::vk::pipelines::Draw& draw);
    void ITexTransforms(gxvk::vk::pipelines::Draw& draw);
};

CGxDevice* CreateCGxDeviceVulkan();

#endif
