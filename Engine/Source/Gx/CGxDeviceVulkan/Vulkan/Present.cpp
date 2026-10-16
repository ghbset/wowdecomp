// The present pass: the scene image onto the acquired swapchain image through the gamma ramp.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Present.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/PresentSpirv.inc"

#include <cstring>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Warn;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;
    using gxvk::vk::FormatName;

    constexpr uint32_t kMaxImages = 8;
    constexpr uint32_t kSlots     = frame::kFramesInFlight;

    /// The gamma block of Shaders/Present.frag: vec4 lut[256], vec4 params.
    constexpr uint32_t kLutEntries = 256;
    constexpr uint32_t kGammaBytes = (kLutEntries + 1) * 4 * sizeof(float);

    struct Slot
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        memory::Buffer  gamma;
        uint32_t        gammaGen = UINT32_MAX;   ///< the ramp generation the buffer holds
    };

    struct State
    {
        bool                  ready  = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        VkShaderModule        vert   = VK_NULL_HANDLE;
        VkShaderModule        frag   = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout      layout    = VK_NULL_HANDLE;
        VkDescriptorPool      pool      = VK_NULL_HANDLE;
        VkSampler             sampler   = VK_NULL_HANDLE;
        Slot                  slots[kSlots];

        // --- per swapchain -----------------------------------------------------------------------
        VkFormat      format   = VK_FORMAT_UNDEFINED;
        VkRenderPass  pass     = VK_NULL_HANDLE;
        VkPipeline    pipeline = VK_NULL_HANDLE;
        VkExtent2D    extent   = { 0, 0 };
        VkFramebuffer fbs[kMaxImages] = {};
        uint32_t      fbCount  = 0;
        VkImageView   scene    = VK_NULL_HANDLE;

        /// The last Rebuild() asked for, kept so an Init() that comes after it can apply it.
        bool        pendingRebuild = false;
        VkFormat    pendingFormat  = VK_FORMAT_UNDEFINED;
        VkExtent2D  pendingExtent  = { 0, 0 };
        VkImageView pendingViews[kMaxImages] = {};
        uint32_t    pendingCount   = 0;
        VkImageView pendingScene   = VK_NULL_HANDLE;

        // --- the ramp ----------------------------------------------------------------------------
        float    lut[kGammaBytes / sizeof(float)] = {};
        uint32_t gammaGen = 0;
        bool     applies  = false;

        bool loggedNoRecord = false;
    };
    State s;

    bool CreateModule(const uint32_t* words, size_t bytes, VkShaderModule& out, const char* what)
    {
        VkShaderModuleCreateInfo info = {};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes;
        info.pCode    = words;
        const VkResult r = s.dapi->CreateShaderModule(s.device, &info, nullptr, &out);
        if (r != VK_SUCCESS)
        {
            out = VK_NULL_HANDLE;
            Write(Error, "vulkan: present: vkCreateShaderModule (%s) failed: %s", what, ResultName(r));
            return false;
        }
        return true;
    }

    /// The pass over the swapchain's format: the whole image is written, so its contents are not
    /// loaded; it ends ready to present. The colour output waits for the acquire (the submit's
    /// wait stage) and for the scene's last pass through the scene pass's own outgoing dependency.
    bool CreatePass(VkFormat format)
    {
        VkAttachmentDescription a = {};
        a.format         = format;
        a.samples        = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency dep = {};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo info = {};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments    = &a;
        info.subpassCount    = 1;
        info.pSubpasses      = &sub;
        info.dependencyCount = 1;
        info.pDependencies   = &dep;
        const VkResult r = s.dapi->CreateRenderPass(s.device, &info, nullptr, &s.pass);
        if (r != VK_SUCCESS)
        {
            s.pass = VK_NULL_HANDLE;
            Write(Error, "vulkan: present: vkCreateRenderPass failed: %s", ResultName(r));
            return false;
        }
        s.format = format;
        return true;
    }

    /// The pipeline: no vertex input (the triangle comes from gl_VertexIndex), no culling, no
    /// depth, no blending, viewport and scissor dynamic so a resize keeps it.
    bool CreatePipeline()
    {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = s.vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = s.frag;
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
        raster.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blend = {};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blending = {};
        blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blending.attachmentCount = 1;
        blending.pAttachments    = &blend;

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
        info.pColorBlendState    = &blending;
        info.pDynamicState       = &dynamic;
        info.layout              = s.layout;
        info.renderPass          = s.pass;
        info.subpass             = 0;
        const VkResult r = s.dapi->CreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &info, nullptr, &s.pipeline);
        if (r != VK_SUCCESS)
        {
            s.pipeline = VK_NULL_HANDLE;
            Write(Error, "vulkan: present: vkCreateGraphicsPipelines failed: %s", ResultName(r));
            return false;
        }
        return true;
    }

    void DropPassObjects()
    {
        if (s.pipeline != VK_NULL_HANDLE) s.dapi->DestroyPipeline(s.device, s.pipeline, nullptr);
        if (s.pass != VK_NULL_HANDLE) s.dapi->DestroyRenderPass(s.device, s.pass, nullptr);
        s.pipeline = VK_NULL_HANDLE;
        s.pass     = VK_NULL_HANDLE;
        s.format   = VK_FORMAT_UNDEFINED;
    }

    /// The sets' scene binding: every slot samples the one scene image.
    void WriteSceneBinding()
    {
        VkDescriptorImageInfo image = {};
        image.sampler     = s.sampler;
        image.imageView   = s.scene;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[kSlots] = {};
        for (uint32_t i = 0; i < kSlots; ++i)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = s.slots[i].set;
            writes[i].dstBinding      = 0;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &image;
        }
        s.dapi->UpdateDescriptorSets(s.device, kSlots, writes, 0, nullptr);
    }

    bool Apply(VkFormat format, VkExtent2D extent, const VkImageView* views, uint32_t count, VkImageView scene)
    {
        if (s.pass != VK_NULL_HANDLE && s.format != format) DropPassObjects();
        if (s.pass == VK_NULL_HANDLE && !CreatePass(format)) return false;
        if (s.pipeline == VK_NULL_HANDLE && !CreatePipeline()) return false;

        gxvk::vk::present::Drop();
        for (uint32_t i = 0; i < count; ++i)
        {
            VkFramebufferCreateInfo info = {};
            info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass      = s.pass;
            info.attachmentCount = 1;
            info.pAttachments    = &views[i];
            info.width           = extent.width;
            info.height          = extent.height;
            info.layers          = 1;
            const VkResult r = s.dapi->CreateFramebuffer(s.device, &info, nullptr, &s.fbs[i]);
            if (r != VK_SUCCESS)
            {
                s.fbs[i] = VK_NULL_HANDLE;
                Write(Error, "vulkan: present: vkCreateFramebuffer failed: %s", ResultName(r));
                gxvk::vk::present::Drop();
                return false;
            }
            s.fbCount = i + 1;
        }
        s.extent = extent;
        s.scene  = scene;
        WriteSceneBinding();
        Write(Debug, "vulkan: present: pass over %s, %u framebuffer(s) of %ux%u", FormatName(format), count, extent.width, extent.height);
        return true;
    }
}

namespace gxvk::vk::present
{
    bool Init()
    {
        if (s.ready) return true;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi)
        {
            Write(Warn, "vulkan: present: Init() without a device; declining");
            return false;
        }
        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        if (!CreateModule(kPresentVert, sizeof kPresentVert, s.vert, "Present.vert")
            || !CreateModule(kPresentFrag, sizeof kPresentFrag, s.frag, "Present.frag"))
        {
            Shutdown();
            return false;
        }

        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo setLayout = {};
        setLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayout.bindingCount = 2;
        setLayout.pBindings    = bindings;
        VkResult r = d.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &s.setLayout);
        if (r == VK_SUCCESS)
        {
            VkPipelineLayoutCreateInfo layout = {};
            layout.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layout.setLayoutCount = 1;
            layout.pSetLayouts    = &s.setLayout;
            r = d.CreatePipelineLayout(s.device, &layout, nullptr, &s.layout);
        }
        if (r != VK_SUCCESS)
        {
            Write(Error, "vulkan: present: the layouts could not be made: %s", ResultName(r));
            Shutdown();
            return false;
        }

        // The scene and the swapchain image are the same size: a 1:1 copy, no filtering.
        VkSamplerCreateInfo sampler = {};
        sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter    = VK_FILTER_NEAREST;
        sampler.minFilter    = VK_FILTER_NEAREST;
        sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod       = 0.0f;
        r = d.CreateSampler(s.device, &sampler, nullptr, &s.sampler);
        if (r != VK_SUCCESS)
        {
            Write(Error, "vulkan: present: vkCreateSampler failed: %s", ResultName(r));
            Shutdown();
            return false;
        }

        VkDescriptorPoolSize sizes[2] = {};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = kSlots;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[1].descriptorCount = kSlots;
        VkDescriptorPoolCreateInfo pool = {};
        pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets       = kSlots;
        pool.poolSizeCount = 2;
        pool.pPoolSizes    = sizes;
        r = d.CreateDescriptorPool(s.device, &pool, nullptr, &s.pool);
        if (r != VK_SUCCESS)
        {
            Write(Error, "vulkan: present: vkCreateDescriptorPool failed: %s", ResultName(r));
            Shutdown();
            return false;
        }

        for (uint32_t i = 0; i < kSlots; ++i)
        {
            Slot& slot = s.slots[i];
            VkDescriptorSetAllocateInfo alloc = {};
            alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool     = s.pool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts        = &s.setLayout;
            r = d.AllocateDescriptorSets(s.device, &alloc, &slot.set);
            if (r != VK_SUCCESS)
            {
                slot.set = VK_NULL_HANDLE;
                Write(Error, "vulkan: present: vkAllocateDescriptorSets failed: %s", ResultName(r));
                Shutdown();
                return false;
            }
            if (!memory::CreateBuffer(kGammaBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, memory::Where::Stream, slot.gamma) || !slot.gamma.block.mapped)
            {
                Write(Error, "vulkan: present: no buffer for the gamma ramp");
                Shutdown();
                return false;
            }
            VkDescriptorBufferInfo buffer = {};
            buffer.buffer = slot.gamma.handle;
            buffer.offset = 0;
            buffer.range  = kGammaBytes;
            VkWriteDescriptorSet write = {};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = slot.set;
            write.dstBinding      = 1;
            write.descriptorCount = 1;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo     = &buffer;
            d.UpdateDescriptorSets(s.device, 1, &write, 0, nullptr);
        }

        // Identity until the device sets a ramp.
        if (s.gammaGen == 0)
        {
            for (uint32_t i = 0; i < kLutEntries; ++i)
            {
                const float v = static_cast<float>(i) / 255.0f;
                s.lut[i * 4 + 0] = v;
                s.lut[i * 4 + 1] = v;
                s.lut[i * 4 + 2] = v;
                s.lut[i * 4 + 3] = 0.0f;
            }
        }

        s.ready = true;
        Write(Debug, "vulkan: present: ready (%u-byte gamma block per frame slot)", kGammaBytes);
        if (s.pendingRebuild)
        {
            s.pendingRebuild = false;
            Apply(s.pendingFormat, s.pendingExtent, s.pendingViews, s.pendingCount, s.pendingScene);
        }
        return true;
    }

    void Shutdown()
    {
        if (!s.dapi || !s.device) return;
        const loader::Device& d = *s.dapi;
        Drop();
        DropPassObjects();
        for (Slot& slot : s.slots)
        {
            memory::DestroyBuffer(slot.gamma);
            slot = Slot();
        }
        if (s.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, s.pool, nullptr);
        if (s.sampler != VK_NULL_HANDLE) d.DestroySampler(s.device, s.sampler, nullptr);
        if (s.layout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.layout, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        if (s.vert != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.vert, nullptr);
        if (s.frag != VK_NULL_HANDLE) d.DestroyShaderModule(s.device, s.frag, nullptr);
        // The ramp outlives the device objects: the device sets it once, at bring-up.
        float          lut[kGammaBytes / sizeof(float)];
        std::memcpy(lut, s.lut, sizeof lut);
        const uint32_t gen     = s.gammaGen;
        const bool     applies = s.applies;
        s = State();
        std::memcpy(s.lut, lut, sizeof lut);
        s.gammaGen = gen;
        s.applies  = applies;
    }

    bool Rebuild(VkFormat format, VkExtent2D extent, const VkImageView* views, uint32_t count, VkImageView scene)
    {
        if (count > kMaxImages) count = kMaxImages;
        if (!s.ready)
        {
            s.pendingRebuild = true;
            s.pendingFormat  = format;
            s.pendingExtent  = extent;
            s.pendingCount   = count;
            s.pendingScene   = scene;
            for (uint32_t i = 0; i < count; ++i) s.pendingViews[i] = views[i];
            return false;
        }
        s.pendingRebuild = false;
        return Apply(format, extent, views, count, scene);
    }

    void Drop()
    {
        s.pendingRebuild = false;
        if (!s.dapi || !s.device) return;
        for (uint32_t i = 0; i < s.fbCount; ++i)
        {
            if (s.fbs[i] != VK_NULL_HANDLE) s.dapi->DestroyFramebuffer(s.device, s.fbs[i], nullptr);
            s.fbs[i] = VK_NULL_HANDLE;
        }
        s.fbCount = 0;
        s.extent  = VkExtent2D{ 0, 0 };
        s.scene   = VK_NULL_HANDLE;
    }

    bool Record(VkCommandBuffer cmd, uint32_t image, uint32_t slotIndex)
    {
        if (!s.ready || !cmd || image >= s.fbCount || slotIndex >= kSlots || s.pipeline == VK_NULL_HANDLE)
        {
            if (!s.loggedNoRecord)
            {
                s.loggedNoRecord = true;
                Write(Warn, "vulkan: present: nothing to record with (ready %d, %u framebuffer(s), image %u)", s.ready ? 1 : 0, s.fbCount, image);
            }
            return false;
        }
        const loader::Device& d    = *s.dapi;
        Slot&                 slot = s.slots[slotIndex];

        // The slot's gamma block, rewritten when the ramp changed since this slot last presented;
        // the slot's previous frame has completed (the frame waited its fence), so the write is safe.
        if (slot.gammaGen != s.gammaGen)
        {
            float* dst = static_cast<float*>(slot.gamma.block.mapped);
            std::memcpy(dst, s.lut, kLutEntries * 4 * sizeof(float));
            dst[kLutEntries * 4 + 0] = s.applies ? 1.0f : 0.0f;
            dst[kLutEntries * 4 + 1] = 0.0f;
            dst[kLutEntries * 4 + 2] = 0.0f;
            dst[kLutEntries * 4 + 3] = 0.0f;
            memory::FlushHost(slot.gamma.block);
            slot.gammaGen = s.gammaGen;
        }

        VkRenderPassBeginInfo begin = {};
        begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass        = s.pass;
        begin.framebuffer       = s.fbs[image];
        begin.renderArea.extent = s.extent;
        d.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
        d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
        d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &slot.set, 0, nullptr);
        VkViewport viewport = {};
        viewport.width    = static_cast<float>(s.extent.width);
        viewport.height   = static_cast<float>(s.extent.height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor = {};
        scissor.extent = s.extent;
        d.CmdSetViewport(cmd, 0, 1, &viewport);
        d.CmdSetScissor(cmd, 0, 1, &scissor);
        d.CmdDraw(cmd, 3, 1, 0, 0);
        d.CmdEndRenderPass(cmd);
        return true;
    }

    void SetGamma(const uint16_t* red, const uint16_t* green, const uint16_t* blue, bool apply)
    {
        frame::Drain();   // RENDERTHREAD: Record() reads the ramp on the render thread
        for (uint32_t i = 0; i < kLutEntries; ++i)
        {
            s.lut[i * 4 + 0] = red[i] / 65535.0f;
            s.lut[i * 4 + 1] = green[i] / 65535.0f;
            s.lut[i * 4 + 2] = blue[i] / 65535.0f;
            s.lut[i * 4 + 3] = 0.0f;
        }
        s.applies = apply;
        ++s.gammaGen;
    }

    bool Ready() { return s.ready && s.fbCount != 0; }
}
