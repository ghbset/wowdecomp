// The descriptor set layouts and the pipeline layouts: set 0 is one combined image sampler per
// declared sampler slot (one layout per distinct slot set), set 1 the translated interface's
// dynamic uniform buffer; every pipeline layout is {set 0, set 1} plus the push range.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines/Internal.hpp"

namespace gxvk::vk::pipelines::detail
{
    /// Set 1: binding 0, a dynamic uniform buffer read by both stages (the vertex module reads v[],
    /// the pixel module p[] and misc).
    bool CreateRegsLayout()
    {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layout = {};
        layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount = 1;
        layout.pBindings    = &binding;
        const VkResult r = s.dapi->CreateDescriptorSetLayout(s.device, &layout, nullptr, &s.regsLayout);
        if (r != VK_SUCCESS)
        {
            s.regsLayout = VK_NULL_HANDLE;
            Write(Error, "vulkan: pipelines: vkCreateDescriptorSetLayout (set 1) failed: %s", ResultName(r));
            return false;
        }
        return true;
    }

    /**
     * The set-0 layout and pipeline layout for a set of sampler slots, created on first use.
     * Binding N is slot N, a combined image sampler in the fragment stage. A mask of 0 (a pixel
     * module with no sampler) is a layout with no bindings, which keeps set 1 at index 1 for every
     * pipeline.
     *
     * @param uint32 samplerMask : bit N for slot N
     * @param uint32& index : receives the table index
     * @return bool ok : false when the table is full or a create failed (logged)
     */
    bool LayoutFor(uint32_t samplerMask, uint32_t& index)
    {
        for (uint32_t i = 0; i < s.layoutCount; ++i)
        {
            if (s.layouts[i].mask == samplerMask)
            {
                index = i;
                return true;
            }
        }
        if (s.layoutCount >= kMaxLayouts)
        {
            if (!s.warnedLayouts)
            {
                s.warnedLayouts = true;
                Write(Warn, "vulkan: pipelines: %u distinct sampler-slot sets; mask 0x%04X and later are refused (logged once)", kMaxLayouts, samplerMask);
            }
            return false;
        }

        const loader::Device& d = *s.dapi;
        VkDescriptorSetLayoutBinding bindings[kTextureStages] = {};
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < kTextureStages; ++slot)
        {
            if (!(samplerMask & (1u << slot))) continue;
            VkDescriptorSetLayoutBinding& b = bindings[count++];
            b.binding         = slot;
            b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setLayout = {};
        setLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayout.bindingCount = count;
        setLayout.pBindings    = count ? bindings : nullptr;

        LayoutEntry e;
        e.mask = samplerMask;
        VkResult r = d.CreateDescriptorSetLayout(s.device, &setLayout, nullptr, &e.set0);
        if (r != VK_SUCCESS)
        {
            Write(Error, "vulkan: pipelines: vkCreateDescriptorSetLayout (set 0) failed: %s", ResultName(r));
            return false;
        }

        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push.offset     = 0;
        push.size       = kPushBytes;
        const VkDescriptorSetLayout sets[2] = { e.set0, s.regsLayout };
        VkPipelineLayoutCreateInfo layout = {};
        layout.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount         = 2;
        layout.pSetLayouts            = sets;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges    = &push;
        r = d.CreatePipelineLayout(s.device, &layout, nullptr, &e.layout);
        if (r != VK_SUCCESS)
        {
            Write(Error, "vulkan: pipelines: vkCreatePipelineLayout failed: %s", ResultName(r));
            d.DestroyDescriptorSetLayout(s.device, e.set0, nullptr);
            return false;
        }

        index = s.layoutCount;
        s.layouts[s.layoutCount++] = e;
        s.stats.layouts = s.layoutCount;
        Write(Debug, "vulkan: pipelines: layout #%u for sampler slots 0x%04X (%u binding%s)", s.layoutCount, samplerMask, count, count == 1 ? "" : "s");
        return true;
    }

    void DestroyLayouts()
    {
        const loader::Device& d = *s.dapi;
        for (uint32_t i = 0; i < s.layoutCount; ++i)
        {
            if (s.layouts[i].layout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.layouts[i].layout, nullptr);
            if (s.layouts[i].set0 != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.layouts[i].set0, nullptr);
            s.layouts[i] = LayoutEntry();
        }
        s.layoutCount = 0;
        if (s.regsLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.regsLayout, nullptr);
        s.regsLayout = VK_NULL_HANDLE;
    }
}
