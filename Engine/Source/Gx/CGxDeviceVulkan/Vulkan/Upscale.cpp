// FSR: see Upscale.hpp. The provider calls follow wxl-fsr src/Fsr4.cpp (branch fsr4), the motion vectors wxl-light
// src/Depth.cpp kMotionHlsl (branch fsr4); keep the semantics in step with those when either changes.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Upscale.hpp"

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture/Internal.hpp"
#include "Engine/Source/Gx/CGxMatrixStack.h"
#include "Engine/Source/Gx/GxUpscale.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Ffx/upscalers/include/ffx_upscale.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/UpscaleSpirv.inc"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;
    namespace texture = gxvk::vk::texture;

    using gxvk::log::Write;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;

    // The provider's own descriptors (fsr4vk provider/ffx_vk_provider.cpp, ffx_vk_device_requirements.h), as wxl-fsr.
    constexpr ffxStructType_t kBackendVkType = 3u, kApiVersionType = 0x46535234564b4150ull,
                              kPrepareDeviceType = 0x4656584450524550ull, kReleaseDeviceType = 0x4656584452454c53ull;
    struct BackendVkDesc { ffxCreateContextDescHeader header; VkDevice device; VkPhysicalDevice physicalDevice;
                           PFN_vkGetDeviceProcAddr getDeviceProcAddr; };
    struct ApiVersionDesc { ffxApiHeader header; uint32_t apiVersion; };
    struct PrepareDeviceDesc
    {
        ffxQueryDescHeader header;
        VkPhysicalDevice physicalDevice;
        const VkDeviceCreateInfo* sourceCreateInfo;
        uint32_t apiVersion;
        PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2;
        PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties;
        const VkDeviceCreateInfo* outputCreateInfo;
        void* token;
        const char* errorMessage;
    };
    struct ReleaseDeviceDesc { ffxQueryDescHeader header; void* token; };

    /// Shaders/Upscale.glsl GxUpscale (std140, vec4s and mat4s only)
    struct Constants
    {
        float m_invProj[16];
        float m_reproj[16];
        float m_render[4];
        float m_depthRange[4];
        float m_jitter[4];
        float m_projW[4];
        float m_native[4];
        float m_source[4];
    };

    constexpr VkDeviceSize kUboStride = 512;
    constexpr uint32_t kSets = 4;      ///< dispatches per frame slot (2 used)
    constexpr uint32_t kGroup = 8;     ///< Upscale.glsl local_size
    static_assert(sizeof(Constants) <= kUboStride, "Constants outgrew its slot");

    enum Prog { Prog_In, Prog_Out, Prog_Count };

    struct Cvars
    {
        CVar* mode = nullptr;
        CVar* scale = nullptr;
        CVar* shaders = nullptr;
        CVar* version = nullptr;
        CVar* provider = nullptr;
    };
    Cvars s_cv;

    CVar* Reg(const char* name, const char* help, const char* value)
    {
        return CVar::Register(name, help, 1, value, nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    }

    void RegisterCVars()
    {
        if (s_cv.mode) return;
        s_cv.mode = Reg("fsrMode", "Vulkan world upscaler: off, or fsr4 (FSR 4 from FSR4\\<arch>\\, read when the device is made: restart after turning it on)", "off");
        s_cv.scale = Reg("fsrScale", "Vulkan upscaler render scale, 0.5 to 1 of the world viewport (1 = no upscale)", "0.67");
        s_cv.shaders = Reg("fsr4Shaders", "FSR 4 provider: fast (wxlfsr4-fast.dll, the shader-performance overrides; wxlfsr4.dll if missing) or stock (wxlfsr4.dll)", "fast");
        s_cv.version = Reg("fsr4Version", "FSR 4 version the provider runs: 4.1.1 or 4.0.2 (any version name the provider offers)", "4.1.1");
        s_cv.provider = Reg("fsr4Provider", "FSR 4 provider DLL name in FSR4\\<arch>\\, overriding fsr4Shaders (empty = from fsr4Shaders)", "");
    }

    const char* Str(CVar* cv) { return cv && cv->m_stringValue.m_str ? cv->m_stringValue.m_str : ""; }

    bool ModeFsr4() { return _stricmp(Str(s_cv.mode), "fsr4") == 0; }

    /// FSR's jitter sequence (ffxFsr3GetJitterOffset): radical inverse of index in base.
    float Halton(int index, int base)
    {
        float f = 1.0f, r = 0.0f;
        for (; index > 0; index /= base) { f /= base; r += f * (index % base); }
        return r;
    }

    void ProviderMsg(uint32_t type, const wchar_t* m)
    {
        if (m) Write(type == FFX_API_MESSAGE_TYPE_ERROR ? Warn : Info, "vulkan: fsr4 provider: %ls", m);
    }

    /// The provider, loaded once per process and never unloaded.
    struct Provider
    {
        HMODULE lib = nullptr;
        bool tried = false;
        PfnFfxCreateContext  create = nullptr;
        PfnFfxDestroyContext destroy = nullptr;
        PfnFfxQuery          query = nullptr;
        PfnFfxDispatch       dispatch = nullptr;
        char path[MAX_PATH] = "";
        void* token = nullptr;        ///< PrepareDevice's create-info copy, until DevicePrepared
        bool deviceReady = false;     ///< the device was made with the provider's requirements
        uint32_t apiVersion = 0;      ///< what it was made as

        bool Load()
        {
            if (this->tried) return this->lib != nullptr;
            this->tried = true;

            char dir[MAX_PATH] = "";
            DWORD n = GetModuleFileNameA(nullptr, dir, MAX_PATH);
            while (n && dir[n - 1] != '\\' && dir[n - 1] != '/') --n;
            dir[n] = '\0';
#if defined(_WIN64)
            const char* arch = "x64";
#else
            const char* arch = "x86";
#endif
            // the lookup: fsr4Provider as named, else the fsr4Shaders choice, the stock shaders last
            const char* names[3] = { Str(s_cv.provider), _stricmp(Str(s_cv.shaders), "stock") ? "wxlfsr4-fast.dll" : "", "wxlfsr4.dll" };

            for (const char* name : names)
            {
                if (!*name) continue;
                snprintf(this->path, sizeof(this->path), "%sFSR4\\%s\\%s", dir, arch, name);
                this->lib = LoadLibraryExA(this->path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (this->lib) break;
                Write(Warn, "vulkan: fsr4: %s did not load (error %lu)", this->path, GetLastError());
            }

            if (!this->lib) return false;
            this->create = reinterpret_cast<PfnFfxCreateContext>(reinterpret_cast<void*>(GetProcAddress(this->lib, "ffxCreateContext")));
            this->destroy = reinterpret_cast<PfnFfxDestroyContext>(reinterpret_cast<void*>(GetProcAddress(this->lib, "ffxDestroyContext")));
            this->query = reinterpret_cast<PfnFfxQuery>(reinterpret_cast<void*>(GetProcAddress(this->lib, "ffxQuery")));
            this->dispatch = reinterpret_cast<PfnFfxDispatch>(reinterpret_cast<void*>(GetProcAddress(this->lib, "ffxDispatch")));

            if (!this->create || !this->destroy || !this->query || !this->dispatch)
            {
                Write(Warn, "vulkan: fsr4: %s lacks an ffx entry point; no upscaler", this->path);
                FreeLibrary(this->lib);
                this->lib = nullptr;
                return false;
            }

            Write(Info, "vulkan: fsr4: provider %s", this->path);
            return true;
        }

        void Release()
        {
            if (!this->token) return;
            ReleaseDeviceDesc rel = {};
            rel.header.type = kReleaseDeviceType;
            rel.token = this->token;
            this->query(nullptr, &rel.header);
            this->token = nullptr;
        }
    };
    Provider s_provider;

    struct Slot
    {
        uint64_t         serial = 0;
        VkDescriptorPool pool   = VK_NULL_HANDLE;
        memory::Buffer   ubo;
        uint32_t         used   = 0;
    };

    /// The resource description the provider reads: on Wow32 a pointer to the 64-bit VkImage (fsr4vk/build.sh
    /// FSR4_IMAGE_OF), on Wow64 the handle itself.
    FfxApiResource Res(const VkImage& img, uint32_t w, uint32_t h, uint32_t fmt, uint32_t state, uint32_t usage)
    {
        FfxApiResource x = {};
#if defined(_WIN64)
        x.resource = reinterpret_cast<void*>(img);
#else
        x.resource = const_cast<VkImage*>(&img);
#endif
        x.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        x.description.format = fmt;
        x.description.width = w;
        x.description.height = h;
        x.description.depth = 1;
        x.description.mipCount = 1;
        x.description.usage = usage;
        x.state = state;
        return x;
    }

    class VulkanUpscale final : public IGxUpscale
    {
        public:
        void BeginWorld(float& scaleX, float& scaleY) override;
        void SetupView(const C3Vector& origin) override;
        void EndWorld() override;
        void Release();

        CGxDeviceVulkan* m_device = nullptr;

        private:
        bool m_failed = false;          ///< for this device: FSR is not tried again
        bool m_madeObjects = false;
        bool m_active = false;          ///< this frame's world is drawn at the render size
        bool m_viewSet = false;
        bool m_loggedMsaa = false;
        bool m_loggedSize = false;
        bool m_loggedFirst = false;
        VkRect2D m_native = {};         ///< the world viewport, scene pixels
        VkExtent2D m_render = {};       ///< the part of it drawn into, at its top-left
        float m_minDepth = 0.0f;
        float m_maxDepth = 1.0f;
        float m_jitterPx[2] = {};
        float m_jitterNdc[2] = {};
        uint32_t m_index = 0;
        LARGE_INTEGER m_lastTime = {};

        // this frame's camera (SetupView) and last frame's, unjittered
        C44Matrix m_view, m_proj, m_projJ;
        C3Vector m_origin;
        C44Matrix m_prevView, m_prevProj;
        C3Vector m_prevOrigin;
        bool m_havePrev = false;

        VkDevice m_vkDevice = VK_NULL_HANDLE;
        const loader::Device* m_dapi = nullptr;
        VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkPipeline m_pipelines[Prog_Count] = {};
        VkSampler m_nearest = VK_NULL_HANDLE;
        VkSampler m_linear = VK_NULL_HANDLE;
        Slot m_slots[frame::kFramesInFlight];

        ffxContext m_ctx = nullptr;
        uint64_t m_versionId = 0;
        char m_versionName[32] = "";
        VkExtent2D m_ctxRender = {}, m_ctxNative = {};

        texture::Image m_depth;       ///< the scene depth copy
        texture::Image m_color;       ///< RGBA16F, render size
        texture::Image m_fsrDepth;    ///< R32F, render size
        texture::Image m_motion;      ///< RG16F, render size
        texture::Image m_out;         ///< RGBA16F, the viewport's size: FSR's output
        texture::Image m_sceneOut;    ///< RGBA8 at the scene's extent, B, G, R, A: copied into the scene
        uint64_t m_lastUsed = 0;

        bool ICreate();
        bool IVersion();
        bool IContext();
        bool IEnsure(texture::Image& img, uint32_t width, uint32_t height, VkFormat format, bool depth);
        bool IDispatch(VkCommandBuffer cmd, Prog prog, const Constants& c, VkImageView depth, VkImageView color, VkImageLayout colorLayout,
                       texture::Image* outs[3], uint32_t width, uint32_t height);
        void IConstants(Constants& c) const;
        bool IFsr(VkCommandBuffer cmd, bool reset);
        void IJitter(bool on);
        void IGeneral(VkCommandBuffer cmd, texture::Image& img);
        void IToRead(VkCommandBuffer cmd, texture::Image& img);
        void IFail(const char* what);
    };

    VulkanUpscale s_upscale;

    void VulkanUpscale::IFail(const char* what)
    {
        if (!this->m_failed) Write(Warn, "vulkan: fsr4 unavailable: %s -- the world draws at full size", what);
        this->m_failed = true;
    }

    bool VulkanUpscale::ICreate()
    {
        if (this->m_madeObjects) return true;
        if (this->m_failed) return false;

        const context::Handles* h = context::Get();
        if (!h || !h->dapi) return false;
        this->m_vkDevice = h->device;
        this->m_dapi = h->dapi;
        const loader::Device& d = *this->m_dapi;

        const uint32_t numbers[6] = { 0, 1, 3, 4, 5, 6 };
        VkDescriptorSetLayoutBinding bindings[6] = {};
        for (uint32_t i = 0; i < 6; i++)
        {
            bindings[i].binding         = numbers[i];
            bindings[i].descriptorType  = numbers[i] < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                        : numbers[i] == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo = {};
        setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = 6;
        setInfo.pBindings    = bindings;
        VkResult r = d.CreateDescriptorSetLayout(this->m_vkDevice, &setInfo, nullptr, &this->m_setLayout);

        if (r == VK_SUCCESS)
        {
            VkPipelineLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts    = &this->m_setLayout;
            r = d.CreatePipelineLayout(this->m_vkDevice, &layoutInfo, nullptr, &this->m_layout);
        }

        struct Code { const uint32_t* words; size_t bytes; };
        const Code code[Prog_Count] = { { kUpscaleInComp, sizeof kUpscaleInComp }, { kUpscaleOutComp, sizeof kUpscaleOutComp } };

        for (uint32_t i = 0; r == VK_SUCCESS && i < Prog_Count; i++)
        {
            VkShaderModuleCreateInfo moduleInfo = {};
            moduleInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            moduleInfo.codeSize = code[i].bytes;
            moduleInfo.pCode    = code[i].words;
            VkShaderModule module = VK_NULL_HANDLE;
            r = d.CreateShaderModule(this->m_vkDevice, &moduleInfo, nullptr, &module);
            if (r != VK_SUCCESS) break;

            VkComputePipelineCreateInfo info = {};
            info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = module;
            info.stage.pName  = "main";
            info.layout       = this->m_layout;
            r = d.CreateComputePipelines(this->m_vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &this->m_pipelines[i]);
            d.DestroyShaderModule(this->m_vkDevice, module, nullptr);
        }

        for (uint32_t i = 0; r == VK_SUCCESS && i < 2; i++)
        {
            VkSamplerCreateInfo info = {};
            info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            info.magFilter    = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            info.minFilter    = info.magFilter;
            info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            r = d.CreateSampler(this->m_vkDevice, &info, nullptr, i ? &this->m_linear : &this->m_nearest);
        }

        VkDescriptorPoolSize sizes[3] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kSets },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * kSets },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSets },
        };
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = kSets;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes    = sizes;

        for (Slot& slot : this->m_slots)
        {
            if (r == VK_SUCCESS) r = d.CreateDescriptorPool(this->m_vkDevice, &poolInfo, nullptr, &slot.pool);
            if (r == VK_SUCCESS && !memory::CreateBuffer(kSets * kUboStride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, slot.ubo)) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            if (r == VK_SUCCESS && !slot.ubo.block.mapped) r = VK_ERROR_MEMORY_MAP_FAILED;
        }

        this->m_madeObjects = true;   // Release() takes whatever was made

        if (r != VK_SUCCESS)
        {
            Write(Error, "vulkan: fsr4: the upscale passes could not be made: %s", gxvk::vk::ResultName(r));
            this->IFail("its passes could not be made");
            return false;
        }

        return this->IVersion();
    }

    // The version fsr4Version names among the provider's (wxl-fsr Fsr4::Init step 6).
    bool VulkanUpscale::IVersion()
    {
        uint64_t count = 0, ids[8] = {};
        const char* names[8] = {};
        ffxQueryDescGetVersions qv = {};
        qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        qv.outputCount = &count;
        s_provider.query(nullptr, &qv.header);
        if (count > 8) count = 8;
        qv.versionIds = ids;
        qv.versionNames = names;
        if (count) s_provider.query(nullptr, &qv.header);

        const char* want = Str(s_cv.version);
        for (uint64_t i = 0; i < count; ++i)
        {
            if (names[i] && (!*want || strstr(names[i], want)))
            {
                this->m_versionId = ids[i];
                snprintf(this->m_versionName, sizeof(this->m_versionName), "%s", names[i]);
                break;
            }
        }

        if (!this->m_versionId)
        {
            char line[200];
            snprintf(line, sizeof(line), "%s offers no FSR %s (%u versions)", s_provider.path, want, static_cast<unsigned>(count));
            this->IFail(line);
            return false;
        }

        return true;
    }

    // The FSR context for this render and viewport size (wxl-fsr Fsr4::CreateSized); the old one after the GPU is done.
    bool VulkanUpscale::IContext()
    {
        if (this->m_ctx && this->m_ctxRender.width == this->m_render.width && this->m_ctxRender.height == this->m_render.height
            && this->m_ctxNative.width == this->m_native.extent.width && this->m_ctxNative.height == this->m_native.extent.height)
            return true;

        const context::Handles* h = context::Get();

        if (this->m_ctx)
        {
            this->m_dapi->DeviceWaitIdle(this->m_vkDevice);
            s_provider.destroy(&this->m_ctx, nullptr);
            this->m_ctx = nullptr;
        }

        ffxCreateContextDescUpscale cd = {};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        // the 8-bit gamma frame as it is; exposure left to the provider's own pass
        cd.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        cd.maxRenderSize = { this->m_render.width, this->m_render.height };
        cd.maxUpscaleSize = { this->m_native.extent.width, this->m_native.extent.height };
        cd.fpMessage = &ProviderMsg;
        BackendVkDesc be = {};
        be.header.type = kBackendVkType;
        be.device = h->device;
        be.physicalDevice = h->physical;
        be.getDeviceProcAddr = h->iapi->GetDeviceProcAddr;
        ApiVersionDesc av = {};
        av.header.type = kApiVersionType;
        av.apiVersion = s_provider.apiVersion;
        ffxOverrideVersion ov = {};
        ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ov.versionId = this->m_versionId;
        cd.header.pNext = &be.header;
        be.header.pNext = &av.header;
        av.header.pNext = &ov.header;

        const ffxReturnCode_t r = s_provider.create(&this->m_ctx, &cd.header, nullptr);

        if (r != FFX_API_RETURN_OK || !this->m_ctx)
        {
            this->m_ctx = nullptr;
            char line[160];
            snprintf(line, sizeof(line), "ffxCreateContext %u (%ux%u -> %ux%u)", static_cast<unsigned>(r), this->m_render.width, this->m_render.height,
                     this->m_native.extent.width, this->m_native.extent.height);
            this->IFail(line);
            return false;
        }

        this->m_ctxRender = this->m_render;
        this->m_ctxNative = this->m_native.extent;
        this->m_havePrev = false;
        Write(Info, "vulkan: fsr4: FSR %s context %ux%u -> %ux%u (the model builds on the first frame)", this->m_versionName,
              this->m_render.width, this->m_render.height, this->m_native.extent.width, this->m_native.extent.height);
        return true;
    }

    bool VulkanUpscale::IEnsure(texture::Image& img, uint32_t width, uint32_t height, VkFormat format, bool depth)
    {
        if (img.handle != VK_NULL_HANDLE && img.extent.width == width && img.extent.height == height && img.format == format) return true;
        if (img.handle != VK_NULL_HANDLE) frame::RetireImage(img, this->m_lastUsed);

        img = texture::Image();
        const bool ok = depth ? texture::CreateDepthCopy(width, height, format, img) : texture::CreateStorage(width, height, format, img);
        if (!ok) img = texture::Image();
        return ok;
    }

    // Written whole this frame: what it held does not matter.
    void VulkanUpscale::IGeneral(VkCommandBuffer cmd, texture::Image& img)
    {
        texture::detail::Transition(cmd, img.handle, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        img.layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // FSR's COMPUTE_READ: after our compute writes.
    void VulkanUpscale::IToRead(VkCommandBuffer cmd, texture::Image& img)
    {
        texture::detail::Transition(cmd, img.handle, 1, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    void VulkanUpscale::IConstants(Constants& c) const
    {
        memset(&c, 0, sizeof(c));

        C44Matrix invProj = this->m_projJ.Inverse();
        memcpy(c.m_invProj, &invProj, sizeof(c.m_invProj));

        // this frame's view space -> render-relative world (this origin) -> last frame's origin -> last frame's clip
        if (this->m_havePrev)
        {
            C44Matrix shift;
            shift.d0 = this->m_origin.x - this->m_prevOrigin.x;
            shift.d1 = this->m_origin.y - this->m_prevOrigin.y;
            shift.d2 = this->m_origin.z - this->m_prevOrigin.z;
            C44Matrix reproj = this->m_view.Inverse() * shift * this->m_prevView * this->m_prevProj;
            memcpy(c.m_reproj, &reproj, sizeof(c.m_reproj));
        }

        c.m_render[0] = static_cast<float>(this->m_native.offset.x);
        c.m_render[1] = static_cast<float>(this->m_native.offset.y);
        c.m_render[2] = static_cast<float>(this->m_render.width);
        c.m_render[3] = static_cast<float>(this->m_render.height);
        c.m_depthRange[0] = this->m_minDepth;
        c.m_depthRange[1] = this->m_maxDepth;
        c.m_depthRange[2] = this->m_maxDepth > this->m_minDepth ? 1.0f / (this->m_maxDepth - this->m_minDepth) : 1.0f;
        const float nearPlane = this->m_proj.c2 != 0.0f ? -this->m_proj.d2 / this->m_proj.c2 : 0.0f;
        c.m_depthRange[3] = nearPlane > 0.0f ? nearPlane : 0.2f;
        c.m_jitter[0] = this->m_jitterNdc[0];
        c.m_jitter[1] = this->m_jitterNdc[1];
        c.m_jitter[2] = this->m_havePrev ? 1.0f : 0.0f;
        c.m_projW[0] = this->m_proj.a3;
        c.m_projW[1] = this->m_proj.b3;
        c.m_projW[2] = this->m_proj.c3;
        c.m_projW[3] = this->m_proj.d3;
        c.m_native[0] = static_cast<float>(this->m_native.offset.x);
        c.m_native[1] = static_cast<float>(this->m_native.offset.y);
        c.m_native[2] = static_cast<float>(this->m_native.extent.width);
        c.m_native[3] = static_cast<float>(this->m_native.extent.height);
    }

    bool VulkanUpscale::IDispatch(VkCommandBuffer cmd, Prog prog, const Constants& c, VkImageView depth, VkImageView color, VkImageLayout colorLayout,
                                  texture::Image* outs[3], uint32_t width, uint32_t height)
    {
        Slot& slot = this->m_slots[frame::Slot()];
        const loader::Device& d = *this->m_dapi;

        if (slot.serial != frame::Serial())
        {
            // the slot's previous frame has completed: the frame waited on its fence to open
            d.ResetDescriptorPool(this->m_vkDevice, slot.pool, 0);
            slot.serial = frame::Serial();
            slot.used = 0;
        }

        if (slot.used == kSets) return false;

        VkDescriptorSetAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = slot.pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &this->m_setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (d.AllocateDescriptorSets(this->m_vkDevice, &alloc, &set) != VK_SUCCESS) return false;

        const VkDeviceSize offset = slot.used++ * kUboStride;
        memcpy(static_cast<uint8_t*>(slot.ubo.block.mapped) + offset, &c, sizeof(c));
        memory::FlushHost(slot.ubo.block);

        VkDescriptorImageInfo images[5] = {};
        images[0] = { this->m_nearest, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        images[1] = { this->m_linear, color, colorLayout };
        const uint32_t outBindings[3] = { 3, 5, 6 };
        VkDescriptorBufferInfo ubo = { slot.ubo.handle, offset, sizeof(Constants) };

        // only what the pass reads: a binding no code of the pipeline names needs no descriptor
        VkWriteDescriptorSet writes[6] = {};
        uint32_t count = 0;
        auto add = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* image) {
            VkWriteDescriptorSet& w = writes[count++];
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = set;
            w.dstBinding      = binding;
            w.descriptorCount = 1;
            w.descriptorType  = type;
            if (image) w.pImageInfo = image;
            else w.pBufferInfo = &ubo;
        };

        if (depth != VK_NULL_HANDLE) add(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[0]);
        add(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[1]);
        add(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr);

        for (uint32_t i = 0; i < 3; i++)
        {
            if (!outs[i]) continue;
            images[2 + i] = { VK_NULL_HANDLE, outs[i]->view, VK_IMAGE_LAYOUT_GENERAL };
            add(outBindings[i], VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &images[2 + i]);
        }

        d.UpdateDescriptorSets(this->m_vkDevice, count, writes, 0, nullptr);

        // every earlier write (the scene's pass, the depth copy, the previous dispatch, FSR) before this one
        VkMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->m_pipelines[prog]);
        d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->m_layout, 0, 1, &set, 0, nullptr);
        d.CmdDispatch(cmd, (width + kGroup - 1) / kGroup, (height + kGroup - 1) / kGroup, 1);
        return true;
    }

    // The inputs packed, FSR 4 dispatched into m_out (wxl-fsr Fsr4::Run, on this device's own command buffer).
    bool VulkanUpscale::IFsr(VkCommandBuffer cmd, bool reset)
    {
        const texture::Image* scene = frame::Scene();
        Constants c;
        this->IConstants(c);

        this->IGeneral(cmd, this->m_color);
        this->IGeneral(cmd, this->m_fsrDepth);
        this->IGeneral(cmd, this->m_motion);
        texture::Image* ins[3] = { &this->m_color, &this->m_fsrDepth, &this->m_motion };
        if (!this->IDispatch(cmd, Prog_In, c, this->m_depth.view, scene->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ins, this->m_render.width, this->m_render.height))
            return false;

        for (texture::Image* img : ins) this->IToRead(cmd, *img);
        this->IGeneral(cmd, this->m_out);

        const uint32_t rw = this->m_render.width, rh = this->m_render.height;
        const uint32_t nw = this->m_native.extent.width, nh = this->m_native.extent.height;
        ffxDispatchDescUpscale di = {};
        di.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        di.commandList = cmd;
        di.color = Res(this->m_color.handle, rw, rh, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ, 0);
        di.depth = Res(this->m_fsrDepth.handle, rw, rh, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ, 0);
        di.motionVectors = Res(this->m_motion.handle, rw, rh, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ, 0);
        di.output = Res(this->m_out.handle, nw, nh, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
        di.jitterOffset = { this->m_jitterPx[0], this->m_jitterPx[1] };
        di.motionVectorScale = { static_cast<float>(rw), static_cast<float>(rh) };   // the motion is in render uv
        di.renderSize = { rw, rh };
        di.upscaleSize = { nw, nh };

        LARGE_INTEGER now = {}, freq = {};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const double dt = this->m_lastTime.QuadPart && freq.QuadPart ? double(now.QuadPart - this->m_lastTime.QuadPart) * 1000.0 / double(freq.QuadPart) : 16.7;
        this->m_lastTime = now;
        di.frameTimeDelta = static_cast<float>(dt < 0.1 ? 0.1 : dt > 200.0 ? 200.0 : dt);
        di.preExposure = 1.0f;
        di.reset = reset;
        di.cameraNear = c.m_depthRange[3];
        di.cameraFar = 100000.0f;
        di.cameraFovAngleVertical = this->m_proj.b1 != 0.0f ? 2.0f * atanf(1.0f / fabsf(this->m_proj.b1)) : 1.0f;
        di.viewSpaceToMetersFactor = 0.9144f;
        di.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

        const ffxReturnCode_t r = s_provider.dispatch(&this->m_ctx, &di.header);
        // the provider bound its own pipelines and pushed its own constants on this command buffer
        gxvk::vk::pipelines::ForgetBound();

        if (r != FFX_API_RETURN_OK)
        {
            char line[120];
            snprintf(line, sizeof(line), "ffxDispatch %u (see the 'fsr4 provider' lines)", static_cast<unsigned>(r));
            this->IFail(line);
            return false;
        }

        for (texture::Image* img : ins)
        {
            texture::detail::Transition(cmd, img->handle, 1, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0);
            img->layout = VK_IMAGE_LAYOUT_GENERAL;
        }

        if (!this->m_loggedFirst)
        {
            this->m_loggedFirst = true;
            Write(Info, "vulkan: fsr4: first frame recorded (%ux%u -> %ux%u, jitter %.3f, %.3f px)", rw, rh, nw, nh, this->m_jitterPx[0], this->m_jitterPx[1]);
        }

        return true;
    }

    // The jitter into (on) or out of (off) every perspective projection; the current one changed in place.
    void VulkanUpscale::IJitter(bool on)
    {
        CGxDeviceVulkan* dev = this->m_device;
        const float want[2] = { on ? this->m_jitterNdc[0] : 0.0f, on ? this->m_jitterNdc[1] : 0.0f };
        const float add[2] = { want[0] - dev->m_projJitter[0], want[1] - dev->m_projJitter[1] };
        dev->m_projJitter[0] = want[0];
        dev->m_projJitter[1] = want[1];

        if (dev->m_projNative.d3 == 0.0f && (add[0] != 0.0f || add[1] != 0.0f))
        {
            dev->m_projNative.c0 += add[0];
            dev->m_projNative.c1 += add[1];
            dev->m_xforms[GxXform_Projection].m_dirty = 1;
        }
    }

    void VulkanUpscale::BeginWorld(float& scaleX, float& scaleY)
    {
        scaleX = 1.0f;
        scaleY = 1.0f;
        if (this->m_active) this->EndWorld();

        float scale = s_cv.scale ? s_cv.scale->m_floatValue : 1.0f;
        scale = scale < 0.5f ? 0.5f : scale;
        CGxDeviceVulkan* dev = this->m_device;

        if (this->m_failed || !ModeFsr4() || scale >= 0.999f || !dev || !dev->m_context || !dev->m_drawPath)
        {
            this->m_havePrev = false;
            return;
        }

        if (frame::Target() || frame::QueryActive() || !frame::Scene() || frame::Failed() || !this->ICreate())
        {
            this->m_havePrev = false;
            return;
        }

        if (frame::PassSamples() > 1)
        {
            if (!this->m_loggedMsaa)
            {
                this->m_loggedMsaa = true;
                Write(Warn, "vulkan: fsr4 skipped: the scene is multisampled; set gxMultisample 1 (logged once)");
            }
            return;
        }

        VkViewport vp;
        VkRect2D scissor;
        if (!dev->IPlaceD3d(vp, scissor)) return;

        const uint32_t nw = static_cast<uint32_t>(vp.width), nh = static_cast<uint32_t>(vp.height);
        const uint32_t rw = static_cast<uint32_t>(nw * scale + 0.5f), rh = static_cast<uint32_t>(nh * scale + 0.5f);

        // the provider's bounds (fsr4vk INTEGRATION-CONTRACT.md)
        if (nw > 3840 || nh > 2160 || rw < 64 || rh < 64)
        {
            if (!this->m_loggedSize)
            {
                this->m_loggedSize = true;
                Write(Warn, "vulkan: fsr4 skipped: %ux%u -> %ux%u is outside the provider's 64..3840 x 64..2160 (logged once)", rw, rh, nw, nh);
            }
            return;
        }

        this->m_native = { { static_cast<int32_t>(vp.x), static_cast<int32_t>(vp.y) }, { nw, nh } };
        this->m_render = { rw, rh };
        this->m_minDepth = vp.minDepth;
        this->m_maxDepth = vp.maxDepth;
        scaleX = static_cast<float>(rw) / static_cast<float>(nw);
        scaleY = static_cast<float>(rh) / static_cast<float>(nh);

        // the jitter (wxl-light Depth.cpp OpenJitter, mode 1): FSR's phase count for this ratio
        const float ratio = static_cast<float>(nw) / static_cast<float>(rw);
        uint32_t phases = static_cast<uint32_t>(8.0f * ratio * ratio);
        phases = phases < 8 ? 8 : phases;
        const int i = static_cast<int>(this->m_index++ % phases) + 1;
        this->m_jitterPx[0] = Halton(i, 2) - 0.5f;
        this->m_jitterPx[1] = Halton(i, 3) - 0.5f;
        this->m_jitterNdc[0] = 2.0f * this->m_jitterPx[0] / static_cast<float>(rw);
        this->m_jitterNdc[1] = -2.0f * this->m_jitterPx[1] / static_cast<float>(rh);
        this->IJitter(true);

        this->m_active = true;
        this->m_viewSet = false;
    }

    void VulkanUpscale::SetupView(const C3Vector& origin)
    {
        if (!this->m_active) return;

        CGxDeviceVulkan* dev = this->m_device;
        const CGxMatrixStack& viewStack = dev->m_xforms[GxXform_View];
        this->m_view = viewStack.m_mtx[viewStack.m_level];
        this->m_projJ = dev->m_projNative;
        this->m_proj = dev->m_projNative;

        if (this->m_proj.d3 == 0.0f)
        {
            this->m_proj.c0 -= dev->m_projJitter[0];
            this->m_proj.c1 -= dev->m_projJitter[1];
        }

        this->m_origin = origin;
        this->m_viewSet = true;
    }

    void VulkanUpscale::EndWorld()
    {
        if (!this->m_active) return;
        this->m_active = false;
        this->IJitter(false);

        // the world is in the top-left of its viewport now: from here on it has to be put back at full size
        const texture::Image* scene = frame::Scene();
        if (frame::Target() || frame::QueryActive() || !scene || frame::Failed()) { this->m_havePrev = false; return; }

        const VkExtent2D se = scene->extent;
        if (!this->IEnsure(this->m_sceneOut, se.width, se.height, VK_FORMAT_R8G8B8A8_UNORM, false)) { this->m_havePrev = false; return; }

        bool fsr = this->m_viewSet && !this->m_failed && ModeFsr4()
            && this->IEnsure(this->m_depth, se.width, se.height, frame::DepthFormat(), true)
            && this->IEnsure(this->m_color, this->m_render.width, this->m_render.height, VK_FORMAT_R16G16B16A16_SFLOAT, false)
            && this->IEnsure(this->m_fsrDepth, this->m_render.width, this->m_render.height, VK_FORMAT_R32_SFLOAT, false)
            && this->IEnsure(this->m_motion, this->m_render.width, this->m_render.height, VK_FORMAT_R16G16_SFLOAT, false)
            && this->IEnsure(this->m_out, this->m_native.extent.width, this->m_native.extent.height, VK_FORMAT_R16G16B16A16_SFLOAT, false)
            && this->IContext()
            && frame::CopyDepthFromScene(this->m_depth);

        VkCommandBuffer cmd = frame::Outside();
        if (!cmd) { this->m_havePrev = false; return; }

        this->m_lastUsed = frame::Serial();
        fsr = fsr && this->IFsr(cmd, !this->m_havePrev);

        // FSR's output, or the scene's render-size part stretched, into the viewport
        Constants c;
        this->IConstants(c);
        const VkExtent2D from = fsr ? this->m_out.extent : se;
        c.m_source[0] = 1.0f / static_cast<float>(from.width);
        c.m_source[1] = 1.0f / static_cast<float>(from.height);
        c.m_source[2] = fsr ? 0.0f : 1.0f;
        this->IGeneral(cmd, this->m_sceneOut);
        texture::Image* outs[3] = { &this->m_sceneOut, nullptr, nullptr };
        const bool ok = this->IDispatch(cmd, Prog_Out, c, VK_NULL_HANDLE, fsr ? this->m_out.view : scene->view,
                                        fsr ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, outs,
                                        this->m_native.extent.width, this->m_native.extent.height);

        if (!ok || !frame::CopyIntoScene(this->m_sceneOut, this->m_native))
            Write(Warn, "vulkan: fsr4: the upscale could not be recorded this frame");

        // this frame's camera is the next one's previous
        this->m_havePrev = fsr && this->m_viewSet;
        this->m_prevView = this->m_view;
        this->m_prevProj = this->m_proj;
        this->m_prevOrigin = this->m_origin;
    }

    // After the device is idle (IDestroyVulkan): every object, destroyed at once.
    void VulkanUpscale::Release()
    {
        if (this->m_ctx) s_provider.destroy(&this->m_ctx, nullptr);
        texture::Image* images[] = { &this->m_depth, &this->m_color, &this->m_fsrDepth, &this->m_motion, &this->m_out, &this->m_sceneOut };
        for (texture::Image* img : images) if (img->handle != VK_NULL_HANDLE) texture::DestroyIdle(*img);

        if (this->m_dapi)
        {
            const loader::Device& d = *this->m_dapi;
            for (Slot& slot : this->m_slots)
            {
                if (slot.ubo.handle != VK_NULL_HANDLE) memory::DestroyBuffer(slot.ubo);
                if (slot.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(this->m_vkDevice, slot.pool, nullptr);
            }
            for (VkPipeline pipeline : this->m_pipelines)
                if (pipeline != VK_NULL_HANDLE) d.DestroyPipeline(this->m_vkDevice, pipeline, nullptr);
            if (this->m_linear != VK_NULL_HANDLE) d.DestroySampler(this->m_vkDevice, this->m_linear, nullptr);
            if (this->m_nearest != VK_NULL_HANDLE) d.DestroySampler(this->m_vkDevice, this->m_nearest, nullptr);
            if (this->m_layout != VK_NULL_HANDLE) d.DestroyPipelineLayout(this->m_vkDevice, this->m_layout, nullptr);
            if (this->m_setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(this->m_vkDevice, this->m_setLayout, nullptr);
        }

        if (this->m_device)
        {
            this->m_device->m_projJitter[0] = 0.0f;
            this->m_device->m_projJitter[1] = 0.0f;
        }

        CGxDeviceVulkan* device = this->m_device;
        *this = VulkanUpscale();
        this->m_device = device;
    }
}

namespace gxvk::vk::upscale
{
    bool Wanted()
    {
        RegisterCVars();
        return ModeFsr4();
    }

    const VkDeviceCreateInfo* PrepareDevice(VkInstance instance, VkPhysicalDevice physical, const VkDeviceCreateInfo& info, uint32_t apiVersion)
    {
        s_provider.deviceReady = false;
        if (!Wanted() || !s_provider.Load()) return nullptr;

        if (apiVersion < VK_API_VERSION_1_1)
        {
            Write(Warn, "vulkan: fsr4: the device is Vulkan 1.0; the provider needs 1.1. No upscaler");
            return nullptr;
        }

        const loader::Global& api = loader::Api();
        auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(api.GetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
        auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(api.GetInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
        if (!features2 || !extensions) return nullptr;

        PrepareDeviceDesc prep = {};
        prep.header.type = kPrepareDeviceType;
        prep.physicalDevice = physical;
        prep.sourceCreateInfo = &info;
        prep.apiVersion = apiVersion;
        prep.getPhysicalDeviceFeatures2 = features2;
        prep.enumerateDeviceExtensionProperties = extensions;
        const ffxReturnCode_t r = s_provider.query(nullptr, &prep.header);

        if (r != FFX_API_RETURN_OK || !prep.outputCreateInfo)
        {
            Write(Warn, "vulkan: fsr4: this GPU / driver cannot run the provider (code %u): %s. No upscaler", static_cast<unsigned>(r),
                  prep.errorMessage ? prep.errorMessage : "?");
            s_provider.token = prep.token;
            s_provider.Release();
            return nullptr;
        }

        s_provider.token = prep.token;
        s_provider.apiVersion = apiVersion;
        return prep.outputCreateInfo;
    }

    void DevicePrepared(bool created)
    {
        s_provider.Release();
        s_provider.deviceReady = created;
        if (!created) Write(Warn, "vulkan: fsr4: vkCreateDevice with the provider's features failed; the device without them, no upscaler");
    }

    void Init(CGxDeviceVulkan* device)
    {
        s_upscale.m_device = device;
        if (s_provider.deviceReady) g_gxUpscale = &s_upscale;
    }

    void Shutdown()
    {
        if (g_gxUpscale == &s_upscale) g_gxUpscale = nullptr;
        s_upscale.Release();
        s_upscale.m_device = nullptr;
    }
}
