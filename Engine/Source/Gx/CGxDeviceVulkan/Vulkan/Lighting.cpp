// LIGHTING: see Lighting.hpp. The constants and the pass order follow D3d11Lighting.cpp; keep the two in step.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Lighting.hpp"

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
#include "Engine/Source/Gx/GxLighting.h"
#include "Engine/Source/Tempest/C44Matrix.h"

#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/LightingSpirv.inc"
#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/PresentSpirv.inc"

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;
    namespace pipelines = gxvk::vk::pipelines;
    namespace texture = gxvk::vk::texture;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Warn;
    using gxvk::log::Error;

    /// Shaders/Lighting.glsl GxPost (std140: every member a vec4 or a mat4, so the C layout is the same)
    struct PostConstants
    {
        float m_invProj[16];
        float m_invView[16];
        float m_viewport[4];
        float m_depthRange[4];
        float m_target[4];
        float m_ao[4];
        float m_aoProj[4];
        float m_fogColor[4];
        float m_fogParams[4];
        float m_sunColor[4];
        float m_sunDir[4];
        float m_heightFog[4];
        float m_camera[4];
        float m_bloom[4];
        float m_tone[4];
        float m_source[4];
        float m_projW[4];
        float m_pass[4];
    };

    enum Prog { Prog_Ao, Prog_Blur, Prog_Apply, Prog_Bloom, Prog_Tonemap, Prog_Count };

    constexpr uint32_t kBloomLevels = 6;
    constexpr uint32_t kMaxSets     = 32;     ///< dispatches one frame slot may record (a frame uses up to 17)
    constexpr VkDeviceSize kUboStride = 512;  ///< >= sizeof(PostConstants), a multiple of any minUniformBufferOffsetAlignment (<= 256)
    constexpr uint32_t kGroup       = 8;      ///< Lighting.glsl local_size_x / _y
    constexpr uint32_t kCopyPasses  = 4;      ///< scene render passes (MSAA, with or without the normal target) one generation may hand out

    static_assert(sizeof(PostConstants) <= kUboStride, "PostConstants outgrew its slot");

    struct Slot
    {
        uint64_t         serial = 0;
        VkDescriptorPool pool   = VK_NULL_HANDLE;
        memory::Buffer   ubo;
        uint32_t         used   = 0;
    };

    /// A sampled input: a view and the layout it rests in. Null view: not bound.
    struct Input
    {
        VkImageView   view   = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };

    class VulkanLighting final : public IGxLighting
    {
        public:
        void BeginWorld(const GxLightingParams& params) override;
        void SetupView(const GxLightingParams& params) override;
        void OpaqueDone() override;
        void EndWorld() override;
        void Release();

        CGxDeviceVulkan* m_device = nullptr;

        private:
        GxLightingParams m_params;
        bool m_worldActive = false;
        bool m_viewSet = false;
        bool m_failed = false;
        bool m_loggedCopy = false;
        VkViewport m_viewport = {};
        float m_viewInv[16] = {};
        float m_proj[16] = {};
        float m_projInv[16] = {};

        VkDevice m_vkDevice = VK_NULL_HANDLE;
        const loader::Device* m_dapi = nullptr;
        VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_layout = VK_NULL_HANDLE;
        VkPipeline m_pipelines[Prog_Count] = {};
        VkSampler m_linear = VK_NULL_HANDLE;
        VkSampler m_nearest = VK_NULL_HANDLE;
        Slot m_slots[frame::kFramesInFlight];

        // MSAA: the copy back into the multisampled scene (Shaders/LightingCopy.frag), one pipeline per scene
        // pass, all dropped with the pass generation (the frame made the device idle before it changed)
        VkShaderModule m_copyVert = VK_NULL_HANDLE;
        VkShaderModule m_copyFrag = VK_NULL_HANDLE;
        VkRenderPass m_copyPass[kCopyPasses] = {};
        VkPipeline m_copyPipeline[kCopyPasses] = {};
        uint32_t m_copyGen = 0;

        texture::Image m_depth;               ///< the scene depth copy
        texture::Image m_out;                 ///< RGBA8 at the scene's extent, B, G, R, A: what is copied into the scene
        texture::Image m_ao[2];               ///< RGBA16F at half the world viewport (rg16f would need shaderStorageImageExtendedFormats)
        texture::Image m_bloom[kBloomLevels]; ///< RGBA16F, halved level by level from the scene's extent
        uint64_t m_lastUsed = 0;

        bool ICreate();
        bool IGate();
        bool IEnsure(texture::Image& img, uint32_t width, uint32_t height, VkFormat format, bool depth);
        void IConstants(PostConstants& c, uint32_t targetWidth, uint32_t targetHeight, const float* source) const;
        VkDescriptorSet ISet(const PostConstants& c, VkImageView depth, Input color, Input color2, VkImageView out);
        bool IDispatch(VkCommandBuffer cmd, Prog prog, const PostConstants& c, VkImageView depth, Input color, Input color2, texture::Image& out, uint32_t width, uint32_t height);
        VkPipeline ICopyPipeline();
        bool IIntoScene(VkCommandBuffer cmd, const PostConstants& c, const VkRect2D& vp);
        VkRect2D IViewportRect() const;
        void IToGeneral(VkCommandBuffer cmd, texture::Image& img);
        void IBarrier(VkCommandBuffer cmd);
    };

    VulkanLighting s_lighting;

    bool VulkanLighting::ICreate()
    {
        if (this->m_failed) return false;
        if (this->m_layout != VK_NULL_HANDLE) return true;

        const context::Handles* h = context::Get();
        if (!h || !h->dapi || !h->iapi) return false;

        uint32_t familyCount = 0;
        h->iapi->GetPhysicalDeviceQueueFamilyProperties(h->physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        if (familyCount) h->iapi->GetPhysicalDeviceQueueFamilyProperties(h->physical, &familyCount, families.data());
        if (h->graphicsFamily >= familyCount || !(families[h->graphicsFamily].queueFlags & VK_QUEUE_COMPUTE_BIT))
        {
            Write(Warn, "vulkan: lighting: the graphics queue cannot run compute; the lighting passes stay off");
            this->m_failed = true;
            return false;
        }

        this->m_vkDevice = h->device;
        this->m_dapi = h->dapi;
        const loader::Device& d = *this->m_dapi;

        VkDescriptorSetLayoutBinding bindings[5] = {};
        for (uint32_t i = 0; i < 5; i++)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : (i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;   // fragment: LightingCopy.frag
        }
        VkDescriptorSetLayoutCreateInfo setInfo = {};
        setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = 5;
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
        const Code code[Prog_Count] = {
            { kLightingAoComp, sizeof kLightingAoComp },
            { kLightingBlurComp, sizeof kLightingBlurComp },
            { kLightingApplyComp, sizeof kLightingApplyComp },
            { kLightingBloomComp, sizeof kLightingBloomComp },
            { kLightingTonemapComp, sizeof kLightingTonemapComp },
        };

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

        if (r == VK_SUCCESS)
        {
            VkShaderModuleCreateInfo moduleInfo = {};
            moduleInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            moduleInfo.codeSize = sizeof kPresentVert;
            moduleInfo.pCode    = kPresentVert;
            r = d.CreateShaderModule(this->m_vkDevice, &moduleInfo, nullptr, &this->m_copyVert);
            moduleInfo.codeSize = sizeof kLightingCopyFrag;
            moduleInfo.pCode    = kLightingCopyFrag;
            if (r == VK_SUCCESS) r = d.CreateShaderModule(this->m_vkDevice, &moduleInfo, nullptr, &this->m_copyFrag);
        }

        for (uint32_t i = 0; r == VK_SUCCESS && i < 2; i++)
        {
            VkSamplerCreateInfo info = {};
            info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            info.magFilter    = i ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            info.minFilter    = info.magFilter;
            info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.maxLod       = 0.0f;
            r = d.CreateSampler(this->m_vkDevice, &info, nullptr, i ? &this->m_nearest : &this->m_linear);
        }

        VkDescriptorPoolSize sizes[3] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kMaxSets },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxSets },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxSets },
        };
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = kMaxSets;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes    = sizes;

        for (Slot& slot : this->m_slots)
        {
            if (r == VK_SUCCESS) r = d.CreateDescriptorPool(this->m_vkDevice, &poolInfo, nullptr, &slot.pool);
            if (r == VK_SUCCESS && !memory::CreateBuffer(kMaxSets * kUboStride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, slot.ubo)) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            if (r == VK_SUCCESS && !slot.ubo.block.mapped) r = VK_ERROR_MEMORY_MAP_FAILED;
        }

        if (r != VK_SUCCESS)
        {
            Write(Error, "vulkan: lighting: the post passes could not be made: %s; lighting stays off", gxvk::vk::ResultName(r));
            this->Release();
            this->m_failed = true;
            return false;
        }

        Write(Debug, "vulkan: lighting: post passes ready");
        return true;
    }

    // The scene must be the target, unpinned by a query.
    bool VulkanLighting::IGate()
    {
        if (frame::Target() || frame::QueryActive() || !frame::Scene() || frame::Failed()) return false;
        return this->ICreate();
    }

    bool VulkanLighting::IEnsure(texture::Image& img, uint32_t width, uint32_t height, VkFormat format, bool depth)
    {
        width = width ? width : 1;
        height = height ? height : 1;

        if (img.handle != VK_NULL_HANDLE && img.extent.width == width && img.extent.height == height && img.format == format) return true;
        if (img.handle != VK_NULL_HANDLE) frame::RetireImage(img, this->m_lastUsed);

        img = texture::Image();
        const bool ok = depth ? texture::CreateDepthCopy(width, height, format, img) : texture::CreateStorage(width, height, format, img);
        if (!ok) img = texture::Image();
        return ok;
    }

    VkRect2D VulkanLighting::IViewportRect() const
    {
        const VkExtent2D scene = frame::Scene()->extent;
        VkRect2D rect = { { static_cast<int32_t>(this->m_viewport.x), static_cast<int32_t>(this->m_viewport.y) },
                          { static_cast<uint32_t>(this->m_viewport.width), static_cast<uint32_t>(this->m_viewport.height) } };

        if (rect.extent.width == 0 || rect.extent.height == 0 || rect.offset.x < 0 || rect.offset.y < 0
            || rect.offset.x + rect.extent.width > scene.width || rect.offset.y + rect.extent.height > scene.height)
            rect = { { 0, 0 }, scene };

        return rect;
    }

    void VulkanLighting::IConstants(PostConstants& c, uint32_t targetWidth, uint32_t targetHeight, const float* source) const
    {
        memset(&c, 0, sizeof(c));
        const GxLightingParams& p = this->m_params;
        memcpy(c.m_invProj, this->m_projInv, sizeof(c.m_invProj));
        memcpy(c.m_invView, this->m_viewInv, sizeof(c.m_invView));

        const VkRect2D vp = this->IViewportRect();
        c.m_viewport[0] = static_cast<float>(vp.offset.x);
        c.m_viewport[1] = static_cast<float>(vp.offset.y);
        c.m_viewport[2] = static_cast<float>(vp.extent.width);
        c.m_viewport[3] = static_cast<float>(vp.extent.height);
        c.m_depthRange[0] = this->m_viewport.minDepth;
        c.m_depthRange[1] = this->m_viewport.maxDepth;
        c.m_depthRange[2] = this->m_viewport.maxDepth > this->m_viewport.minDepth ? 1.0f / (this->m_viewport.maxDepth - this->m_viewport.minDepth) : 1.0f;
        c.m_target[0] = static_cast<float>(targetWidth);
        c.m_target[1] = static_cast<float>(targetHeight);
        c.m_target[2] = targetWidth ? 1.0f / targetWidth : 0.0f;
        c.m_target[3] = targetHeight ? 1.0f / targetHeight : 0.0f;

        c.m_ao[0] = p.m_ssaoRadius;
        c.m_ao[1] = p.m_ssaoIntensity;
        c.m_ao[2] = p.m_ssaoDistance * 0.6f;
        c.m_ao[3] = p.m_ssaoDistance;
        c.m_aoProj[0] = 0.5f * c.m_viewport[3] * fabsf(this->m_proj[5]);
        const float nearPlane = this->m_proj[10] != 0.0f ? -this->m_proj[14] / this->m_proj[10] : 0.0f;
        // the depth buffer's step: D16 or 24 bits (D32 matches D24 where the scene's depths are, as the bias in CGxDeviceVulkanDraw.cpp)
        const float steps = frame::DepthFormat() == VK_FORMAT_D16_UNORM ? 65536.0f : 16777216.0f;
        c.m_aoProj[1] = nearPlane > 0.0f ? c.m_depthRange[2] / (steps * nearPlane) : 0.0f;

        c.m_fogColor[0] = p.m_fogColor.x;
        c.m_fogColor[1] = p.m_fogColor.y;
        c.m_fogColor[2] = p.m_fogColor.z;
        c.m_fogColor[3] = (p.m_features & GxLighting_Scattering) ? p.m_scattering : 0.0f;
        c.m_fogParams[0] = p.m_fogStart;
        c.m_fogParams[1] = p.m_fogEnd;
        c.m_fogParams[2] = p.m_fogDensity;
        c.m_fogParams[3] = p.m_fogEnd > p.m_fogStart ? 1.0f : 0.0f;
        c.m_sunColor[0] = p.m_sunColor.x;
        c.m_sunColor[1] = p.m_sunColor.y;
        c.m_sunColor[2] = p.m_sunColor.z;
        c.m_sunDir[0] = -p.m_sunDirection.x;
        c.m_sunDir[1] = -p.m_sunDirection.y;
        c.m_sunDir[2] = -p.m_sunDirection.z;
        c.m_heightFog[0] = (p.m_features & GxLighting_HeightFog) ? p.m_heightFogDensity : 0.0f;
        c.m_heightFog[1] = p.m_heightFogFalloff;
        c.m_heightFog[2] = p.m_heightFogBase;
        c.m_heightFog[3] = p.m_heightFogMax;
        c.m_camera[0] = p.m_cameraPosition.x;
        c.m_camera[1] = p.m_cameraPosition.y;
        c.m_camera[2] = p.m_cameraPosition.z;
        c.m_camera[3] = p.m_time;

        c.m_bloom[0] = p.m_bloomThreshold;
        c.m_bloom[1] = p.m_bloomThreshold * 0.5f;
        c.m_bloom[2] = p.m_bloomIntensity;
        c.m_bloom[3] = (p.m_features & GxLighting_Bloom) && p.m_bloomIntensity > 0.0f ? 1.0f : 0.0f;
        c.m_tone[0] = p.m_exposure;
        c.m_tone[1] = static_cast<float>(p.m_tonemap);
        c.m_tone[2] = static_cast<float>(p.m_debug);

        if (source) memcpy(c.m_source, source, sizeof(c.m_source));

        c.m_projW[0] = this->m_proj[3];
        c.m_projW[1] = this->m_proj[7];
        c.m_projW[2] = this->m_proj[11];
        c.m_projW[3] = this->m_proj[15];
    }

    // A storage image made this frame (or retired and remade) holds nothing worth keeping: UNDEFINED to GENERAL,
    // where it stays (written as storage, sampled in GENERAL, copied from by frame::CopyIntoScene and back).
    void VulkanLighting::IToGeneral(VkCommandBuffer cmd, texture::Image& img)
    {
        if (img.handle == VK_NULL_HANDLE || img.layout == VK_IMAGE_LAYOUT_GENERAL) return;
        texture::detail::Transition(cmd, img.handle, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        img.layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // Every earlier write (the scene's pass, the depth copy, the previous dispatch) before the next dispatch.
    void VulkanLighting::IBarrier(VkCommandBuffer cmd)
    {
        VkMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        this->m_dapi->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    VkDescriptorSet VulkanLighting::ISet(const PostConstants& c, VkImageView depth, Input color, Input color2, VkImageView out)
    {
        Slot& slot = this->m_slots[frame::Slot()];
        const uint64_t serial = frame::Serial();
        const loader::Device& d = *this->m_dapi;

        if (slot.serial != serial)
        {
            // the slot's previous frame has completed: the frame waited on its fence to open
            d.ResetDescriptorPool(this->m_vkDevice, slot.pool, 0);
            slot.serial = serial;
            slot.used = 0;
        }

        if (slot.used == kMaxSets) return VK_NULL_HANDLE;

        VkDescriptorSetAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = slot.pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &this->m_setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (d.AllocateDescriptorSets(this->m_vkDevice, &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;

        const VkDeviceSize offset = slot.used++ * kUboStride;
        memcpy(static_cast<uint8_t*>(slot.ubo.block.mapped) + offset, &c, sizeof(c));
        memory::FlushHost(slot.ubo.block);

        VkDescriptorImageInfo images[4] = {};
        images[0] = { this->m_nearest, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        images[1] = { this->m_linear, color.view, color.layout };
        images[2] = { this->m_linear, color2.view, color2.layout };
        images[3] = { VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo ubo = { slot.ubo.handle, offset, sizeof(PostConstants) };

        // only what the pass reads: a binding no code of the pipeline names needs no descriptor
        VkWriteDescriptorSet writes[5] = {};
        uint32_t count = 0;

        for (uint32_t i = 0; i < 5; i++)
        {
            if (i < 4 && images[i].imageView == VK_NULL_HANDLE) continue;
            VkWriteDescriptorSet& w = writes[count++];
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = set;
            w.dstBinding      = i;
            w.descriptorCount = 1;
            w.descriptorType  = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : (i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            if (i < 4) w.pImageInfo = &images[i];
            else w.pBufferInfo = &ubo;
        }

        d.UpdateDescriptorSets(this->m_vkDevice, count, writes, 0, nullptr);
        return set;
    }

    bool VulkanLighting::IDispatch(VkCommandBuffer cmd, Prog prog, const PostConstants& c, VkImageView depth, Input color, Input color2, texture::Image& out, uint32_t width, uint32_t height)
    {
        const VkDescriptorSet set = this->ISet(c, depth, color, color2, out.view);
        if (set == VK_NULL_HANDLE) return false;

        const loader::Device& d = *this->m_dapi;
        this->IBarrier(cmd);
        d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->m_pipelines[prog]);
        d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, this->m_layout, 0, 1, &set, 0, nullptr);
        d.CmdDispatch(cmd, (width + kGroup - 1) / kGroup, (height + kGroup - 1) / kGroup, 1);
        return true;
    }

    // The pipeline that draws m_out into the current (scene) pass, built on first use against that pass.
    VkPipeline VulkanLighting::ICopyPipeline()
    {
        const loader::Device& d = *this->m_dapi;
        const VkRenderPass pass = frame::RenderPass();
        if (pass == VK_NULL_HANDLE) return VK_NULL_HANDLE;

        if (this->m_copyGen != frame::PassGeneration())
        {
            // the passes these were built against are gone, and the device was idle when they went
            for (uint32_t i = 0; i < kCopyPasses; i++)
            {
                if (this->m_copyPipeline[i] != VK_NULL_HANDLE) d.DestroyPipeline(this->m_vkDevice, this->m_copyPipeline[i], nullptr);
                this->m_copyPipeline[i] = VK_NULL_HANDLE;
                this->m_copyPass[i] = VK_NULL_HANDLE;
            }
            this->m_copyGen = frame::PassGeneration();
        }

        uint32_t i = 0;
        for (; i < kCopyPasses && this->m_copyPass[i] != VK_NULL_HANDLE; i++)
            if (this->m_copyPass[i] == pass) return this->m_copyPipeline[i];
        if (i == kCopyPasses) return VK_NULL_HANDLE;

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = this->m_copyVert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = this->m_copyFrag;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly = {};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport = {};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;
        VkPipelineRasterizationStateCreateInfo raster = {};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.lineWidth   = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(frame::PassSamples());
        VkPipelineDepthStencilStateCreateInfo depth = {};   // no test, no write
        depth.sType          = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.maxDepthBounds = 1.0f;
        // the scene colour whole (as the single-sample copy writes it); GBUFFER's normal target untouched
        VkPipelineColorBlendAttachmentState attachments[2] = {};
        attachments[0].colorWriteMask = 0xFu;
        VkPipelineColorBlendStateCreateInfo blending = {};
        blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blending.attachmentCount = frame::PassHasNormal() ? 2u : 1u;
        blending.pAttachments    = attachments;
        const VkDynamicState dynamics[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic = {};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamics;

        VkGraphicsPipelineCreateInfo info = {};
        info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount          = 2;
        info.pStages             = stages;
        info.pVertexInputState   = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState      = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pDepthStencilState  = &depth;
        info.pColorBlendState    = &blending;
        info.pDynamicState       = &dynamic;
        info.layout              = this->m_layout;
        info.renderPass          = pass;

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult r = d.CreateGraphicsPipelines(this->m_vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (r != VK_SUCCESS)
        {
            if (!this->m_loggedCopy) Write(Error, "vulkan: lighting: the MSAA copy pipeline could not be made: %s (logged once)", gxvk::vk::ResultName(r));
            this->m_loggedCopy = true;
            return VK_NULL_HANDLE;
        }

        this->m_copyPass[i] = pass;
        this->m_copyPipeline[i] = pipeline;
        return pipeline;
    }

    // m_out (just written by `cmd`'s last dispatch) into the world viewport of the scene. Single-sampled: a
    // copy. MSAA: every pass instance resolves the multisampled colour over the scene, so the copy would be
    // lost at the next resume; m_out is drawn into the multisampled colour instead, in the scene's pass, and
    // reaches the scene with that pass's resolve (the draws after it keep their MSAA).
    bool VulkanLighting::IIntoScene(VkCommandBuffer cmd, const PostConstants& c, const VkRect2D& vp)
    {
        if (frame::PassSamples() <= 1) return frame::CopyIntoScene(this->m_out, vp);

        const VkPipeline pipeline = this->ICopyPipeline();
        const VkDescriptorSet set = pipeline != VK_NULL_HANDLE ? this->ISet(c, VK_NULL_HANDLE, { this->m_out.view, VK_IMAGE_LAYOUT_GENERAL }, {}, VK_NULL_HANDLE) : VK_NULL_HANDLE;
        if (set == VK_NULL_HANDLE) return false;

        // the dispatch's write before the fragment reads, outside the pass
        const loader::Device& d = *this->m_dapi;
        VkMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

        VkCommandBuffer pass = frame::Pass();
        if (!pass) return false;

        const VkExtent2D scene = frame::Scene()->extent;
        const VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(scene.width), static_cast<float>(scene.height), 0.0f, 1.0f };
        d.CmdBindPipeline(pass, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        d.CmdBindDescriptorSets(pass, VK_PIPELINE_BIND_POINT_GRAPHICS, this->m_layout, 0, 1, &set, 0, nullptr);
        d.CmdSetViewport(pass, 0, 1, &viewport);
        d.CmdSetScissor(pass, 0, 1, &vp);
        d.CmdDraw(pass, 3, 1, 0, 0);
        pipelines::Unbind();   // the draw path's binding cache no longer says what is bound
        return true;
    }

    void VulkanLighting::BeginWorld(const GxLightingParams& params)
    {
        if (this->m_worldActive) this->EndWorld();
        if (!this->m_device || !this->m_device->m_context || !this->m_device->m_drawPath) return;

        this->m_params = params;
        this->m_worldActive = true;
        this->m_viewSet = false;

        // the world viewport, as the draws will place it (CGWorldFrame::OnWorldRender has just set it)
        VkRect2D scissor;
        if (!this->m_device->IPlaceD3d(this->m_viewport, scissor)) this->m_viewport = {};
    }

    void VulkanLighting::SetupView(const GxLightingParams& params)
    {
        if (!this->m_worldActive) return;

        CGxDeviceVulkan* dev = this->m_device;
        this->m_params = params;
        const CGxMatrixStack& viewStack = dev->m_xforms[GxXform_View];
        const C44Matrix& view = viewStack.m_mtx[viewStack.m_level];
        C44Matrix viewInv = view.Inverse();
        C44Matrix projInv = dev->m_projNative.Inverse();
        memcpy(this->m_viewInv, &viewInv, sizeof(this->m_viewInv));
        memcpy(this->m_proj, &dev->m_projNative, sizeof(this->m_proj));
        memcpy(this->m_projInv, &projInv, sizeof(this->m_projInv));
        this->m_viewSet = true;
    }

    void VulkanLighting::OpaqueDone()
    {
        const GxLightingParams& p = this->m_params;
        const bool ssao = (p.m_features & GxLighting_Ssao) && p.m_ssaoIntensity > 0.0f;
        const bool fog = (p.m_features & (GxLighting_HeightFog | GxLighting_Scattering)) != 0;

        if (!this->m_worldActive || !this->m_viewSet || (!ssao && !fog) || !this->IGate()) return;

        const texture::Image* scene = frame::Scene();
        const uint32_t width = scene->extent.width;
        const uint32_t height = scene->extent.height;

        if (!this->IEnsure(this->m_depth, width, height, frame::DepthFormat(), true) || !frame::CopyDepthFromScene(this->m_depth)) return;

        const VkRect2D vp = this->IViewportRect();
        const uint32_t aoWidth = (vp.extent.width + 1) / 2;
        const uint32_t aoHeight = (vp.extent.height + 1) / 2;
        const bool aoOk = !ssao || (this->IEnsure(this->m_ao[0], aoWidth, aoHeight, VK_FORMAT_R16G16B16A16_SFLOAT, false) && this->IEnsure(this->m_ao[1], aoWidth, aoHeight, VK_FORMAT_R16G16B16A16_SFLOAT, false));
        if (!aoOk || !this->IEnsure(this->m_out, width, height, VK_FORMAT_R8G8B8A8_UNORM, false)) return;

        VkCommandBuffer cmd = frame::Outside();
        if (!cmd) return;

        this->m_lastUsed = frame::Serial();
        this->IToGeneral(cmd, this->m_out);
        const Input sceneIn = { scene->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        PostConstants c;
        float source[4] = { 1.0f / aoWidth, 1.0f / aoHeight, 0.0f, 0.0f };
        bool ok = true;

        if (ssao)
        {
            this->IToGeneral(cmd, this->m_ao[0]);
            this->IToGeneral(cmd, this->m_ao[1]);
            const Input ao0 = { this->m_ao[0].view, VK_IMAGE_LAYOUT_GENERAL };
            const Input ao1 = { this->m_ao[1].view, VK_IMAGE_LAYOUT_GENERAL };

            this->IConstants(c, aoWidth, aoHeight, source);
            ok = this->IDispatch(cmd, Prog_Ao, c, this->m_depth.view, {}, {}, this->m_ao[0], aoWidth, aoHeight);
            source[2] = 1.0f;
            this->IConstants(c, aoWidth, aoHeight, source);
            ok = ok && this->IDispatch(cmd, Prog_Blur, c, VK_NULL_HANDLE, ao0, {}, this->m_ao[1], aoWidth, aoHeight);
            source[2] = 0.0f;
            source[3] = 1.0f;
            this->IConstants(c, aoWidth, aoHeight, source);
            ok = ok && this->IDispatch(cmd, Prog_Blur, c, VK_NULL_HANDLE, ao1, {}, this->m_ao[0], aoWidth, aoHeight);
            source[3] = 0.0f;
        }

        // the apply pass names gxColor in code either way: the scene stands in for the occlusion when SSAO is off
        this->IConstants(c, width, height, source);
        c.m_pass[0] = ssao ? 1.0f : 0.0f;
        c.m_pass[1] = fog ? 1.0f : 0.0f;
        const Input aoIn = ssao ? Input{ this->m_ao[0].view, VK_IMAGE_LAYOUT_GENERAL } : sceneIn;
        ok = ok && this->IDispatch(cmd, Prog_Apply, c, this->m_depth.view, aoIn, sceneIn, this->m_out, vp.extent.width, vp.extent.height);

        if (!ok || !this->IIntoScene(cmd, c, vp))
            Write(Warn, "vulkan: lighting: the SSAO / fog passes could not be recorded this frame");
    }

    void VulkanLighting::EndWorld()
    {
        if (!this->m_worldActive) return;
        this->m_worldActive = false;

        const GxLightingParams& p = this->m_params;
        if (!(p.m_features & GxLighting_Hdr) || !this->IGate()) return;

        const texture::Image* scene = frame::Scene();
        const uint32_t width = scene->extent.width;
        const uint32_t height = scene->extent.height;
        if (!this->IEnsure(this->m_out, width, height, VK_FORMAT_R8G8B8A8_UNORM, false)) return;

        uint32_t levels = 0;

        if ((p.m_features & GxLighting_Bloom) && p.m_bloomIntensity > 0.0f)
        {
            uint32_t w = width;
            uint32_t h = height;

            for (; levels < kBloomLevels; levels++)
            {
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
                if (!this->IEnsure(this->m_bloom[levels], w, h, VK_FORMAT_R16G16B16A16_SFLOAT, false)) break;
            }
        }

        VkCommandBuffer cmd = frame::Outside();
        if (!cmd) return;

        this->m_lastUsed = frame::Serial();
        this->IToGeneral(cmd, this->m_out);
        const Input sceneIn = { scene->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        PostConstants c;
        bool ok = true;

        for (uint32_t i = 0; ok && i < levels; i++)
        {
            // down: the bright part of the scene to half size, then halved again level by level
            texture::Image& to = this->m_bloom[i];
            this->IToGeneral(cmd, to);
            const Input from = i ? Input{ this->m_bloom[i - 1].view, VK_IMAGE_LAYOUT_GENERAL } : sceneIn;
            const VkExtent2D fromSize = i ? this->m_bloom[i - 1].extent : scene->extent;
            const float source[4] = { 1.0f / fromSize.width, 1.0f / fromSize.height, 0.0f, 0.0f };
            this->IConstants(c, to.extent.width, to.extent.height, source);
            c.m_pass[0] = i ? 1.0f : 0.0f;
            ok = this->IDispatch(cmd, Prog_Bloom, c, VK_NULL_HANDLE, from, {}, to, to.extent.width, to.extent.height);
        }

        for (uint32_t i = levels ? levels - 1 : 0; ok && i > 0; i--)
        {
            // up: each level spread and added into the one above
            texture::Image& from = this->m_bloom[i];
            texture::Image& to = this->m_bloom[i - 1];
            const float source[4] = { 1.0f / from.extent.width, 1.0f / from.extent.height, 0.0f, 0.0f };
            this->IConstants(c, to.extent.width, to.extent.height, source);
            c.m_pass[0] = 2.0f;
            ok = this->IDispatch(cmd, Prog_Bloom, c, VK_NULL_HANDLE, { from.view, VK_IMAGE_LAYOUT_GENERAL }, {}, to, to.extent.width, to.extent.height);
        }

        // the tonemap over the world viewport of the scene; z the bloom levels summed
        const VkRect2D vp = this->IViewportRect();
        const float source[4] = { 1.0f / width, 1.0f / height, static_cast<float>(levels ? levels : 1), 0.0f };
        this->IConstants(c, width, height, source);
        if (!levels || !ok) c.m_bloom[3] = 0.0f;
        const Input bloomIn = levels && ok ? Input{ this->m_bloom[0].view, VK_IMAGE_LAYOUT_GENERAL } : sceneIn;
        ok = this->IDispatch(cmd, Prog_Tonemap, c, VK_NULL_HANDLE, sceneIn, bloomIn, this->m_out, vp.extent.width, vp.extent.height);

        if (!ok || !this->IIntoScene(cmd, c, vp))
            Write(Warn, "vulkan: lighting: the tonemap could not be recorded this frame");
    }

    // After the device is idle (IDestroyVulkan): every object, destroyed at once.
    void VulkanLighting::Release()
    {
        texture::Image* images[] = { &this->m_depth, &this->m_out, &this->m_ao[0], &this->m_ao[1] };
        for (texture::Image* img : images) if (img->handle != VK_NULL_HANDLE) texture::DestroyIdle(*img);
        for (texture::Image& img : this->m_bloom) if (img.handle != VK_NULL_HANDLE) texture::DestroyIdle(img);

        if (this->m_dapi)
        {
            const loader::Device& d = *this->m_dapi;
            for (Slot& slot : this->m_slots)
            {
                if (slot.ubo.handle != VK_NULL_HANDLE) memory::DestroyBuffer(slot.ubo);
                if (slot.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(this->m_vkDevice, slot.pool, nullptr);
                slot = Slot();
            }
            for (VkPipeline& pipeline : this->m_pipelines)
                if (pipeline != VK_NULL_HANDLE) d.DestroyPipeline(this->m_vkDevice, pipeline, nullptr);
            for (VkPipeline& pipeline : this->m_copyPipeline)
                if (pipeline != VK_NULL_HANDLE) d.DestroyPipeline(this->m_vkDevice, pipeline, nullptr);
            if (this->m_copyVert != VK_NULL_HANDLE) d.DestroyShaderModule(this->m_vkDevice, this->m_copyVert, nullptr);
            if (this->m_copyFrag != VK_NULL_HANDLE) d.DestroyShaderModule(this->m_vkDevice, this->m_copyFrag, nullptr);
            if (this->m_linear != VK_NULL_HANDLE) d.DestroySampler(this->m_vkDevice, this->m_linear, nullptr);
            if (this->m_nearest != VK_NULL_HANDLE) d.DestroySampler(this->m_vkDevice, this->m_nearest, nullptr);
            if (this->m_layout != VK_NULL_HANDLE) d.DestroyPipelineLayout(this->m_vkDevice, this->m_layout, nullptr);
            if (this->m_setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(this->m_vkDevice, this->m_setLayout, nullptr);
        }

        CGxDeviceVulkan* device = this->m_device;
        *this = VulkanLighting();
        this->m_device = device;
    }
}

namespace gxvk::vk::lighting
{
    void Init(CGxDeviceVulkan* device)
    {
        s_lighting.m_device = device;
        g_gxLighting = &s_lighting;
    }

    void Shutdown()
    {
        if (g_gxLighting == &s_lighting) g_gxLighting = nullptr;
        s_lighting.Release();
        s_lighting.m_device = nullptr;
    }
}
