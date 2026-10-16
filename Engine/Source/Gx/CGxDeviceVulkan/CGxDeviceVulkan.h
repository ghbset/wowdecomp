#ifndef ENGINE_SOURCE_GX_CGXDEVICEVULKAN_CGXDEVICEVULKAN_H
#define ENGINE_SOURCE_GX_CGXDEVICEVULKAN_CGXDEVICEVULKAN_H

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>
#include <windows.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Storm/H/stpl.h"

// Native Vulkan backend for Wow32.exe / Wow64.exe (`SET gxApi "Vulkan"`). Not part of 12340 and not hosted in the
// stock exe: no OFFSET tags, no wxl layout reservations. Structure mirrors CGxDeviceD3d11; see README.md here.
class CGxDeviceVulkan : public CGxDevice {
    public:
    enum { kFrames = 2, kPoolVersions = 8 };

    // A suballocation (VkMemoryBlock range). m_block is null for an empty Alloc.
    struct MemBlock;
    struct Alloc {
        MemBlock* m_block;
        VkDeviceSize m_offset;
        VkDeviceSize m_size;
        char* m_map;
    };

    struct MemBlock {
        VkDeviceMemory m_memory;
        VkDeviceSize m_size;
        uint32_t m_type;
        bool m_image;
        char* m_map;
        std::map<VkDeviceSize, VkDeviceSize> m_free; // offset -> size
    };

    // An image plus what the device tracks about it (layout for barriers).
    struct Image {
        VkImage m_image;
        VkImageView m_view;        // sampled view (depth aspect only for depth)
        VkImageView m_rtView[6];   // attachment view per face (colour) or [0] depth+stencil
        VkFormat m_format;
        VkImageLayout m_layout;
        VkImageAspectFlags m_aspect;
        uint32_t m_width;
        uint32_t m_height;
        uint32_t m_levels;
        uint32_t m_layers;
        Alloc m_alloc;
    };

    // CGxPool::m_apiSpecific. Static pools: device-local buffer, writes go through a CPU copy and a staging copy.
    // Dynamic/Stream pools: host-visible, kPoolVersions copies; a stream-ring discard moves to the next copy.
    struct PoolApi {
        VkBuffer m_buffer;
        Alloc m_alloc;
        char* m_shadow;
        uint32_t m_version;
        uint64_t m_versionSerial[kPoolVersions];
    };

    struct Frame {
        VkCommandPool m_cmdPool;
        VkCommandBuffer m_uploadCmd;
        VkCommandBuffer m_cmd;
        VkFence m_fence;
        VkSemaphore m_acquired;
        VkBuffer m_uboBuffer;
        Alloc m_uboAlloc;
        uint32_t m_uboHead;
        VkDeviceSize m_stagingHead; // used bytes of the last staging buffer
        VkDeviceSize m_stagingCap;
        std::vector<VkBuffer> m_stagingBuffers;
        std::vector<Alloc> m_stagingAllocs;
        std::vector<std::function<void()>> m_deletes;
        std::vector<VkDescriptorPool> m_texPools; // set 1 sets, reset every frame
        uint32_t m_texPool;
    };

    // CGxShader::m_apiSpecific (and the FF tables): the module plus the VKS1 vertex input table
    // (usage, usageIndex, location, compType). m_id keys pipelines (handles and addresses are reused).
    struct ShaderApi {
        VkShaderModule m_module;
        uint64_t m_id;
        uint32_t m_inputCount;
        uint8_t m_inputs[32][4];
    };

    HWND m_hwnd;
    ATOM m_hwndClass;
    int32_t m_ownhwnd;
    HMODULE m_vkLib;
    VkInstance m_instance;
    VkDebugUtilsMessengerEXT m_messenger;
    VkSurfaceKHR m_surface;
    VkPhysicalDevice m_physDevice;
    VkPhysicalDeviceProperties m_physProps;
    VkPhysicalDeviceMemoryProperties m_memProps;
    VkDevice m_vkDevice;
    VkQueue m_queue;
    uint32_t m_queueFamily;
    VkFormat m_depthFormat;
    VkSwapchainKHR m_swapchain;
    VkFormat m_swapFormat;
    VkExtent2D m_swapExtent;
    std::vector<Image> m_swapImages;
    std::vector<VkSemaphore> m_swapReady; // per swapchain image: render done, present may start
    uint32_t m_swapIndex;
    bool m_swapDirty;
    Image m_depth;
    Image m_rtDepth;
    Frame m_frames[kFrames];
    uint64_t m_frameSerial;   // serial of the frame being recorded
    bool m_recording;
    bool m_rendering;
    Image* m_curColor;
    uint32_t m_curColorFace;
    Image* m_curDepth;
    Image* m_passColor;        // attachments of the open dynamic-rendering pass
    Image* m_passDepth;
    VkExtent2D m_passExtent;
    VkViewport m_vkViewport;   // negative height: D3D clip space without a shader flip
    VkRect2D m_vpRect;         // the same viewport in pixels, for clears
    VkRect2D m_vkScissor;
    bool m_scissorTest;
    float m_depthBias;
    uint32_t m_uboBindingCount;
    std::vector<MemBlock*> m_blocks;
    VkDescriptorSetLayout m_setLayouts[2];
    VkPipelineLayout m_pipelineLayout;
    VkDescriptorPool m_descPool;
    VkDescriptorSet m_uboSets[kFrames];
    VkBuffer m_dummyUbo;
    Alloc m_dummyUboAlloc;
    ShaderApi m_ffVs[48];
    ShaderApi m_ffPs[3];
    ShaderApi* m_curVs;
    ShaderApi* m_curPs;
    VkPipelineCache m_pipelineCache;
    std::unordered_map<uint64_t, VkPipeline> m_pipelines;
    std::unordered_map<uint64_t, VkSampler> m_samplers;
    VkPipeline m_curPipeline;
    uint32_t m_uboOffsets[3];  // VS b0, PS b0, cb1 in the frame's UBO ring; UINT32_MAX = not in this frame's ring yet
    int32_t m_cbSource[2];     // what VS/PS b0 hold: 0 app shadow constants, 1 FF constants
    uint32_t m_cbHigh[2];      // highest app register ever written, per stage
    float m_devConstants[23 * 4];
    float m_ffConstants[2][53 * 4];
    float m_blendFactor[4];
    Image m_dummyImage;        // 1x1 zero texture for unbound set 1 slots
    VkDescriptorSet m_curTexSet;
    VkImageView m_curViews[16];
    VkSampler m_curSamplers[16];
    uint32_t m_blendMode;
    VkCompareOp m_depthCompare;
    bool m_depthWrite;
    VkCullModeFlags m_cullMode;
    uint32_t m_colorMask;

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
    virtual bool QueryBegin(CGxQuery* query);
    virtual bool QueryEnd(CGxQuery* query);
    virtual bool QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data);
    virtual bool QueryGetData(CGxQuery* query, uint32_t& data);
    virtual void IShaderReload(CGxShader* shader, const char* path, const char* name);
    virtual void IShaderCreate(CGxShader* shader);
    virtual void ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations);
    virtual void ShaderDestroy(CGxShader*& shader);
    virtual void StereoSetConvergence(float convergence);
    virtual float StereoGetConvergence();
    virtual void StereoSetSeparation(float separation);
    virtual float StereoGetSeparation();
    virtual bool StereoEnabled();
    virtual void DeviceUnk81(uint32_t a, uint32_t b);
    virtual void DeviceUnk82();
    virtual void DeviceUnk83(uint32_t a, uint32_t b);

    static LRESULT CALLBACK WindowProcVulkan(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    CGxDeviceVulkan();
    virtual ~CGxDeviceVulkan();
    int32_t ICreateWindow(CGxFormat& format);
    void ISetWindowFocus(int32_t focus);
    int32_t ICreateVkInstance();
    int32_t ICreateVkDevice(const CGxFormat& format);
    void IDestroyVk();
    void IDestroyVkInstance();
    void ISetCaps(const CGxFormat& format);
    int32_t ICreateSwapchain();
    void IReleaseSwapchain();
    // Memory
    bool IAlloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags, bool image, Alloc& out);
    void IFree(Alloc& alloc);
    bool ICreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, VkBuffer& buffer, Alloc& alloc);
    bool ICreateImage(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, VkFormat format, VkImageUsageFlags usage, bool cube, Image& out);
    void IDestroyImage(Image& image, bool deferred);
    void IDefer(std::function<void()> fn);
    // Frames and commands
    void IBeginFrame();
    bool IWaitSerial(uint64_t serial);
    bool IWaitFence(VkFence fence, const char* what);
    void IWaitIdle(const char* what);
    void IBeginRendering();
    void IReleaseRtDepth();
    void IEndRendering();
    void ITransition(VkCommandBuffer cmd, Image& image, VkImageLayout layout);
    char* IStaging(uint32_t size, VkBuffer& buffer, VkDeviceSize& offset);
    void IBindRenderTargets();
    void IXformSetViewport();
    void IStateSyncScissorRect();
    VkPipeline IPipelineGet(uint32_t topologyClass, const VkVertexInputAttributeDescription* attribs, uint32_t attribCount, uint32_t bindingCount);
    bool IShaderApiCreate(const void* body, uint32_t size, ShaderApi& out);
    void IShaderApiRelease(ShaderApi& api);
    bool IStateSyncShaders(bool& ffVs, bool& ffPs);
    void IStateSyncFixedFunction(bool vs, bool ps, bool lighting, bool ambientOnly);
    bool IStateSyncConstants(bool ffVs, bool ffPs);
    uint32_t IUboPush(const void* data, uint32_t size);
    VkDescriptorSet IStateSyncTextures();
    VkSampler ISamplerGet(uint32_t filter, bool wrapU, bool wrapV, uint32_t anisotropy, float lodBias);
    void ISetTexGen(uint32_t tmu, int32_t texGen);
    // Pools, textures
    int32_t CreatePoolAPI(CGxPool* pool);
    void IReleasePool(CGxPool* pool);
    void IReleasePools(int32_t freePools);
    char* IBufLock(CGxBuf* buf);
    void IBufUnlock(CGxBuf* buf);
    void ITexCreate(CGxTex* texId);
    void ITexRelease(CGxTex* texId);
    void ITexUpload(CGxTex* texId);
    void ITexForceRecreation(int32_t all);
};

CGxDevice* CreateCGxDeviceVulkan();

#endif
