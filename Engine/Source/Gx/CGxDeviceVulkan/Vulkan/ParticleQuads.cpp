// GPUPARTICLES: the M2 particle quad expansion (see ParticleQuads.hpp).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/ParticleQuads.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Shaders/ParticleSpirv.inc"

#include <cstring>
#include <vector>

namespace
{
    namespace context = gxvk::vk::context;
    namespace frame   = gxvk::vk::frame;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    /// The output chunks of one frame slot: device-local vertex + storage buffers. A frame's
    /// particles are some hundred KiB to a few MiB; a draw bigger than a chunk gets a chunk its size.
    constexpr VkDeviceSize kChunkBytes = 4u * 1024u * 1024u;
    constexpr uint32_t     kMaxChunks  = 16;

    /// Descriptor sets one frame slot may write: one per distinct (stream chunk, output chunk).
    constexpr uint32_t kMaxSets = 256;

    constexpr uint32_t kGroup = 64;   ///< ParticleQuads.comp's local_size_x

    struct PushBlock
    {
        uint32_t inBase, outBase, count, stride, normalOff, colorOff, uvOff, pad;
        float    normal[4];
    };

    struct SetEntry
    {
        VkBuffer        in    = VK_NULL_HANDLE;
        uint32_t        chunk = 0;
        VkDescriptorSet set   = VK_NULL_HANDLE;
    };

    struct Slot
    {
        uint64_t         serial  = 0;
        VkDescriptorPool pool    = VK_NULL_HANDLE;
        memory::Buffer   chunks[kMaxChunks];
        uint32_t         chunkCount = 0;
        uint32_t         current    = 0;   ///< the chunk being filled this frame
        VkDeviceSize     cursor     = 0;
        SetEntry         sets[kMaxSets];
        uint32_t         setCount   = 0;
    };

    struct State
    {
        bool                  ready     = false;
        VkDevice              device    = nullptr;
        const loader::Device* dapi      = nullptr;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout      layout    = VK_NULL_HANDLE;
        VkPipeline            pipeline  = VK_NULL_HANDLE;
        Slot                  slots[frame::kFramesInFlight];
        bool                  warned    = false;
    };
    State s;

    void WarnOnce(const char* what)
    {
        if (s.warned) return;
        s.warned = true;
        Write(Error, "vulkan: particles: %s; those particle draws are dropped (logged once)", what);
    }

    /// The chunk `index` of a slot, made on first need with at least `bytes`.
    bool EnsureChunk(Slot& slot, uint32_t index, VkDeviceSize bytes)
    {
        if (index < slot.chunkCount && slot.chunks[index].size >= bytes) return true;
        if (index >= kMaxChunks) return false;
        if (index < slot.chunkCount)
        {
            // Too small for this draw: replaced. Its descriptor sets go with the pool reset, and
            // this frame's already recorded dispatches into it keep it alive through the retire.
            frame::RetireBuffer(slot.chunks[index], frame::Serial());
            for (uint32_t i = 0; i < slot.setCount; i++)
                if (slot.sets[i].chunk == index) slot.sets[i].in = VK_NULL_HANDLE;
        }
        const VkDeviceSize size = bytes > kChunkBytes ? bytes : kChunkBytes;
        memory::Buffer& b = slot.chunks[index];
        if (!memory::CreateBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, memory::Where::DeviceLocal, b))
        {
            b = memory::Buffer();
            return false;
        }
        if (index >= slot.chunkCount) slot.chunkCount = index + 1;
        return true;
    }

    VkDescriptorSet SetFor(Slot& slot, VkBuffer in, uint32_t chunk)
    {
        for (uint32_t i = 0; i < slot.setCount; i++)
            if (slot.sets[i].in == in && slot.sets[i].chunk == chunk) return slot.sets[i].set;
        if (slot.setCount == kMaxSets) return VK_NULL_HANDLE;

        VkDescriptorSetAllocateInfo alloc = {};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = slot.pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &s.setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (s.dapi->AllocateDescriptorSets(s.device, &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;

        VkDescriptorBufferInfo infos[2] = {};
        infos[0].buffer = in;
        infos[0].range  = VK_WHOLE_SIZE;
        infos[1].buffer = slot.chunks[chunk].handle;
        infos[1].range  = VK_WHOLE_SIZE;
        VkWriteDescriptorSet write = {};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 0;
        write.descriptorCount = 2;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = infos;
        s.dapi->UpdateDescriptorSets(s.device, 1, &write, 0, nullptr);

        slot.sets[slot.setCount++] = { in, chunk, set };
        return set;
    }
}

namespace gxvk::vk::particles
{
    bool Init()
    {
        if (s.ready) return true;
        const context::Handles* h = context::Get();
        if (!h || !h->dapi || !h->iapi) return false;

        uint32_t familyCount = 0;
        h->iapi->GetPhysicalDeviceQueueFamilyProperties(h->physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        if (familyCount) h->iapi->GetPhysicalDeviceQueueFamilyProperties(h->physical, &familyCount, families.data());
        if (h->graphicsFamily >= familyCount || !(families[h->graphicsFamily].queueFlags & VK_QUEUE_COMPUTE_BIT))
        {
            Write(Debug, "vulkan: particles: the graphics queue cannot run compute; particle quads stay on the CPU");
            return false;
        }

        s.device = h->device;
        s.dapi   = h->dapi;
        const loader::Device& d = *s.dapi;

        VkDescriptorSetLayoutBinding bindings[2] = {};
        for (uint32_t i = 0; i < 2; i++)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo = {};
        setInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = 2;
        setInfo.pBindings    = bindings;

        VkPushConstantRange range = {};
        range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        range.size       = sizeof(PushBlock);

        VkShaderModuleCreateInfo moduleInfo = {};
        moduleInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = sizeof kParticleQuadsComp;
        moduleInfo.pCode    = kParticleQuadsComp;
        VkShaderModule module = VK_NULL_HANDLE;

        VkResult r = d.CreateDescriptorSetLayout(s.device, &setInfo, nullptr, &s.setLayout);
        if (r == VK_SUCCESS)
        {
            VkPipelineLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layoutInfo.setLayoutCount         = 1;
            layoutInfo.pSetLayouts            = &s.setLayout;
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges    = &range;
            r = d.CreatePipelineLayout(s.device, &layoutInfo, nullptr, &s.layout);
        }
        if (r == VK_SUCCESS) r = d.CreateShaderModule(s.device, &moduleInfo, nullptr, &module);
        if (r == VK_SUCCESS)
        {
            VkComputePipelineCreateInfo info = {};
            info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = module;
            info.stage.pName  = "main";
            info.layout       = s.layout;
            r = d.CreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &info, nullptr, &s.pipeline);
            d.DestroyShaderModule(s.device, module, nullptr);
        }

        VkDescriptorPoolSize poolSize = {};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 2 * kMaxSets;
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = kMaxSets;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        for (Slot& slot : s.slots)
            if (r == VK_SUCCESS) r = d.CreateDescriptorPool(s.device, &poolInfo, nullptr, &slot.pool);

        if (r != VK_SUCCESS)
        {
            Write(Error, "vulkan: particles: the expansion pipeline could not be made: %s; particle quads stay on the CPU", ResultName(r));
            Shutdown();
            return false;
        }
        s.ready = true;
        Write(Debug, "vulkan: particles: GPU quad expansion ready");
        return true;
    }

    void Shutdown()
    {
        if (!s.dapi) return;
        const loader::Device& d = *s.dapi;
        for (Slot& slot : s.slots)
        {
            for (uint32_t i = 0; i < slot.chunkCount; i++) memory::DestroyBuffer(slot.chunks[i]);
            if (slot.pool != VK_NULL_HANDLE) d.DestroyDescriptorPool(s.device, slot.pool, nullptr);
        }
        if (s.pipeline != VK_NULL_HANDLE) d.DestroyPipeline(s.device, s.pipeline, nullptr);
        if (s.layout != VK_NULL_HANDLE) d.DestroyPipelineLayout(s.device, s.layout, nullptr);
        if (s.setLayout != VK_NULL_HANDLE) d.DestroyDescriptorSetLayout(s.device, s.setLayout, nullptr);
        s = State();
    }

    bool Ready()
    {
        return s.ready;
    }

    bool Expand(VkBuffer in, VkDeviceSize inOffset, uint32_t quads, const Layout& layout, const float normal[3], VkBuffer& out, VkDeviceSize& outOffset)
    {
        if (!s.ready || !quads || (inOffset & 3) || (layout.stride & 3) || (layout.colorOff & 3) || (layout.uvOff & 3)) return false;

        // The pre-pass opens the frame, so the slot and serial below are the open frame's.
        VkCommandBuffer cmd = frame::PrePass();
        if (!cmd) return false;

        Slot& slot = s.slots[frame::Slot()];
        const uint64_t serial = frame::Serial();
        if (slot.serial != serial)
        {
            // The slot's previous frame has completed (the frame waited on its fence to open).
            s.dapi->ResetDescriptorPool(s.device, slot.pool, 0);
            slot.serial   = serial;
            slot.setCount = 0;
            slot.current  = 0;
            slot.cursor   = 0;
        }

        const VkDeviceSize bytes = static_cast<VkDeviceSize>(quads) * 4u * layout.stride;
        if (slot.current < slot.chunkCount && slot.cursor + bytes > slot.chunks[slot.current].size)
        {
            slot.current++;
            slot.cursor = 0;
        }
        if (!EnsureChunk(slot, slot.current, slot.cursor + bytes))
        {
            WarnOnce("no device memory for the expanded vertices");
            return false;
        }

        VkDescriptorSet set = SetFor(slot, in, slot.current);
        if (set == VK_NULL_HANDLE)
        {
            WarnOnce("no descriptor set left this frame");
            return false;
        }

        PushBlock push = {};
        push.inBase    = static_cast<uint32_t>(inOffset / 4);
        push.outBase   = static_cast<uint32_t>(slot.cursor / 4);
        push.count     = quads;
        push.stride    = layout.stride / 4;
        push.normalOff = layout.normalOff == UINT32_MAX ? UINT32_MAX : layout.normalOff / 4;
        push.colorOff  = layout.colorOff / 4;
        push.uvOff     = layout.uvOff / 4;
        if (normal) std::memcpy(push.normal, normal, 3 * sizeof(float));

        const loader::Device& d = *s.dapi;
        d.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
        d.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &set, 0, nullptr);
        d.CmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        d.CmdDispatch(cmd, (quads + kGroup - 1) / kGroup, 1, 1);

        VkMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        d.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

        out       = slot.chunks[slot.current].handle;
        outOffset = slot.cursor;
        slot.cursor += bytes;
        return true;
    }
}
