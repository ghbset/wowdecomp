#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/FfShadersSpv.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Error.h"
#include "Engine/Source/Storm/H/storm.h"

// Phase V1 (see README.md): instance, device, swapchain, frames in flight, dynamic rendering, readable depth,
// clear/present/resize, buffer pools, texture create/upload, draw calls. Every draw goes through one stub pipeline
// (solid magenta, position only, FF world*view*proj); state that is already wired: blend, colour mask, depth, cull,
// depth bias, viewport, scissor.

// ---------------------------------------------------------------------------------------------------------------
// Loader: vulkan-1.dll at run time, everything through vkGetInstanceProcAddr / vkGetDeviceProcAddr (no import lib).

#define VK_GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties) X(vkEnumerateInstanceExtensionProperties)

#define VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)

#define VK_DEBUG_FUNCS(X) X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT)

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) \
    X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) \
    X(vkCmdCopyBufferToImage) X(vkCmdBlitImage) X(vkCmdBeginRendering) X(vkCmdEndRendering) \
    X(vkCmdClearAttachments) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdDraw) \
    X(vkCmdDrawIndexed) X(vkCmdSetPrimitiveTopology) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBias) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkCreateSampler) X(vkDestroySampler) X(vkResetDescriptorPool) \
    X(vkCmdClearColorImage) X(vkCmdSetBlendConstants)

#define VK_DECLARE(name) static PFN_##name name;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VK_GLOBAL_FUNCS(VK_DECLARE)
VK_INSTANCE_FUNCS(VK_DECLARE)
VK_DEBUG_FUNCS(VK_DECLARE)
VK_DEVICE_FUNCS(VK_DECLARE)
#undef VK_DECLARE

static const VkDeviceSize kBlockSize = 64 << 20;
static const VkDeviceSize kStagingSize = 4 << 20;
static const uint32_t kUboRingSize = 32 << 20;
static const uint32_t kConstantBytes = 256 * 16;
static const uint32_t kDeviceConstantBytes = 23 * 16;

static const uint32_t s_gxAttribSize[7] = { 4, 4, 4, 8, 12, 4, 4 };
static const VkFormat s_gxAttribFormat[7] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32_SFLOAT };

// Same memory layouts as the D3D9 formats (Blit copies texels unchanged). D24X8 is replaced by m_depthFormat.
static const VkFormat s_gxTexFmtToVk[GxTexFormats_Last] = {
    VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_A4R4G4B4_UNORM_PACK16,
    VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
    VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT
};

// As CGxDeviceD3d11: where a format is unsupported, textures move to this one (Blit converts on upload).
static const EGxTexFormat s_tolerableTexFmtMapping[GxTexFormats_Last] = { GxTex_Unknown, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Abgr8888, GxTex_Argb1555, GxTex_Rgb565, GxTex_Dxt1, GxTex_Dxt3, GxTex_Dxt5, GxTex_Uv88, GxTex_Gr1616F, GxTex_R32F, GxTex_D24X8 };

static const VkPrimitiveTopology s_primitiveConversion[GxPrims_Last] = {
    VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN
};

// D3D9 s_srcBlend / s_dstBlend per GxBlend mode; D3D9 blends alpha with the same factors, so alpha reuses them.
static const VkBlendFactor s_srcBlend[12] = { VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_CONSTANT_COLOR };
static const VkBlendFactor s_dstBlend[12] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR };
static const VkCompareOp s_cmpFunc[4] = { VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_LESS };
static const VkCullModeFlags s_cullMode[3] = { VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT };

static VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize align) {
    return (value + align - 1) / align * align;
}

// Every Vulkan log line goes to disk at once (SLogFlush = WriteFile + FlushFileBuffers): a hang that ends with
// Windows killing the process must still leave gx.log behind.
static void VkLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    CGxDevice::Log(format, args);
    va_end(args);

    if (CGxDevice::m_log) {
        SLogFlush(CGxDevice::m_log);
    }
}

// Finite waits only: 2 s, then the caller logs and gives up instead of hanging the window thread.
static const uint64_t kWaitTimeoutNs = 2000000000ull;

static uint64_t Fnv64(const void* data, size_t size, uint64_t hash = 0xCBF29CE484222325ull) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ bytes[i]) * 0x100000001B3ull;
    }
    return hash;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        VkLog("Vulkan: %s", data->pMessage);
    }
    return VK_FALSE;
}

CGxDevice* CreateCGxDeviceVulkan() {
    void* mem = SMemAlloc(sizeof(CGxDeviceVulkan), __FILE__, __LINE__, 0);

    if (!mem) {
        return nullptr;
    }

    return new (mem) CGxDeviceVulkan();
}

CGxDeviceVulkan::CGxDeviceVulkan() {
    this->m_api = GxApi_Vulkan;
    this->m_hwnd = nullptr;
    this->m_hwndClass = 0;
    this->m_ownhwnd = 0;
    this->m_vkLib = nullptr;
    this->m_instance = VK_NULL_HANDLE;
    this->m_messenger = VK_NULL_HANDLE;
    this->m_surface = VK_NULL_HANDLE;
    this->m_physDevice = VK_NULL_HANDLE;
    this->m_vkDevice = VK_NULL_HANDLE;
    this->m_queue = VK_NULL_HANDLE;
    this->m_queueFamily = 0;
    this->m_depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    this->m_swapchain = VK_NULL_HANDLE;
    this->m_swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    this->m_swapExtent = { 0, 0 };
    this->m_swapIndex = UINT32_MAX;
    this->m_swapDirty = false;
    memset(&this->m_depth, 0, sizeof(this->m_depth));
    memset(&this->m_rtDepth, 0, sizeof(this->m_rtDepth));
    for (Frame& frame : this->m_frames) {
        frame.m_cmdPool = VK_NULL_HANDLE;
        frame.m_fence = VK_NULL_HANDLE;
        frame.m_acquired = VK_NULL_HANDLE;
        frame.m_uboBuffer = VK_NULL_HANDLE;
        frame.m_uboAlloc = {};
        frame.m_uboHead = 0;
        frame.m_stagingHead = 0;
        frame.m_stagingCap = 0;
        frame.m_texPool = 0;
    }
    this->m_frameSerial = kFrames; // serials below kFrames count as completed
    this->m_recording = false;
    this->m_rendering = false;
    this->m_curColor = nullptr;
    this->m_curColorFace = 0;
    this->m_curDepth = nullptr;
    this->m_passColor = nullptr;
    this->m_passDepth = nullptr;
    this->m_passExtent = { 0, 0 };
    this->m_vkViewport = {};
    this->m_vpRect = {};
    this->m_vkScissor = {};
    this->m_scissorTest = false;
    this->m_depthBias = 0.0f;
    this->m_uboBindingCount = 3;
    memset(this->m_uboSets, 0, sizeof(this->m_uboSets));
    this->m_setLayouts[0] = this->m_setLayouts[1] = VK_NULL_HANDLE;
    this->m_pipelineLayout = VK_NULL_HANDLE;
    this->m_descPool = VK_NULL_HANDLE;
    this->m_dummyUbo = VK_NULL_HANDLE;
    this->m_dummyUboAlloc = {};
    memset(this->m_ffVs, 0, sizeof(this->m_ffVs));
    memset(this->m_ffPs, 0, sizeof(this->m_ffPs));
    this->m_curVs = this->m_curPs = nullptr;
    this->m_pipelineCache = VK_NULL_HANDLE;
    this->m_curPipeline = VK_NULL_HANDLE;
    memset(this->m_uboOffsets, 0xFF, sizeof(this->m_uboOffsets));
    this->m_cbSource[0] = this->m_cbSource[1] = -1;
    this->m_cbHigh[0] = this->m_cbHigh[1] = 0;
    memset(this->m_devConstants, 0, sizeof(this->m_devConstants));
    memset(this->m_ffConstants, 0, sizeof(this->m_ffConstants));
    memset(this->m_blendFactor, 0, sizeof(this->m_blendFactor));
    memset(&this->m_dummyImage, 0, sizeof(this->m_dummyImage));
    this->m_curTexSet = VK_NULL_HANDLE;
    memset(this->m_curViews, 0, sizeof(this->m_curViews));
    memset(this->m_curSamplers, 0, sizeof(this->m_curSamplers));
    this->m_blendMode = 0;
    this->m_depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    this->m_depthWrite = true;
    this->m_cullMode = VK_CULL_MODE_NONE;
    this->m_colorMask = 0xF;
    this->m_caps.m_colorFormat = GxCF_argb;
    this->m_caps.unk_0018 = 1;
    this->DeviceCreatePools();
    this->DeviceCreateStreamBufs();
}

CGxDeviceVulkan::~CGxDeviceVulkan() {
}

// ---------------------------------------------------------------------------------------------------------------
// Memory: first-fit suballocation from 64 MB blocks per (memory type, image or buffer), so buffers and optimal
// images never share a block (no bufferImageGranularity checks). Requests of half a block or more get their own.
// ponytail: no defragmentation and blocks are never returned while the device lives; add when memory reports show
// fragmentation.

bool CGxDeviceVulkan::IAlloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags, bool image, Alloc& out) {
    out = {};
    uint32_t type = UINT32_MAX;

    for (uint32_t i = 0; i < this->m_memProps.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) && (this->m_memProps.memoryTypes[i].propertyFlags & flags) == flags) {
            type = i;
            break;
        }
    }

    if (type == UINT32_MAX) {
        return false;
    }

    VkDeviceSize align = req.alignment > 16 ? req.alignment : 16;
    bool dedicated = req.size >= kBlockSize / 2;

    if (!dedicated) {
        for (MemBlock* block : this->m_blocks) {
            if (block->m_type != type || block->m_image != image || block->m_size != kBlockSize) {
                continue;
            }

            for (auto it = block->m_free.begin(); it != block->m_free.end(); ++it) {
                VkDeviceSize start = AlignUp(it->first, align);
                VkDeviceSize end = it->first + it->second;

                if (start + req.size > end) {
                    continue;
                }

                VkDeviceSize freeStart = it->first;
                block->m_free.erase(it);

                if (start > freeStart) {
                    block->m_free[freeStart] = start - freeStart;
                }

                if (start + req.size < end) {
                    block->m_free[start + req.size] = end - start - req.size;
                }

                out = { block, start, req.size, block->m_map ? block->m_map + start : nullptr };
                return true;
            }
        }
    }

    MemBlock* block = new MemBlock;
    block->m_size = dedicated ? req.size : kBlockSize;
    block->m_type = type;
    block->m_image = image;
    block->m_map = nullptr;

    VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    info.allocationSize = block->m_size;
    info.memoryTypeIndex = type;

    if (vkAllocateMemory(this->m_vkDevice, &info, nullptr, &block->m_memory) != VK_SUCCESS) {
        delete block;
        return false;
    }

    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* map = nullptr;
        vkMapMemory(this->m_vkDevice, block->m_memory, 0, VK_WHOLE_SIZE, 0, &map);
        block->m_map = static_cast<char*>(map);
    }

    if (block->m_size > req.size) {
        block->m_free[req.size] = block->m_size - req.size;
    }

    this->m_blocks.push_back(block);
    out = { block, 0, req.size, block->m_map };
    return true;
}

void CGxDeviceVulkan::IFree(Alloc& alloc) {
    MemBlock* block = alloc.m_block;
    VkDeviceSize offset = alloc.m_offset;
    VkDeviceSize size = alloc.m_size;
    alloc = {};

    if (!block) {
        return;
    }

    if (block->m_size != kBlockSize) {
        // Dedicated: the whole block goes.
        vkFreeMemory(this->m_vkDevice, block->m_memory, nullptr);

        for (size_t i = 0; i < this->m_blocks.size(); i++) {
            if (this->m_blocks[i] == block) {
                this->m_blocks.erase(this->m_blocks.begin() + i);
                break;
            }
        }

        delete block;
        return;
    }

    // Insert and merge with the neighbours.
    auto next = block->m_free.lower_bound(offset);

    if (next != block->m_free.end() && offset + size == next->first) {
        size += next->second;
        next = block->m_free.erase(next);
    }

    if (next != block->m_free.begin()) {
        auto prev = std::prev(next);

        if (prev->first + prev->second == offset) {
            prev->second += size;
            return;
        }
    }

    block->m_free[offset] = size;
}

bool CGxDeviceVulkan::ICreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, VkBuffer& buffer, Alloc& alloc) {
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(this->m_vkDevice, &info, nullptr, &buffer) != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(this->m_vkDevice, buffer, &req);
    VkMemoryPropertyFlags flags = hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    if (!this->IAlloc(req, flags, false, alloc) || vkBindBufferMemory(this->m_vkDevice, buffer, alloc.m_block->m_memory, alloc.m_offset) != VK_SUCCESS) {
        vkDestroyBuffer(this->m_vkDevice, buffer, nullptr);
        this->IFree(alloc);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

bool CGxDeviceVulkan::ICreateImage(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, VkFormat format, VkImageUsageFlags usage, bool cube, Image& out) {
    memset(&out, 0, sizeof(out));
    bool depth = format == this->m_depthFormat;

    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width ? width : 1, height ? height : 1, 1 };
    info.mipLevels = levels;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(this->m_vkDevice, &info, nullptr, &out.m_image) != VK_SUCCESS) {
        out.m_image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(this->m_vkDevice, out.m_image, &req);

    if (!this->IAlloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, out.m_alloc) || vkBindImageMemory(this->m_vkDevice, out.m_image, out.m_alloc.m_block->m_memory, out.m_alloc.m_offset) != VK_SUCCESS) {
        this->IDestroyImage(out, false);
        return false;
    }

    out.m_format = format;
    out.m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.m_aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    out.m_width = info.extent.width;
    out.m_height = info.extent.height;
    out.m_levels = levels;
    out.m_layers = layers;

    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = out.m_image;
    view.format = format;

    // Sampled view: depth aspect only for depth (the readable depth of the contract).
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        view.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange = { static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, levels, 0, layers };
        vkCreateImageView(this->m_vkDevice, &view, nullptr, &out.m_view);
    }

    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        for (uint32_t face = 0; face < layers && face < 6; face++) {
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.subresourceRange = { out.m_aspect, 0, 1, face, 1 };
            vkCreateImageView(this->m_vkDevice, &view, nullptr, &out.m_rtView[face]);
        }
    }

    return true;
}

// Deferred: the handles are destroyed once the frame being recorded has finished on the GPU.
void CGxDeviceVulkan::IDestroyImage(Image& image, bool deferred) {
    VkDevice device = this->m_vkDevice;
    Image copy = image;
    memset(&image, 0, sizeof(image));

    std::function<void()> destroy = [this, device, copy]() mutable {
        if (copy.m_view) {
            vkDestroyImageView(device, copy.m_view, nullptr);
        }

        for (VkImageView rtView : copy.m_rtView) {
            if (rtView) {
                vkDestroyImageView(device, rtView, nullptr);
            }
        }

        if (copy.m_image) {
            vkDestroyImage(device, copy.m_image, nullptr);
        }

        this->IFree(copy.m_alloc);
    };

    if (deferred) {
        this->IDefer(destroy);
    } else {
        destroy();
    }
}

void CGxDeviceVulkan::IDefer(std::function<void()> fn) {
    this->m_frames[this->m_frameSerial % kFrames].m_deletes.push_back(fn);
}

// ---------------------------------------------------------------------------------------------------------------
// Window: copied from CGxDeviceD3d11 (itself CGxDeviceD3d) apart from the class name.

static ATOM WindowClassCreateVulkan() {
    HINSTANCE instance = GetModuleHandleA(nullptr);

    WNDCLASSEXA windowClass;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_OWNDC;
    windowClass.lpfnWndProc = CGxDeviceVulkan::WindowProcVulkan;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = "GxWindowClassVulkan";
    windowClass.hIcon = static_cast<HICON>(LoadImageA(instance, "BlizzardIcon.ico", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    windowClass.hCursor = LoadCursorA(instance, "BlizzardCursor.cur");

    if (!windowClass.hCursor) {
        windowClass.hCursor = LoadCursorA(instance, IDC_ARROW);
    }

    return RegisterClassExA(&windowClass);
}

// Same as CGxDeviceD3d::ICreateWindow apart from the class name.
int32_t CGxDeviceVulkan::ICreateWindow(CGxFormat& format) {
    HINSTANCE instance = GetModuleHandleA(nullptr);
    DWORD style;

    if (!format.m_window) {
        style = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_SYSMENU;
    } else if (format.m_maximize == 1) {
        style = WS_POPUP | WS_VISIBLE;
    } else if (format.m_maximize == 2) {
        style = WS_POPUP;
    } else {
        style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    }

    float aspectRatio = 0.0f;

    if (format.m_aspect && format.m_size.x && format.m_size.y) {
        aspectRatio = static_cast<float>(format.m_size.x) / format.m_size.y;
    }

    RECT clientRect = { 0, 0, format.m_size.x, format.m_size.y };
    AdjustWindowRectEx(&clientRect, style, FALSE, 0);

    CGxDevice::s_clientAdjust.x = clientRect.right - format.m_size.x - clientRect.left;
    CGxDevice::s_clientAdjust.y = clientRect.bottom - format.m_size.y - clientRect.top;

    if (format.m_window) {
        if (format.m_maximize == 1) {
            format.m_pos.x = 0;
            format.m_pos.y = 0;
            format.m_size.x = GetSystemMetrics(SM_CXSCREEN);
            format.m_size.y = GetSystemMetrics(SM_CYSCREEN);
        } else {
            RECT windowRect = { 0, 0, format.m_size.x, format.m_size.y };

            if (CGxDevice::AdjustWindowRectToWorkArea(aspectRatio, &CGxDevice::s_clientAdjust, &windowRect)) {
                format.m_pos.x = windowRect.left;
                format.m_pos.y = windowRect.top;
                format.m_size.x = windowRect.right - windowRect.left;
                format.m_size.y = windowRect.bottom - windowRect.top;
            } else {
                format.m_pos.x = 0;
                format.m_pos.y = 0;
            }
        }
    }

    int32_t width = format.m_size.x ? format.m_size.x : CW_USEDEFAULT;
    int32_t height = format.m_size.y ? format.m_size.y : CW_USEDEFAULT;

    if (format.m_window && format.m_maximize != 1 && format.m_size.x && format.m_size.y) {
        width += CGxDevice::s_clientAdjust.x;
        height += CGxDevice::s_clientAdjust.y;
    }

    this->m_hwnd = CreateWindowExA(WS_EX_APPWINDOW, "GxWindowClassVulkan", "World of Warcraft", style, format.m_pos.x, format.m_pos.y, width, height, nullptr, nullptr, instance, this);

    if (this->m_hwnd && format.m_maximize != 2) {
        ShowWindow(this->m_hwnd, SW_SHOWNORMAL);
    }

    return this->m_hwnd != nullptr;
}

// Same as CGxDeviceD3d::WindowProcD3d, except WM_SETCURSOR: there is no hardware cursor in Vulkan.
LRESULT CALLBACK CGxDeviceVulkan::WindowProcVulkan(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    CGxDeviceVulkan* device = reinterpret_cast<CGxDeviceVulkan*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE:
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTA*>(lparam)->lpCreateParams));
            return 0;

        case WM_DESTROY:
            device->DeviceWM(GxWM_Destroy, 0, 0);
            return 0;

        case WM_SIZE: {
            CRect rect;
            rect.minY = 0.0f;
            rect.minX = 0.0f;
            rect.maxY = HIWORD(lparam);
            rect.maxX = LOWORD(lparam);

            uint32_t resizeType = 0;

            if (wparam == SIZE_MINIMIZED) {
                resizeType = 1;
            } else if (wparam == SIZE_MAXHIDE) {
                resizeType = 2;
            }

            device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), resizeType);
            break;
        }

        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) {
                if (!device->IDevIsWindowed()) {
                    CRect rect;
                    rect.minY = 0.0f;
                    rect.minX = 0.0f;
                    rect.maxY = 0.0f;
                    rect.maxX = 0.0f;
                    device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 1);
                }
            } else if (LOWORD(wparam) == WA_ACTIVE && !device->IDevIsWindowed()) {
                CRect rect;
                rect.minY = 0.0f;
                rect.minX = 0.0f;
                rect.maxY = 0.0f;
                rect.maxX = 0.0f;
                GxCapsWindowSizeInScreenCoords(rect);
                device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 3);
            }

            break;

        case WM_SETFOCUS:
            device->DeviceWM(GxWM_SetFocus, 0, 0);
            return 0;

        case WM_KILLFOCUS:
            device->DeviceWM(GxWM_KillFocus, 0, 0);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_ERASEBKGND:
            return 0;

        case WM_SETCURSOR:
            if (!device) {
                return DefWindowProcA(hwnd, message, wparam, lparam);
            }

            if (device->m_vkDevice && LOWORD(lparam) == HTCLIENT) {
                SetCursor(nullptr);
                return 1;
            }

            break;

        case WM_DISPLAYCHANGE:
            if (!device->IDevIsWindowed()) {
                CRect rect;
                rect.minY = 0.0f;
                rect.minX = 0.0f;
                rect.maxY = HIWORD(lparam);
                rect.maxX = LOWORD(lparam);
                device->DeviceWM(GxWM_DisplayChange, reinterpret_cast<uintptr_t>(&rect), 0);
            }

            break;

        case WM_SYSCOMMAND:
            if (wparam == SC_SCREENSAVE || wparam == SC_MONITORPOWER) {
                return 0;
            }

            break;

        case WM_SIZING: {
            RECT* sizingRect = reinterpret_cast<RECT*>(lparam);

            CiRect rect;
            rect.minX = sizingRect->left;
            rect.minY = sizingRect->top;
            rect.maxX = sizingRect->right;
            rect.maxY = sizingRect->bottom;

            if (rect.maxX - rect.minX <= 0 || rect.maxY - rect.minY <= 0) {
                return 0;
            }

            switch (wparam) {
                case WMSZ_LEFT:
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    break;

                case WMSZ_RIGHT:
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    break;

                case WMSZ_TOP:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    break;

                case WMSZ_TOPLEFT:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    break;

                case WMSZ_TOPRIGHT:
                    CGxDevice::ClampRectTop(rect);
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectTopByAspect(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                case WMSZ_BOTTOM:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                case WMSZ_BOTTOMLEFT:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectLeft(rect);
                    CGxDevice::ClampRectLeftByAspect(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    break;

                case WMSZ_BOTTOMRIGHT:
                    CGxDevice::ClampRectBottom(rect);
                    CGxDevice::ClampRectRight(rect);
                    CGxDevice::ClampRectBottomByAspect(rect);
                    CGxDevice::ClampRectRightByAspect(rect);
                    break;

                default:
                    break;
            }

            sizingRect->left = rect.minX;
            sizingRect->bottom = rect.maxY;
            sizingRect->top = rect.minY;
            sizingRect->right = rect.maxX;

            return 1;
        }

        default:
            break;
    }

    if (device && device->m_windowProc) {
        return device->m_windowProc(hwnd, message, wparam, lparam);
    }

    return DefWindowProcA(hwnd, message, wparam, lparam);
}

// No exclusive fullscreen in Vulkan (the swapchain presents to a popup window): keep the cursor clipped and minimise
// a fullscreen window on focus loss, as the D3D11 device does.
void CGxDeviceVulkan::ISetWindowFocus(int32_t focus) {
    int32_t changed = this->m_windowFocus != focus;
    this->m_windowFocus = focus;

    if (focus && !this->m_format.m_window) {
        RECT rect;
        GetWindowRect(this->m_hwnd, &rect);
        ClipCursor(&rect);
    }

    if (changed && !focus && !this->m_format.m_window && this->m_hwnd) {
        ShowWindow(this->m_hwnd, SW_MINIMIZE);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Device lifetime

int32_t CGxDeviceVulkan::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;

    HDC dc = GetDC(nullptr);

    if (GetDeviceGammaRamp(dc, &this->m_systemGammaRamp) && &this->m_gammaRamp != &this->m_systemGammaRamp) {
        this->m_gammaRamp = this->m_systemGammaRamp;
    }

    ReleaseDC(nullptr, dc);

    this->m_hwndClass = WindowClassCreateVulkan();

    if (!this->m_hwndClass) {
        char* error = OsGetLastErrorStr();
        VkLog("CGxDeviceVulkan::DeviceCreate(): WindowClassCreate() failed: %s", error);
        OsFreeLastErrorStr(error);
        this->DeviceDestroy();
        return 0;
    }

    if (this->ICreateVkInstance() && CGxDevice::DeviceCreate(windowProc, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

int32_t CGxDeviceVulkan::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_ownhwnd = 0;
    CGxDevice::DeviceCreate(window, format);
    this->m_hwnd = static_cast<HWND>(window);

    if (this->ICreateVkInstance() && this->ICreateVkDevice(format) && CGxDevice::DeviceCreate(window, format)) {
        return 1;
    }

    this->DeviceDestroy();
    return 0;
}

int32_t CGxDeviceVulkan::DeviceSetFormat(const CGxFormat& format) {
    VkLog("CGxDeviceVulkan::DeviceSetFormat():");
    this->Log(format);

    if (this->m_hwnd) {
        ShowWindow(this->m_hwnd, SW_HIDE);
    }

    this->IDestroyVk();

    if (this->m_hwnd) {
        DestroyWindow(this->m_hwnd);
    }

    this->m_hwnd = nullptr;
    this->m_format = format;

    CGxFormat createFormat = format;

    if (this->ICreateWindow(createFormat) && this->ICreateVkDevice(createFormat) && CGxDevice::DeviceSetFormat(createFormat)) {
        this->ISetWindowFocus(1);
        return 1;
    }

    VkLog("CGxDeviceVulkan::DeviceSetFormat(): unable to set format!");
    this->IDestroyVk();

    if (this->m_hwnd) {
        DestroyWindow(this->m_hwnd);
    }

    this->m_hwnd = nullptr;

    return 0;
}

void CGxDeviceVulkan::DeviceDestroy() {
    CGxDevice::DeviceDestroy();

    if (this->m_hwnd && this->m_ownhwnd) {
        DestroyWindow(this->m_hwnd);
        this->m_hwnd = nullptr;
    }

    if (this->m_hwndClass) {
        UnregisterClassA(reinterpret_cast<const char*>(static_cast<uintptr_t>(this->m_hwndClass)), GetModuleHandleA(nullptr));
        this->m_hwndClass = 0;
    }

    this->IDestroyVk();
    this->IDestroyVkInstance();
}

// vulkan-1.dll, the instance, the validation layer when present (debug messenger → gx.log). The instance outlives
// format changes; the surface and device do not (the window is recreated).
int32_t CGxDeviceVulkan::ICreateVkInstance() {
    if (this->m_instance) {
        return 1;
    }

    VkLog("CGxDeviceVulkan: LoadLibrary vulkan-1.dll ...");
    this->m_vkLib = LoadLibraryA("vulkan-1.dll");
    VkLog("CGxDeviceVulkan: LoadLibrary vulkan-1.dll -> %p", this->m_vkLib);

    if (!this->m_vkLib) {
        VkLog("CGxDeviceVulkan: vulkan-1.dll not found");
        return 0;
    }

    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(this->m_vkLib, "vkGetInstanceProcAddr")));

    if (!vkGetInstanceProcAddr) {
        return 0;
    }

#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    VK_GLOBAL_FUNCS(VK_LOAD)
#undef VK_LOAD

    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool validation = false;

    for (const VkLayerProperties& layer : layers) {
        validation |= !strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation");
    }

    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());
    bool debugUtils = false;

    for (const VkExtensionProperties& ext : exts) {
        debugUtils |= !strcmp(ext.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    const char* extNames[3] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    const char* layerName = "VK_LAYER_KHRONOS_validation";

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "World of Warcraft";
    app.applicationVersion = 12340;
    app.pEngineName = "Gx";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = debugUtils ? 3 : 2;
    info.ppEnabledExtensionNames = extNames;
    info.enabledLayerCount = validation ? 1 : 0;
    info.ppEnabledLayerNames = &layerName;

    VkLog("CGxDeviceVulkan: vkCreateInstance ... (API 1.3, %u layers available, %u extensions available)", layerCount, extCount);
    VkLog("CGxDeviceVulkan:   layers: %s", validation ? layerName : "(none)");

    for (uint32_t i = 0; i < info.enabledExtensionCount; i++) {
        VkLog("CGxDeviceVulkan:   extension: %s", extNames[i]);
    }

    VkResult result = vkCreateInstance(&info, nullptr, &this->m_instance);
    VkLog("CGxDeviceVulkan: vkCreateInstance -> %d", result);

    if (result != VK_SUCCESS) {
        VkLog("CGxDeviceVulkan: vkCreateInstance failed: %d", result);
        this->m_instance = VK_NULL_HANDLE;
        return 0;
    }

#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(this->m_instance, #name));
    VK_INSTANCE_FUNCS(VK_LOAD)
    VK_DEBUG_FUNCS(VK_LOAD)
#undef VK_LOAD

    VkLog("CGxDeviceVulkan: instance created (validation %d, debug utils %d)", validation, debugUtils);

    if (debugUtils && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT messenger = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = DebugCallback;
        vkCreateDebugUtilsMessengerEXT(this->m_instance, &messenger, nullptr, &this->m_messenger);
    }

    return 1;
}

void CGxDeviceVulkan::IDestroyVkInstance() {
    if (this->m_messenger) {
        vkDestroyDebugUtilsMessengerEXT(this->m_instance, this->m_messenger, nullptr);
        this->m_messenger = VK_NULL_HANDLE;
    }

    if (this->m_instance) {
        vkDestroyInstance(this->m_instance, nullptr);
        this->m_instance = VK_NULL_HANDLE;
    }

    if (this->m_vkLib) {
        FreeLibrary(this->m_vkLib);
        this->m_vkLib = nullptr;
    }
}

// Surface, physical device (discrete first, Vulkan 1.3, one graphics+present family), device, frames, descriptor
// layouts per the SPIR-V contract (shader-xlate/SPIRV.md), stub pipeline modules, swapchain.
int32_t CGxDeviceVulkan::ICreateVkDevice(const CGxFormat& format) {
    VkWin32SurfaceCreateInfoKHR surfaceInfo = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    surfaceInfo.hinstance = GetModuleHandleA(nullptr);
    surfaceInfo.hwnd = this->m_hwnd;

    VkLog("CGxDeviceVulkan: vkCreateWin32SurfaceKHR ... (hwnd %p, visible %d)", this->m_hwnd, this->m_hwnd ? IsWindowVisible(this->m_hwnd) : 0);
    VkResult surfaceResult = vkCreateWin32SurfaceKHR(this->m_instance, &surfaceInfo, nullptr, &this->m_surface);
    VkLog("CGxDeviceVulkan: vkCreateWin32SurfaceKHR -> %d", surfaceResult);

    if (surfaceResult != VK_SUCCESS) {
        VkLog("CGxDeviceVulkan: vkCreateWin32SurfaceKHR failed");
        this->m_surface = VK_NULL_HANDLE;
        return 0;
    }

    uint32_t count = 0;
    VkLog("CGxDeviceVulkan: enumerating physical devices ...");
    vkEnumeratePhysicalDevices(this->m_instance, &count, nullptr);
    VkLog("CGxDeviceVulkan: %u physical devices", count);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(this->m_instance, &count, devices.data());
    int32_t bestScore = -1;

    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        if (props.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

        for (uint32_t i = 0; i < familyCount; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, this->m_surface, &present);

            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) {
                continue;
            }

            int32_t score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0);

            if (score > bestScore) {
                bestScore = score;
                this->m_physDevice = device;
                this->m_queueFamily = i;
            }

            break;
        }
    }

    if (bestScore < 0) {
        VkLog("CGxDeviceVulkan: no Vulkan 1.3 device with a graphics+present queue");
        return 0;
    }

    vkGetPhysicalDeviceProperties(this->m_physDevice, &this->m_physProps);
    vkGetPhysicalDeviceMemoryProperties(this->m_physDevice, &this->m_memProps);
    VkLog("CGxDeviceVulkan: %s (vendor 0x%04X device 0x%04X, API %u.%u.%u)", this->m_physProps.deviceName, this->m_physProps.vendorID, this->m_physProps.deviceID, VK_API_VERSION_MAJOR(this->m_physProps.apiVersion), VK_API_VERSION_MINOR(this->m_physProps.apiVersion), VK_API_VERSION_PATCH(this->m_physProps.apiVersion));

    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(this->m_physDevice, &supported);

    VkPhysicalDeviceVulkan13Features features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    features13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features.pNext = &features13;
    features.features.textureCompressionBC = supported.textureCompressionBC;
    features.features.samplerAnisotropy = supported.samplerAnisotropy;
    // SPIRV.md: o12/o13 become a 6-element ClipDistance array.
    features.features.shaderClipDistance = supported.shaderClipDistance;
    features.features.fillModeNonSolid = supported.fillModeNonSolid;
    features.features.geometryShader = supported.geometryShader; // FF point-sprite GS

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = this->m_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* deviceExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.pNext = &features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = &deviceExt;

    VkLog("CGxDeviceVulkan: vkCreateDevice ... (queue family %u, %u extensions)", this->m_queueFamily, deviceInfo.enabledExtensionCount);
    VkResult result = vkCreateDevice(this->m_physDevice, &deviceInfo, nullptr, &this->m_vkDevice);
    VkLog("CGxDeviceVulkan: vkCreateDevice -> %d", result);

    if (result != VK_SUCCESS) {
        VkLog("CGxDeviceVulkan: vkCreateDevice failed: %d", result);
        this->m_vkDevice = VK_NULL_HANDLE;
        return 0;
    }

#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(this->m_vkDevice, #name));
    VK_DEVICE_FUNCS(VK_LOAD)
#undef VK_LOAD

    vkGetDeviceQueue(this->m_vkDevice, this->m_queueFamily, 0, &this->m_queue);

    // Readable depth: D24S8 when it can be both attachment and sampled, else D32S8.
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(this->m_physDevice, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    VkFormatFeatureFlags depthNeeds = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    this->m_depthFormat = (fp.optimalTilingFeatures & depthNeeds) == depthNeeds ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;

    // Frames: command pool with an upload and a main command buffer, fence (signalled), acquire semaphore, UBO ring.
    for (Frame& frame : this->m_frames) {
        VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = this->m_queueFamily;
        vkCreateCommandPool(this->m_vkDevice, &poolInfo, nullptr, &frame.m_cmdPool);

        VkCommandBuffer cmds[2];
        VkCommandBufferAllocateInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cmdInfo.commandPool = frame.m_cmdPool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 2;
        vkAllocateCommandBuffers(this->m_vkDevice, &cmdInfo, cmds);
        frame.m_uploadCmd = cmds[0];
        frame.m_cmd = cmds[1];

        VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(this->m_vkDevice, &fenceInfo, nullptr, &frame.m_fence);

        VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(this->m_vkDevice, &semInfo, nullptr, &frame.m_acquired);

        if (!this->ICreateBuffer(kUboRingSize + kConstantBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, frame.m_uboBuffer, frame.m_uboAlloc)) {
            return 0;
        }
    }

    // Zero buffer: GS cb0 and the zero-stride vertex binding for VS inputs the decl lacks.
    if (!this->ICreateBuffer(kConstantBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, this->m_dummyUbo, this->m_dummyUboAlloc)) {
        return 0;
    }

    if (!this->ICreateImage(1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, this->m_dummyImage)) {
        return 0;
    }

    memset(this->m_dummyUboAlloc.m_map, 0, kConstantBytes);

    // Set 0: VS b0, PS b0, device cb1, FF point-sprite GS cb0 (dynamic UBOs). Set 1: t0..t15 at 0..15, s0..s15 at
    // 16..31, shared by all stages.
    VkDescriptorSetLayoutBinding ubos[4];

    for (uint32_t i = 0; i < 4; i++) {
        ubos[i] = { i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, static_cast<VkShaderStageFlags>(i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : (i == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : (i == 2 ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_GEOMETRY_BIT))), nullptr };
    }

    VkDescriptorSetLayoutBinding textures[32];

    for (uint32_t i = 0; i < 32; i++) {
        textures[i] = { i, i < 16 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    }

    VkDescriptorSetLayoutCreateInfo setInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setInfo.bindingCount = supported.geometryShader ? 4 : 3;
    setInfo.pBindings = ubos;
    vkCreateDescriptorSetLayout(this->m_vkDevice, &setInfo, nullptr, &this->m_setLayouts[0]);
    setInfo.bindingCount = 32;
    setInfo.pBindings = textures;
    vkCreateDescriptorSetLayout(this->m_vkDevice, &setInfo, nullptr, &this->m_setLayouts[1]);

    VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = this->m_setLayouts;
    vkCreatePipelineLayout(this->m_vkDevice, &layoutInfo, nullptr, &this->m_pipelineLayout);

    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 * kFrames };
    VkDescriptorPoolCreateInfo descPoolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    descPoolInfo.maxSets = kFrames;
    descPoolInfo.poolSizeCount = 1;
    descPoolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(this->m_vkDevice, &descPoolInfo, nullptr, &this->m_descPool);

    // One set 0 per frame: VS b0, PS b0 and cb1 point into the frame's UBO ring (dynamic offsets per draw), the GS
    // cb0 at the zero buffer (no point-sprite GS yet, as D3D11).
    for (uint32_t f = 0; f < kFrames; f++) {
        VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = this->m_descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &this->m_setLayouts[0];
        vkAllocateDescriptorSets(this->m_vkDevice, &allocInfo, &this->m_uboSets[f]);

        VkDescriptorBufferInfo bufInfo[4] = {
            { this->m_frames[f].m_uboBuffer, 0, kConstantBytes },
            { this->m_frames[f].m_uboBuffer, 0, kConstantBytes },
            { this->m_frames[f].m_uboBuffer, 0, kDeviceConstantBytes },
            { this->m_dummyUbo, 0, kConstantBytes },
        };
        VkWriteDescriptorSet writes[4];

        for (uint32_t i = 0; i < 4; i++) {
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet = this->m_uboSets[f];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[i].pBufferInfo = &bufInfo[i];
        }

        vkUpdateDescriptorSets(this->m_vkDevice, supported.geometryShader ? 4 : 3, writes, 0, nullptr);
    }

    // ponytail: the pipeline cache lives in memory only; save it to disk if first-use hitches show up.
    VkPipelineCacheCreateInfo cacheInfo = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    vkCreatePipelineCache(this->m_vkDevice, &cacheInfo, nullptr, &this->m_pipelineCache);

    for (uint32_t i = 0; i < 48; i++) {
        this->IShaderApiCreate(g_ffVsSpv[i].data, static_cast<uint32_t>(g_ffVsSpv[i].size), this->m_ffVs[i]);
    }

    for (uint32_t i = 0; i < 3; i++) {
        this->IShaderApiCreate(g_ffPsSpv[i].data, static_cast<uint32_t>(g_ffPsSpv[i].size), this->m_ffPs[i]);
    }

    VkLog("CGxDeviceVulkan: fixed-function shader modules created");
    this->m_format = format;

    if (!this->ICreateSwapchain()) {
        return 0;
    }

    VkLog("CGxDeviceVulkan: device ready");

    this->m_context = 1;
    this->ISetCaps(format);
    this->Log(this->m_caps);
    this->DeviceSetGamma(this->m_gammaRamp);
    this->ICursorCreate(format);
    g_theGxDevicePtr->ShaderCreate(CGxDevice::s_uiVertexShader, GxSh_Vertex, "Shaders\\Vertex", "UI", 2);
    g_theGxDevicePtr->ShaderCreate(&CGxDevice::s_uiPixelShader, GxSh_Pixel, "Shaders\\Pixel", "UI", 1);
    return 1;
}

void CGxDeviceVulkan::IDestroyVk() {
    for (uint32_t i = 0; i < 2; i++) {
        if (CGxDevice::s_uiVertexShader[i]) {
            g_theGxDevicePtr->ShaderDestroy(CGxDevice::s_uiVertexShader[i]);
        }
    }

    if (CGxDevice::s_uiPixelShader) {
        g_theGxDevicePtr->ShaderDestroy(CGxDevice::s_uiPixelShader);
    }

    this->ICursorDestroy();

    // App shaders are recreated from their kept code (m_loaded = 0) on the next device.
    for (uint32_t target : { GxSh_Vertex, GxSh_Pixel }) {
        TSHashTable<CGxShader, HASHKEY_STRI>& list = this->m_shaderList[target];

        for (CGxShader* shader = list.m_fulllist.Head(); shader; shader = list.m_fulllist.Next(shader)) {
            if (shader->m_apiSpecific) {
                this->IShaderApiRelease(*static_cast<ShaderApi*>(shader->m_apiSpecific));
                delete static_cast<ShaderApi*>(shader->m_apiSpecific);
                shader->m_apiSpecific = nullptr;
            }

            shader->m_loaded = 0;
            shader->m_valid = 0;
        }
    }

    if (this->m_vkDevice) {
        this->IWaitIdle("destroy"); // before closing the open frame: its reset fence was never submitted

        if (this->m_recording) {
            // Close the open frame without submitting it.
            this->m_rendering = false;
            vkEndCommandBuffer(this->m_frames[this->m_frameSerial % kFrames].m_uploadCmd);
            vkEndCommandBuffer(this->m_frames[this->m_frameSerial % kFrames].m_cmd);
            this->m_recording = false;
        }
    }

    this->ITexForceRecreation(1);
    this->IReleasePools(1);
    this->IReleaseRtDepth();

    for (Frame& frame : this->m_frames) {
        for (std::function<void()>& fn : frame.m_deletes) {
            fn();
        }

        frame.m_deletes.clear();
    }

    if (this->m_vkDevice) {
        this->IReleaseSwapchain();

        for (auto& entry : this->m_pipelines) {
            vkDestroyPipeline(this->m_vkDevice, entry.second, nullptr);
        }

        this->m_pipelines.clear();

        for (auto& entry : this->m_samplers) {
            vkDestroySampler(this->m_vkDevice, entry.second, nullptr);
        }

        this->m_samplers.clear();

        for (ShaderApi& api : this->m_ffVs) {
            this->IShaderApiRelease(api);
        }

        for (ShaderApi& api : this->m_ffPs) {
            this->IShaderApiRelease(api);
        }

        vkDestroyPipelineCache(this->m_vkDevice, this->m_pipelineCache, nullptr);
        this->m_pipelineCache = VK_NULL_HANDLE;
        this->IDestroyImage(this->m_dummyImage, false);
        memset(&this->m_dummyImage, 0, sizeof(this->m_dummyImage));
        vkDestroyDescriptorPool(this->m_vkDevice, this->m_descPool, nullptr);
        vkDestroyPipelineLayout(this->m_vkDevice, this->m_pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(this->m_vkDevice, this->m_setLayouts[0], nullptr);
        vkDestroyDescriptorSetLayout(this->m_vkDevice, this->m_setLayouts[1], nullptr);
        vkDestroyBuffer(this->m_vkDevice, this->m_dummyUbo, nullptr);

        for (Frame& frame : this->m_frames) {
            for (size_t i = 0; i < frame.m_stagingBuffers.size(); i++) {
                vkDestroyBuffer(this->m_vkDevice, frame.m_stagingBuffers[i], nullptr);
            }

            frame.m_stagingBuffers.clear();
            frame.m_stagingAllocs.clear();
            vkDestroyBuffer(this->m_vkDevice, frame.m_uboBuffer, nullptr);

            for (VkDescriptorPool pool : frame.m_texPools) {
                vkDestroyDescriptorPool(this->m_vkDevice, pool, nullptr);
            }

            vkDestroySemaphore(this->m_vkDevice, frame.m_acquired, nullptr);
            vkDestroyFence(this->m_vkDevice, frame.m_fence, nullptr);
            vkDestroyCommandPool(this->m_vkDevice, frame.m_cmdPool, nullptr);
            frame = Frame();
            frame.m_stagingHead = frame.m_stagingCap = 0;
            frame.m_uboHead = 0;
            frame.m_texPool = 0;
        }

        for (MemBlock* block : this->m_blocks) {
            vkFreeMemory(this->m_vkDevice, block->m_memory, nullptr);
            delete block;
        }

        this->m_blocks.clear();
        vkDestroyDevice(this->m_vkDevice, nullptr);
        this->m_vkDevice = VK_NULL_HANDLE;
    }

    if (this->m_surface) {
        vkDestroySurfaceKHR(this->m_instance, this->m_surface, nullptr);
        this->m_surface = VK_NULL_HANDLE;
    }

    this->m_dummyUbo = VK_NULL_HANDLE;
    this->m_dummyUboAlloc = {};
    this->m_curPipeline = VK_NULL_HANDLE;
    this->m_curColor = nullptr;
    this->m_curDepth = nullptr;
    this->m_context = 0;
}

// Swapchain: FIFO with vsync, else MAILBOX (IMMEDIATE, then FIFO, as fallbacks). A zero extent (minimised) leaves no
// swapchain; frames are skipped until the next resize.
int32_t CGxDeviceVulkan::ICreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(this->m_physDevice, this->m_surface, &caps);
    VkExtent2D extent = caps.currentExtent;

    if (extent.width == UINT32_MAX) {
        const CRect& window = this->DeviceCurWindow();
        extent.width = static_cast<uint32_t>(window.maxX);
        extent.height = static_cast<uint32_t>(window.maxY);
    }

    this->m_swapDirty = false;

    if (!extent.width || !extent.height) {
        this->IReleaseSwapchain();
        return 1;
    }

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(this->m_physDevice, this->m_surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(this->m_physDevice, this->m_surface, &count, formats.data());

    if (!count) {
        return 0;
    }

    VkSurfaceFormatKHR surfaceFormat = formats[0];

    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = candidate;
        }
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(this->m_physDevice, this->m_surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(this->m_physDevice, this->m_surface, &count, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

    if (!this->m_format.m_vsync) {
        for (VkPresentModeKHR mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR || (mode == VK_PRESENT_MODE_IMMEDIATE_KHR && presentMode == VK_PRESENT_MODE_FIFO_KHR)) {
                presentMode = mode;
            }
        }
    }

    uint32_t imageCount = caps.minImageCount + 1;

    if (caps.maxImageCount && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = this->m_surface;
    info.minImageCount = imageCount;
    info.imageFormat = surfaceFormat.format;
    info.imageColorSpace = surfaceFormat.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = this->m_swapchain;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkLog("CGxDeviceVulkan: vkCreateSwapchainKHR ... (%ux%u, %u images, present mode %d, format %d)", info.imageExtent.width, info.imageExtent.height, info.minImageCount, info.presentMode, info.imageFormat);
    VkResult result = vkCreateSwapchainKHR(this->m_vkDevice, &info, nullptr, &swapchain);
    VkLog("CGxDeviceVulkan: vkCreateSwapchainKHR -> %d", result);
    this->IReleaseSwapchain();

    if (result != VK_SUCCESS) {
        VkLog("CGxDeviceVulkan: vkCreateSwapchainKHR failed: %d", result);
        return 0;
    }

    this->m_swapchain = swapchain;
    this->m_swapFormat = surfaceFormat.format;
    this->m_swapExtent = extent;

    vkGetSwapchainImagesKHR(this->m_vkDevice, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(this->m_vkDevice, swapchain, &count, images.data());
    this->m_swapImages.resize(count);
    this->m_swapReady.resize(count);

    for (uint32_t i = 0; i < count; i++) {
        Image& image = this->m_swapImages[i];
        memset(&image, 0, sizeof(image));
        image.m_image = images[i];
        image.m_format = surfaceFormat.format;
        image.m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        image.m_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        image.m_width = extent.width;
        image.m_height = extent.height;
        image.m_levels = 1;
        image.m_layers = 1;

        VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = surfaceFormat.format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(this->m_vkDevice, &view, nullptr, &image.m_rtView[0]);

        VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(this->m_vkDevice, &semInfo, nullptr, &this->m_swapReady[i]);
    }

    // Readable depth of the back buffer: attachment + sampled (+ transfer for DeviceReadDepths later).
    return this->ICreateImage(extent.width, extent.height, 1, 1, this->m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false, this->m_depth);
}

// Waits for the GPU, then drops the swapchain images, their semaphores and the back-buffer depth.
void CGxDeviceVulkan::IReleaseSwapchain() {
    if (!this->m_vkDevice) {
        return;
    }

    this->IWaitIdle("swapchain release");

    for (size_t i = 0; i < this->m_swapImages.size(); i++) {
        vkDestroyImageView(this->m_vkDevice, this->m_swapImages[i].m_rtView[0], nullptr);
        vkDestroySemaphore(this->m_vkDevice, this->m_swapReady[i], nullptr);
    }

    this->m_swapImages.clear();
    this->m_swapReady.clear();
    this->m_swapIndex = UINT32_MAX;

    if (this->m_depth.m_image) {
        this->IDestroyImage(this->m_depth, false);
    }

    if (this->m_swapchain) {
        vkDestroySwapchainKHR(this->m_vkDevice, this->m_swapchain, nullptr);
        this->m_swapchain = VK_NULL_HANDLE;
    }
}

void CGxDeviceVulkan::ISetCaps(const CGxFormat& format) {
    const VkPhysicalDeviceLimits& limits = this->m_physProps.limits;
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(this->m_physDevice, &features);
    uint32_t maxTextureSize = limits.maxImageDimension2D < 16384 ? limits.maxImageDimension2D : 16384;

    this->m_caps.m_numTmus = 8;
    this->m_caps.m_pixelCenterOnEdge = 1;
    this->m_caps.m_texelCenterOnEdge = 1;
    this->m_caps.m_texMaxSize[GxTex_2d] = maxTextureSize;
    this->m_caps.m_texMaxSize[GxTex_CubeMap] = maxTextureSize;
    this->m_caps.m_texMaxSize[GxTex_Rectangle] = maxTextureSize;
    this->m_caps.m_texMaxSize[GxTex_NonPow2] = maxTextureSize;
    this->m_caps.m_maxIndex = 0x00FFFFFF;
    this->m_caps.m_texFilterTrilinear = 1;
    this->m_caps.m_texFilterAnisotropic = features.samplerAnisotropy;
    this->m_caps.m_maxTexAnisotropy = features.samplerAnisotropy ? static_cast<uint32_t>(limits.maxSamplerAnisotropy) : 1;
    this->m_caps.m_depthBias = 1;
    this->m_caps.m_numStreams = 16;
    // Stream offsets are always supported: buffers bind at their byte offset and draws use base vertex 0.
    this->m_caps.unk_0010 = 1;

    for (uint32_t i = 0; i < GxShTargets_Last; i++) {
        this->m_caps.m_shaderTargets[i] = 0;
        this->m_caps.m_shaderConstants[i] = 0;
    }

    // SM3 profiles, as on D3D11, so every client feature check matches D3D9; only the load path will differ (V2).
    this->m_caps.m_shaderTargets[GxSh_Vertex] = 3;
    this->m_caps.m_shaderTargets[GxSh_Pixel] = 4;
    this->m_caps.m_shaderConstants[GxSh_Vertex] = 256;

    if (format.m_fixedFunction[GxSh_Pixel] != -1 && format.m_fixedFunction[GxSh_Pixel] <= this->m_caps.m_shaderTargets[GxSh_Pixel]) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = format.m_fixedFunction[GxSh_Pixel];
    }

    if (format.m_fixedFunction[GxSh_Vertex] != -1 && format.m_fixedFunction[GxSh_Vertex] <= this->m_caps.m_shaderTargets[GxSh_Vertex]) {
        this->m_caps.m_shaderTargets[GxSh_Vertex] = format.m_fixedFunction[GxSh_Vertex];
    }

    for (uint32_t i = 1; i < GxTexFormats_Last; i++) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(this->m_physDevice, i == GxTex_D24X8 ? this->m_depthFormat : s_gxTexFmtToVk[i], &props);
        this->m_caps.m_texFmt[i] = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    }

    this->m_caps.m_texFmt[0] = 0;
    this->m_caps.m_generateMipMaps = 1;

    static const EGxTexFormat s_rttFormats[4] = { GxTex_Argb8888, GxTex_Rgb565, GxTex_Gr1616F, GxTex_R32F };

    for (uint32_t i = 0; i < 4; i++) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(this->m_physDevice, s_gxTexFmtToVk[s_rttFormats[i]], &props);
        this->m_caps.m_rttFormat[s_rttFormats[i]] = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    }

    this->m_caps.m_rttFormat[GxTex_D24X8] = 1;
    this->m_caps.m_colorWrite = 1;
    this->m_caps.m_hwCursor = 0;
    this->m_caps.m_maxClipPlanes = features.shaderClipDistance ? 6 : 0;
    this->m_caps.m_texTarget[GxTex_2d] = 1;
    this->m_caps.m_texTarget[GxTex_CubeMap] = 1;
    this->m_caps.m_texTarget[GxTex_Rectangle] = 0;
    this->m_caps.m_texTarget[GxTex_NonPow2] = 1;
    this->m_caps.m_texNonPow2 = 0;
    // TODO(V3): occlusion queries (VkQueryPool).
    this->m_caps.m_occlusionQuery = 0;
    this->m_caps.m_pointParameters = 0;
    this->m_caps.m_pointScaleMax = 1.0f;
    this->m_caps.m_pointSprite = 0;
    this->m_caps.m_blendFactor = 1;
    this->m_caps.unk_0130 = 1;

    for (uint32_t i = 0; i < 5; i++) {
        this->m_caps.unk_0114[i] = 0;
    }

    this->m_caps.unk_0134 = this->m_caps.m_shaderTargets[GxSh_Pixel] != 4;
    this->m_caps.unk_0138 = this->m_caps.m_shaderTargets[GxSh_Pixel] != 4;
    this->m_uboBindingCount = features.geometryShader ? 4 : 3;
}

// ---------------------------------------------------------------------------------------------------------------
// Frames: kFrames in flight. Each has an upload command buffer (staging copies, mip generation) submitted ahead of
// the main one in the same vkQueueSubmit, so uploads made mid-frame land before every draw of that frame.

// Fence waits time out after 2 s. A timeout means the GPU never finished (or the fence was never submitted): it is
// logged and the device is marked lost (m_context = 0) so the window stays alive and gx.log says why.
bool CGxDeviceVulkan::IWaitFence(VkFence fence, const char* what) {
    VkResult result = vkWaitForFences(this->m_vkDevice, 1, &fence, VK_TRUE, kWaitTimeoutNs);

    if (result == VK_SUCCESS) {
        return true;
    }

    VkLog("CGxDeviceVulkan: FREEZE AVOIDED: fence wait (%s) returned %d after 2 s at frame %llu; device marked lost", what, result, static_cast<unsigned long long>(this->m_frameSerial));
    this->m_context = 0;
    return false;
}

// Completed-frame test. IBeginFrame resets the slot fence of the frame being recorded, so a serial that shares that
// slot (serial + kFrames == m_frameSerial while recording) was already waited for there; waiting on the fence again
// would wait on an unsubmitted fence forever (the frame-2 hang: a stream pool wrap in frame N waiting for N - 2).
bool CGxDeviceVulkan::IWaitSerial(uint64_t serial) {
    if (serial >= this->m_frameSerial) {
        return true; // the frame being recorded: nothing to wait for yet
    }

    if (serial + kFrames < this->m_frameSerial || (serial + kFrames == this->m_frameSerial && this->m_recording)) {
        return true; // its slot fence was waited for by a later IBeginFrame
    }

    // Here the slot fence belongs to exactly this (submitted) serial.
    return this->IWaitFence(this->m_frames[serial % kFrames].m_fence, "IWaitSerial");
}

// vkDeviceWaitIdle has no timeout. Every queue submission carries a frame fence, so waiting for the submitted frames'
// fences is device idle; only then is vkDeviceWaitIdle (presents) called.
void CGxDeviceVulkan::IWaitIdle(const char* what) {
    if (!this->m_vkDevice) {
        return;
    }

    VkLog("CGxDeviceVulkan: wait idle (%s)", what);
    bool ok = true;

    for (uint64_t serial = this->m_frameSerial >= kFrames ? this->m_frameSerial - kFrames : 0; serial < this->m_frameSerial && ok; serial++) {
        if (serial + kFrames == this->m_frameSerial && this->m_recording) {
            continue; // the slot being recorded: reset, not submitted
        }

        ok = this->IWaitFence(this->m_frames[serial % kFrames].m_fence, what);
    }

    if (ok) {
        vkDeviceWaitIdle(this->m_vkDevice);
    }

    VkLog("CGxDeviceVulkan: wait idle (%s) done", what);
}

void CGxDeviceVulkan::IBeginFrame() {
    if (this->m_recording || !this->m_vkDevice) {
        return;
    }

    if (this->m_swapDirty) {
        this->ICreateSwapchain();
    }

    Frame& frame = this->m_frames[this->m_frameSerial % kFrames];

    if (this->m_frameSerial < kFrames + 3) {
        VkLog("CGxDeviceVulkan: frame %llu begin (slot %llu)", static_cast<unsigned long long>(this->m_frameSerial), static_cast<unsigned long long>(this->m_frameSerial % kFrames));
    }

    // A timed-out fence is never reset (resetting a fence a pending submit still uses is invalid); the device is lost.
    if (this->IWaitFence(frame.m_fence, "frame begin")) {
        vkResetFences(this->m_vkDevice, 1, &frame.m_fence);
    }

    // Recording from here on: IWaitIdle (swapchain recreation below) must skip this slot's reset, unsubmitted fence.
    this->m_recording = true;

    for (std::function<void()>& fn : frame.m_deletes) {
        fn();
    }

    frame.m_deletes.clear();

    // ponytail: staging buffers are freed every frame; keep the first one around if upload-heavy frames show up.
    for (size_t i = 0; i < frame.m_stagingBuffers.size(); i++) {
        vkDestroyBuffer(this->m_vkDevice, frame.m_stagingBuffers[i], nullptr);
        this->IFree(frame.m_stagingAllocs[i]);
    }

    frame.m_stagingBuffers.clear();
    frame.m_stagingAllocs.clear();
    frame.m_stagingHead = 0;
    frame.m_stagingCap = 0;
    frame.m_uboHead = 0;
    memset(this->m_uboOffsets, 0xFF, sizeof(this->m_uboOffsets)); // re-upload b0/cb1 into this frame's ring

    for (VkDescriptorPool pool : frame.m_texPools) {
        vkResetDescriptorPool(this->m_vkDevice, pool, 0);
    }

    frame.m_texPool = 0;
    this->m_curTexSet = VK_NULL_HANDLE;

    vkResetCommandPool(this->m_vkDevice, frame.m_cmdPool, 0);
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.m_uploadCmd, &begin);
    vkBeginCommandBuffer(frame.m_cmd, &begin);
    this->m_curPipeline = VK_NULL_HANDLE;

    if (this->m_dummyImage.m_image && this->m_dummyImage.m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        VkClearColorValue zero = {};
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        this->ITransition(frame.m_uploadCmd, this->m_dummyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(frame.m_uploadCmd, this->m_dummyImage.m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        this->ITransition(frame.m_uploadCmd, this->m_dummyImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    this->m_swapIndex = UINT32_MAX;

    for (uint32_t attempt = 0; attempt < 2 && this->m_swapchain; attempt++) {
        uint32_t index = 0;
        if (this->m_frameSerial < kFrames + 3) {
            VkLog("CGxDeviceVulkan: frame %llu acquire ...", static_cast<unsigned long long>(this->m_frameSerial));
        }

        VkResult result = vkAcquireNextImageKHR(this->m_vkDevice, this->m_swapchain, kWaitTimeoutNs, frame.m_acquired, VK_NULL_HANDLE, &index);

        if (this->m_frameSerial < kFrames + 3 || result == VK_TIMEOUT || result == VK_NOT_READY) {
            VkLog("CGxDeviceVulkan: frame %llu acquire -> %d (image %u)%s", static_cast<unsigned long long>(this->m_frameSerial), result, index, result == VK_TIMEOUT ? ": FREEZE AVOIDED, no image after 2 s, frame not presented" : "");
        }

        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            this->m_swapIndex = index;
            this->m_swapImages[index].m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            this->m_swapDirty = result == VK_SUBOPTIMAL_KHR;
            break;
        }

        if (result != VK_ERROR_OUT_OF_DATE_KHR) {
            VkLog("CGxDeviceVulkan: vkAcquireNextImageKHR failed: %d", result);
            break;
        }

        this->ICreateSwapchain();
    }
}

// ponytail: one full barrier (all commands → all commands) per transition; narrow the stages per layout if
// barriers show up in GPU captures.
void CGxDeviceVulkan::ITransition(VkCommandBuffer cmd, Image& image, VkImageLayout layout) {
    if (image.m_layout == layout || !image.m_image) {
        return;
    }

    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = image.m_layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.m_image;
    barrier.subresourceRange = { image.m_aspect, 0, image.m_levels, 0, image.m_layers };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    image.m_layout = layout;
}

static void LevelBarrier(VkCommandBuffer cmd, VkImage image, uint32_t level, uint32_t layers, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, layers };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Host-visible scratch for this frame's upload command buffer. Null when it cannot be allocated.
char* CGxDeviceVulkan::IStaging(uint32_t size, VkBuffer& buffer, VkDeviceSize& offset) {
    this->IBeginFrame();
    Frame& frame = this->m_frames[this->m_frameSerial % kFrames];
    VkDeviceSize need = AlignUp(size, 16);

    if (frame.m_stagingBuffers.empty() || frame.m_stagingHead + need > frame.m_stagingCap) {
        VkDeviceSize cap = need > kStagingSize ? need : kStagingSize;
        VkBuffer newBuffer;
        Alloc alloc;

        if (!this->ICreateBuffer(cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, newBuffer, alloc)) {
            return nullptr;
        }

        frame.m_stagingBuffers.push_back(newBuffer);
        frame.m_stagingAllocs.push_back(alloc);
        frame.m_stagingHead = 0;
        frame.m_stagingCap = cap;
    }

    buffer = frame.m_stagingBuffers.back();
    offset = frame.m_stagingHead;
    frame.m_stagingHead += need;
    return frame.m_stagingAllocs.back().m_map + offset;
}

// ---------------------------------------------------------------------------------------------------------------
// Render targets and passes. m_curColor / m_curDepth null mean the back buffer / its depth; the swapchain image is
// only known once the frame has acquired it, so passes resolve it when they begin.

void CGxDeviceVulkan::IBeginRendering() {
    if (this->m_rendering) {
        return;
    }

    this->IBeginFrame();

    if (!this->m_recording) {
        return;
    }

    Image* color = this->m_curColor ? this->m_curColor : (this->m_swapIndex != UINT32_MAX ? &this->m_swapImages[this->m_swapIndex] : nullptr);
    Image* depth = this->m_curDepth ? this->m_curDepth : (this->m_depth.m_image ? &this->m_depth : nullptr);

    if (!color && !depth) {
        return;
    }

    VkCommandBuffer cmd = this->m_frames[this->m_frameSerial % kFrames].m_cmd;
    VkExtent2D extent = { UINT32_MAX, UINT32_MAX };
    VkRenderingAttachmentInfo colorInfo = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo depthInfo = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };

    if (color) {
        this->ITransition(cmd, *color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colorInfo.imageView = color->m_rtView[this->m_curColor ? this->m_curColorFace : 0];
        colorInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = { color->m_width, color->m_height };
    }

    if (depth) {
        this->ITransition(cmd, *depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        depthInfo.imageView = depth->m_rtView[0];
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent.width = depth->m_width < extent.width ? depth->m_width : extent.width;
        extent.height = depth->m_height < extent.height ? depth->m_height : extent.height;
    }

    VkRenderingInfo info = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    info.renderArea = { { 0, 0 }, extent };
    info.layerCount = 1;
    info.colorAttachmentCount = color ? 1 : 0;
    info.pColorAttachments = color ? &colorInfo : nullptr;
    info.pDepthAttachment = depth ? &depthInfo : nullptr;
    info.pStencilAttachment = depth ? &depthInfo : nullptr;
    vkCmdBeginRendering(cmd, &info);

    this->m_passColor = color;
    this->m_passDepth = depth;
    this->m_passExtent = extent;
    this->m_rendering = true;
}

void CGxDeviceVulkan::IEndRendering() {
    if (!this->m_rendering) {
        return;
    }

    VkCommandBuffer cmd = this->m_frames[this->m_frameSerial % kFrames].m_cmd;
    vkCmdEndRendering(cmd);
    this->m_rendering = false;

    // Texture targets go back to sampled. The back buffer stays until present; depth stays an attachment.
    if (this->m_passColor && this->m_passColor == this->m_curColor) {
        this->ITransition(cmd, *this->m_passColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    this->m_passColor = nullptr;
    this->m_passDepth = nullptr;
}

void CGxDeviceVulkan::IReleaseRtDepth() {
    if (this->m_rtDepth.m_image) {
        this->IDestroyImage(this->m_rtDepth, true);
    }
}

void CGxDeviceVulkan::IBindRenderTargets() {
    this->IEndRendering();

    Image* color = static_cast<Image*>(this->m_textureTarget[GxBuffers_Color].m_apiSpecific);
    Image* depth = static_cast<Image*>(this->m_textureTarget[GxBuffers_Depth].m_apiSpecific);

    // ponytail: one scratch depth for colour-only texture targets, recreated on every size change (as D3D11).
    if (color && !depth) {
        if (this->m_rtDepth.m_image && (this->m_rtDepth.m_width != color->m_width || this->m_rtDepth.m_height != color->m_height)) {
            this->IReleaseRtDepth();
        }

        if (!this->m_rtDepth.m_image) {
            this->ICreateImage(color->m_width, color->m_height, 1, 1, this->m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false, this->m_rtDepth);
        }

        depth = this->m_rtDepth.m_image ? &this->m_rtDepth : nullptr;
    }

    uint32_t plane = this->m_textureTarget[GxBuffers_Color].m_plane;
    this->m_curColor = color;
    this->m_curColorFace = plane < 6 ? plane : 0;
    this->m_curDepth = depth;
}

void CGxDeviceVulkan::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (!this->m_context) {
        return;
    }

    CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];

    if (target.m_texture == texture && target.m_plane == plane) {
        return;
    }

    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);
    target.m_apiSpecific = nullptr;

    if (texture) {
        if (texture->m_needsCreation || !texture->m_apiSpecificData) {
            this->ITexCreate(texture);
        }

        if (!texture->m_needsCreation && texture->m_apiSpecificData) {
            target.m_apiSpecific = texture->m_apiSpecificData;
        }
    }

    this->IBindRenderTargets();
    GxXformSetViewport(this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);
    this->m_viewportDirty = 1;
    this->m_scissorRectDirty = 1;
}

int32_t CGxDeviceVulkan::DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size:
            if (param2 == 1 || param2 == 2) {
                this->m_windowVisible = 0;
                break;
            }

            this->m_windowVisible = 1;
            this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));

            // The swapchain is rebuilt at the next frame start; render-target textures are recreated now (their
            // owners rebuild them on the callback), as on D3D11.
            if (this->m_vkDevice && this->m_context) {
                this->ITexForceRecreation(0);
                this->m_swapDirty = true;
                this->ISetWindowFocus(1);
                this->m_intF5C = 0;
                this->NotifyOnDeviceRestored();
            }

            this->m_viewportDirty = 1;
            break;

        case GxWM_DisplayChange:
            if (this->m_windowVisible) {
                this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_Destroy:
            this->m_windowFocus = 0;
            break;

        case GxWM_KillFocus:
            this->ISetWindowFocus(0);
            break;

        case GxWM_SetFocus:
            this->ISetWindowFocus(1);
            break;

        default:
            break;
    }

    return 0;
}

void CGxDeviceVulkan::ScenePresent() {
    if (this->m_context) {
        int32_t scrShotClick = this->m_scrShotClick;
        CGxDevice::ScenePresent();
        this->ICursorDraw();

        if (scrShotClick) {
            this->DeviceScreenShot();
        }

        this->WaitForFPSCap();

        if (this->m_recording) {
            this->IEndRendering();
            Frame& frame = this->m_frames[this->m_frameSerial % kFrames];
            bool image = this->m_swapIndex != UINT32_MAX;

            if (image) {
                this->ITransition(frame.m_cmd, this->m_swapImages[this->m_swapIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            }

            // Uploads (buffer and image copies) are visible to every stage of the main command buffer.
            VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            vkCmdPipelineBarrier(frame.m_uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            vkEndCommandBuffer(frame.m_uploadCmd);
            vkEndCommandBuffer(frame.m_cmd);

            VkCommandBuffer cmds[2] = { frame.m_uploadCmd, frame.m_cmd };
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.waitSemaphoreCount = image ? 1 : 0;
            submit.pWaitSemaphores = &frame.m_acquired;
            submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 2;
            submit.pCommandBuffers = cmds;
            submit.signalSemaphoreCount = image ? 1 : 0;
            submit.pSignalSemaphores = image ? &this->m_swapReady[this->m_swapIndex] : nullptr;
            bool early = this->m_frameSerial < kFrames + 3;

            if (early) {
                VkLog("CGxDeviceVulkan: frame %llu submit ... (image %d)", static_cast<unsigned long long>(this->m_frameSerial), image ? static_cast<int32_t>(this->m_swapIndex) : -1);
            }

            VkResult result = vkQueueSubmit(this->m_queue, 1, &submit, frame.m_fence);

            if (early) {
                VkLog("CGxDeviceVulkan: frame %llu submit -> %d", static_cast<unsigned long long>(this->m_frameSerial), result);
            }

            if (result != VK_SUCCESS) {
                // TODO(V3): device loss → rebuild, as D3D11's IRecreateD3dDevice.
                VkLog("CGxDeviceVulkan::ScenePresent(): vkQueueSubmit failed: %d", result);
                this->m_context = 0;
            } else if (image) {
                VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                present.waitSemaphoreCount = 1;
                present.pWaitSemaphores = &this->m_swapReady[this->m_swapIndex];
                present.swapchainCount = 1;
                present.pSwapchains = &this->m_swapchain;
                present.pImageIndices = &this->m_swapIndex;
                result = vkQueuePresentKHR(this->m_queue, &present);

                if (early) {
                    VkLog("CGxDeviceVulkan: frame %llu present -> %d", static_cast<unsigned long long>(this->m_frameSerial), result);
                }

                if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
                    this->m_swapDirty = true;
                } else if (result != VK_SUCCESS) {
                    VkLog("CGxDeviceVulkan::ScenePresent(): vkQueuePresentKHR failed: %d", result);
                }
            }

            this->m_frameSerial++;
            this->m_recording = false;
            this->m_swapIndex = UINT32_MAX;
        }
    }

    this->ShaderConstantsClear();
}

// Clears are clipped to the viewport, as D3D9 Clear is: vkCmdClearAttachments takes the viewport rect directly, so no
// clear quad is needed (D3D11 needed one).
void CGxDeviceVulkan::SceneClear(uint32_t mask, CImVector color) {
    if (!this->m_context) {
        return;
    }

    this->IBeginRendering();

    if (!this->m_rendering) {
        return;
    }

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }

    VkClearAttachment attachments[2];
    uint32_t count = 0;

    if ((mask & 0x1) && this->m_passColor) {
        attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachments[count].colorAttachment = 0;
        attachments[count].clearValue.color = { { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f } };
        count++;
    }

    if ((mask & 0x2) && this->m_passDepth) {
        attachments[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        attachments[count].colorAttachment = 0;
        attachments[count].clearValue.depthStencil = { 1.0f, 0 };
        count++;
    }

    int32_t x0 = this->m_vpRect.offset.x < 0 ? 0 : this->m_vpRect.offset.x;
    int32_t y0 = this->m_vpRect.offset.y < 0 ? 0 : this->m_vpRect.offset.y;
    int32_t x1 = this->m_vpRect.offset.x + static_cast<int32_t>(this->m_vpRect.extent.width);
    int32_t y1 = this->m_vpRect.offset.y + static_cast<int32_t>(this->m_vpRect.extent.height);
    x1 = x1 > static_cast<int32_t>(this->m_passExtent.width) ? static_cast<int32_t>(this->m_passExtent.width) : x1;
    y1 = y1 > static_cast<int32_t>(this->m_passExtent.height) ? static_cast<int32_t>(this->m_passExtent.height) : y1;

    if (!count || x1 <= x0 || y1 <= y0) {
        return;
    }

    VkClearRect rect = { { { x0, y0 }, { static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0) } }, 0, 1 };
    vkCmdClearAttachments(this->m_frames[this->m_frameSerial % kFrames].m_cmd, count, attachments, 1, &rect);
}

// Same pixel rect as CGxDeviceVulkan::IXformSetViewport (D3D9 rules, RT Y rule included), then a negative height so
// D3D clip space (y up) lands the right way up without a shader flip.
void CGxDeviceVulkan::IXformSetViewport() {
    const CRect& window = this->DeviceCurWindow();
    float x = floorf(this->m_viewport.m_x.m_l * window.maxX + 0.5f);
    float y = floorf((1.0f - this->m_viewport.m_y.m_h) * window.maxY + 0.5f);
    float width = floorf(this->m_viewport.m_x.m_h * window.maxX - x + 0.5f);
    float height = floorf((1.0f - this->m_viewport.m_y.m_l) * window.maxY - y + 0.5f);

    if (this->m_textureTarget[0].m_apiSpecific || this->m_textureTarget[1].m_apiSpecific) {
        y = floorf(window.maxY * this->m_viewport.m_y.m_l);
    }

    this->m_vkViewport = { x, y + height, width, -height, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h };
    this->m_vpRect = { { static_cast<int32_t>(x), static_cast<int32_t>(y) }, { static_cast<uint32_t>(width > 0.0f ? width : 0.0f), static_cast<uint32_t>(height > 0.0f ? height : 0.0f) } };
    this->m_viewportDirty = 0;
}

// Same rect as CGxDeviceVulkan::IStateSyncScissorRect.
void CGxDeviceVulkan::IStateSyncScissorRect() {
    if (!this->m_scissorRectDirty) {
        return;
    }

    const CRect& window = this->DeviceCurWindow();
    int32_t left = static_cast<int32_t>(this->m_scissorRect.minX * window.maxX + 0.5f);
    int32_t right = static_cast<int32_t>(this->m_scissorRect.maxX * window.maxX + 1.0f);
    int32_t top;
    int32_t bottom;

    if (this->m_textureTarget[0].m_apiSpecific || this->m_textureTarget[1].m_apiSpecific) {
        top = static_cast<int32_t>(this->m_scissorRect.minY * window.maxY + 0.5f);
        bottom = static_cast<int32_t>(1.0f + this->m_scissorRect.maxY * window.maxY);
    } else {
        top = static_cast<int32_t>((1.0f - this->m_scissorRect.maxY) * window.maxY + 0.5f);
        bottom = static_cast<int32_t>(1.0f + (1.0f - this->m_scissorRect.minY) * window.maxY);
    }

    left = left < 0 ? 0 : left;
    top = top < 0 ? 0 : top;
    right = right > window.maxX ? static_cast<int32_t>(window.maxX) : right;
    bottom = bottom > window.maxY ? static_cast<int32_t>(window.maxY) : bottom;
    this->m_vkScissor = { { left, top }, { static_cast<uint32_t>(right > left ? right - left : 0), static_cast<uint32_t>(bottom > top ? bottom - top : 0) } };
    this->m_scissorRectDirty = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// State. Render states land in members; the ones a pipeline bakes (blend, colour mask) are in the pipeline key, the
// rest are VK 1.3 dynamic state set per draw.

void CGxDeviceVulkan::IRsSendToHw(EGxRenderState which) {
    const CGxStateBom& value = this->m_appRenderStates[which].m_value;
    uint32_t master = this->m_appMasterEnables;

    switch (which) {
        case GxRs_PolygonOffset:
            // ponytail: D3D9 units × 2^24 is exact for D24 only (as D3D11); D32S8 gets a different bias.
            this->m_depthBias = -value.m_data.f[0] * 16777216.0f;
            break;

        case GxRs_BlendingMode:
            this->m_blendMode = value.m_data.u[0] < 12 ? value.m_data.u[0] : 0;
            break;

        case GxRs_DepthTest:
        case GxRs_DepthFunc:
            if ((master & 0x4) && this->m_appRenderStates[GxRs_DepthTest].m_value.m_data.i[0]) {
                this->m_depthCompare = s_cmpFunc[this->m_appRenderStates[GxRs_DepthFunc].m_value.m_data.u[0] & 3];
            } else {
                this->m_depthCompare = VK_COMPARE_OP_ALWAYS;
            }

            break;

        case GxRs_DepthWrite:
            this->m_depthWrite = (master & 0x8) && value.m_data.i[0];
            break;

        case GxRs_ColorWrite:
            this->m_colorMask = (master & 0x10) ? value.m_data.u[0] & 0xF : 0;
            break;

        case GxRs_Culling: {
            int32_t cull = (master & 0x20) ? value.m_data.i[0] : 0;
            this->m_cullMode = s_cullMode[cull < 0 ? 0 : (cull > 2 ? 2 : cull)];
            break;
        }

        case GxRs_ScissorTest:
            this->m_scissorTest = value.m_data.i[0] != 0;
            break;

        case GxRs_Texture0:
        case GxRs_Texture1:
        case GxRs_Texture2:
        case GxRs_Texture3:
        case GxRs_Texture4:
        case GxRs_Texture5:
        case GxRs_Texture6:
        case GxRs_Texture7:
        case GxRs_Texture8:
        case GxRs_Texture9:
        case GxRs_Texture10:
        case GxRs_Texture11:
        case GxRs_Texture12:
        case GxRs_Texture13:
        case GxRs_Texture14:
        case GxRs_Texture15:
            if (value.m_data.p) {
                this->ITexMarkAsUpdated(static_cast<CGxTex*>(value.m_data.p));
            }

            break;

        case GxRs_TexGen0:
        case GxRs_TexGen1:
        case GxRs_TexGen2:
        case GxRs_TexGen3:
        case GxRs_TexGen4:
        case GxRs_TexGen5:
        case GxRs_TexGen6:
        case GxRs_TexGen7:
            this->ISetTexGen(which - GxRs_TexGen0, value.m_data.i[0]);
            break;

        case GxRs_VertexShader:
        case GxRs_PixelShader: {
            CGxShader* shader = static_cast<CGxShader*>(value.m_data.p);

            if (shader && !shader->m_loaded) {
                this->IShaderCreate(shader);
            }

            break;
        }

        case Unk84:
            for (uint32_t i = 0; i < 4; i++) {
                this->m_blendFactor[i] = value.m_data.f[0];
            }

            break;

        default:
            // Fixed-function, fog, alpha-ref and clip-plane states are read from m_appRenderStates at draw time.
            break;
    }
}

// CGxDeviceD3d::ISetTexGen: the texgen matrix is taken from the view (and world) current when the state changes.
void CGxDeviceVulkan::ISetTexGen(uint32_t tmu, int32_t texGen) {
    C44Matrix matrix;

    if (texGen == GxTexGen_ObjectLinear || texGen == GxTexGen_EyeLinear) {
        CGxMatrixStack& view = this->m_xforms[GxXform_View];
        matrix = view.m_mtx[view.m_level];
        matrix = matrix.AffineInverse();

        if (texGen == GxTexGen_ObjectLinear) {
            CGxMatrixStack& world = this->m_xforms[GxXform_World];
            matrix = matrix * world.m_mtx[world.m_level].Inverse();
        }
    } else if (texGen == GxTexGen_SphereMap) {
        matrix.a0 = 0.5f;
        matrix.b1 = 0.5f;
        matrix.d0 = 0.5f;
        matrix.d1 = 0.5f;
    } else {
        this->m_texGen[tmu].Identity();
        return;
    }

    CGxMatrixStack& stack = this->m_texGen[tmu];
    stack.m_dirty = 1;
    stack.m_flags[stack.m_level] &= ~CGxMatrixStack::F_Identity;
    stack.m_mtx[stack.m_level] = matrix;
}

// ---------------------------------------------------------------------------------------------------------------
// Pipeline state, constants, textures (phase V2). Model: CGxDeviceD3d11's IStateSync* and FF emulation.

// D3D9 s_filterModes, as D3D11's s_filter11: Nearest/Linear have no mip filter there, so they get max LOD 0.
static const VkFilter s_texFilter[GxTexFilters_Last] = { VK_FILTER_NEAREST, VK_FILTER_LINEAR, VK_FILTER_NEAREST, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_FILTER_LINEAR };
static const VkSamplerMipmapMode s_texMip[GxTexFilters_Last] = { VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR };
static const VkFormat s_dummyAttribFormat[3] = { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT };

// The D3D9 type's format (s_gxAttribFormat) with its numeric class switched to the shader input's (VKS1 compType),
// which Vulkan requires to match: UBYTE4 read as float is USCALED (D3D9 gives 0..255), SHORT2 read as float SSCALED.
static VkFormat AttribFormat(uint32_t type, uint32_t compType) {
    VkFormat format = s_gxAttribFormat[type < 7 ? type : 4];

    if (compType == 0 && format == VK_FORMAT_R8G8B8A8_UINT) {
        return VK_FORMAT_R8G8B8A8_USCALED;
    }

    if (compType == 0 && format == VK_FORMAT_R16G16_SINT) {
        return VK_FORMAT_R16G16_SSCALED;
    }

    if (compType == 1 && (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM)) {
        return VK_FORMAT_R8G8B8A8_UINT;
    }

    return format;
}

// D3DDECLUSAGE + usage index (VKS1 table) -> GxVA bit, -1 when the client has no such attribute.
static int32_t GxVaFromUsage(uint32_t usage, uint32_t index) {
    switch (usage) {
        case 0:
        case 1:
        case 2:
        case 3:
            return index == 0 ? static_cast<int32_t>(usage) : -1;
        case 10:
            return index < 2 ? GxVA_Color0 + static_cast<int32_t>(index) : -1;
        case 5:
            return index < 8 ? GxVA_TexCoord0 + static_cast<int32_t>(index) : -1;
        default:
            return -1;
    }
}

static void ColorToFloat4(uint32_t value, float* out, float alpha = -1.0f) {
    CImVector color;
    memcpy(&color, &value, sizeof(color));
    out[0] = color.r * (1.0f / 255.0f);
    out[1] = color.g * (1.0f / 255.0f);
    out[2] = color.b * (1.0f / 255.0f);
    out[3] = alpha < 0.0f ? color.a * (1.0f / 255.0f) : alpha;
}

// The client's own texLodBias setting, applied in the samplers (as D3D11).
static float TexLodBias() {
    static CVar* cvar;

    if (!cvar) {
        cvar = CVar::Lookup("texLodBias");
    }

    return cvar ? cvar->m_floatValue : 0.0f;
}

// Pipelines are hashed on what they bake: VS, PS, topology class, vertex input (attributes and binding count; strides
// are dynamic), blend mode, colour mask and the pass formats. The cache key uses ShaderApi::m_id, never handles.
VkPipeline CGxDeviceVulkan::IPipelineGet(uint32_t topologyClass, const VkVertexInputAttributeDescription* attribs, uint32_t attribCount, uint32_t bindingCount) {
    VkFormat colorFormat = this->m_passColor ? this->m_passColor->m_format : VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = this->m_passDepth ? this->m_passDepth->m_format : VK_FORMAT_UNDEFINED;
    uint64_t words[8] = { this->m_curVs->m_id, this->m_curPs->m_id, topologyClass, this->m_blendMode, this->m_colorMask, static_cast<uint64_t>(colorFormat), static_cast<uint64_t>(depthFormat), bindingCount };
    uint64_t key = Fnv64(attribs, attribCount * sizeof(*attribs), Fnv64(words, sizeof(words)));
    auto found = this->m_pipelines.find(key);

    if (found != this->m_pipelines.end()) {
        return found->second;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, this->m_curVs->m_module, "main", nullptr },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, this->m_curPs->m_module, "main", nullptr },
    };

    VkVertexInputBindingDescription bindings[GxVAs_Last + 1];

    for (uint32_t i = 0; i < bindingCount; i++) {
        bindings[i] = { i, 0, VK_VERTEX_INPUT_RATE_VERTEX };
    }

    VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = bindingCount;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = attribCount;
    vertexInput.pVertexAttributeDescriptions = attribs;

    static const VkPrimitiveTopology s_classTopology[3] = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = s_classTopology[topologyClass];

    VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    // ponytail: always solid; wireframe (GxMasterEnable_PolygonFill off) needs the key + fillModeNonSolid check.
    VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.depthBiasEnable = VK_TRUE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    bool blend = this->m_blendMode >= 2;
    blendAttachment.blendEnable = blend;
    blendAttachment.srcColorBlendFactor = blend ? s_srcBlend[this->m_blendMode] : VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = blend ? s_dstBlend[this->m_blendMode] : VK_BLEND_FACTOR_ZERO;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = blendAttachment.srcColorBlendFactor;
    blendAttachment.dstAlphaBlendFactor = blendAttachment.dstColorBlendFactor;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = this->m_colorMask;

    VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = this->m_passColor ? 1 : 0;
    colorBlend.pAttachments = &blendAttachment;

    static const VkDynamicState s_dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = sizeof(s_dynamic) / sizeof(s_dynamic[0]);
    dynamic.pDynamicStates = s_dynamic;

    VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount = this->m_passColor ? 1 : 0;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = depthFormat;
    rendering.stencilAttachmentFormat = depthFormat;

    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = this->m_pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;

    // Pipeline creation storms look like freezes: count them and log slow ones.
    static uint32_t s_pipelines;
    static double s_pipelineMs;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    VkResult pipelineResult = vkCreateGraphicsPipelines(this->m_vkDevice, this->m_pipelineCache, 1, &info, nullptr, &pipeline);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    s_pipelines++;
    s_pipelineMs += ms;

    if (s_pipelines <= 5 || s_pipelines % 100 == 0 || ms > 50.0) {
        VkLog("CGxDeviceVulkan: pipeline %u at frame %llu took %.1f ms (%.0f ms total)", s_pipelines, static_cast<unsigned long long>(this->m_frameSerial), ms, s_pipelineMs);
    }

    if (pipelineResult != VK_SUCCESS) {
        VkLog("CGxDeviceVulkan: vkCreateGraphicsPipelines failed (VS %llu PS %llu)", static_cast<unsigned long long>(this->m_curVs->m_id), static_cast<unsigned long long>(this->m_curPs->m_id));
        pipeline = VK_NULL_HANDLE;
    }

    this->m_pipelines[key] = pipeline;
    return pipeline;
}

// Copies into the frame's UBO ring; returns the dynamic offset, UINT32_MAX when the ring is full.
uint32_t CGxDeviceVulkan::IUboPush(const void* data, uint32_t size) {
    Frame& frame = this->m_frames[this->m_frameSerial % kFrames];

    if (frame.m_uboHead + size > kUboRingSize) {
        static int32_t s_logged;

        if (!s_logged) {
            s_logged = 1;
            VkLog("CGxDeviceVulkan: UBO ring full, draws skipped until the next frame");
        }

        return UINT32_MAX;
    }

    uint32_t offset = frame.m_uboHead;
    memcpy(frame.m_uboAlloc.m_map + offset, data, size);
    frame.m_uboHead += static_cast<uint32_t>(AlignUp(size, this->m_physProps.limits.minUniformBufferOffsetAlignment));
    return offset;
}

// App shaders when bound, otherwise the FF permutation for the current vertex format, lighting and textures (D3D11's
// IStateSyncShaders). A bound shader without a module (size-0 permutation, missing pack) skips the draw (SPIRV.md).
bool CGxDeviceVulkan::IStateSyncShaders(bool& ffVs, bool& ffPs) {
    const CGxAppRenderState* rs = this->m_appRenderStates.m_data;
    CGxShader* vs = static_cast<CGxShader*>(rs[GxRs_VertexShader].m_value.m_data.p);
    CGxShader* ps = static_cast<CGxShader*>(rs[GxRs_PixelShader].m_value.m_data.p);
    ffVs = !vs;
    ffPs = !ps;

    if ((vs && (!vs->m_valid || !vs->m_apiSpecific)) || (ps && (!ps->m_valid || !ps->m_apiSpecific))) {
        return false;
    }

    bool lighting = rs[GxRs_Lighting].m_value.m_data.i[0] && (this->m_appMasterEnables & 0x1);
    bool ambientOnly = !rs[GxRs_Lighting].m_value.m_data.i[0] && !(this->m_primVertexMask & (1 << GxVA_Color0)) && rs[GxRs_MatDiffuse].m_value.m_data.u[0] != 0xFFFFFFFF;

    this->m_curVs = ffVs ? &this->m_ffVs[FfSelectVs(this->m_primVertexMask, lighting || ambientOnly)] : static_cast<ShaderApi*>(vs->m_apiSpecific);
    this->m_curPs = ffPs ? &this->m_ffPs[FfSelectPs(rs[GxRs_Texture0].m_value.m_data.p != nullptr, rs[GxRs_Texture1].m_value.m_data.p != nullptr)] : static_cast<ShaderApi*>(ps->m_apiSpecific);
    return this->m_curVs->m_module && this->m_curPs->m_module;
}

// The FF register maps of ffshaders/ff_common.hlsli, filled as D3D11's IStateSyncFixedFunction; pushed into the ring
// when they change or the stage held app constants.
void CGxDeviceVulkan::IStateSyncFixedFunction(bool vs, bool ps, bool lighting, bool ambientOnly) {
    const CGxAppRenderState* rs = this->m_appRenderStates.m_data;
    bool fog = rs[GxRs_Fog].m_value.m_data.i[0] && (this->m_appMasterEnables & 0x2);
    bool specular = rs[GxRs_MatSpecularExp].m_value.m_data.f[0] > 0.0f && rs[GxRs_MatSpecular].m_value.m_data.i[0];

    if (vs) {
        float c[53 * 4];
        memset(c, 0, sizeof(c));

        CGxMatrixStack& world = this->m_xforms[GxXform_World];
        CGxMatrixStack& view = this->m_xforms[GxXform_View];
        const C44Matrix& viewMtx = view.m_mtx[view.m_level];
        C44Matrix worldView = world.m_mtx[world.m_level] * viewMtx;
        memcpy(&c[0 * 4], &worldView, sizeof(C44Matrix));
        memcpy(&c[4 * 4], &this->m_projNative, sizeof(C44Matrix));

        for (uint32_t tmu = 0; tmu < 2; tmu++) {
            CGxMatrixStack& texGen = this->m_texGen[tmu];
            CGxMatrixStack& xform = this->m_xforms[tmu];
            int32_t gen = rs[GxRs_TexGen0 + tmu].m_value.m_data.i[0];
            C44Matrix matrix = texGen.m_mtx[texGen.m_level];
            float mode = 0.0f;

            switch (rs[GxRs_TextureShader0 + tmu].m_value.m_data.i[0]) {
                case 0:
                    mode = (texGen.m_flags[texGen.m_level] & CGxMatrixStack::F_Identity) ? 0.0f : 1.0f;
                    break;

                case 1: {
                    C44Matrix product = texGen.m_mtx[texGen.m_level] * xform.m_mtx[xform.m_level];
                    matrix = product;

                    if (!gen) {
                        matrix.c0 = product.d0;
                        matrix.c1 = product.d1;
                    }

                    mode = 1.0f;
                    break;
                }

                case 2:
                    matrix = texGen.m_mtx[texGen.m_level] * xform.m_mtx[xform.m_level];
                    mode = 2.0f;
                    break;

                default:
                    break;
            }

            float source;

            switch (gen) {
                case GxTexGen_ObjectLinear:
                case GxTexGen_EyeLinear:
                case GxTexGen_Unk3:
                    source = 8.0f;
                    break;

                case GxTexGen_ReflectionMap:
                case GxTexGen_SphereMap:
                    source = 10.0f;
                    break;

                case GxTexGen_Normal:
                    source = 9.0f;
                    break;

                default:
                    source = static_cast<float>(rs[GxRs_TextureCoord0 + tmu].m_value.m_data.u[0] & 7);
                    break;
            }

            memcpy(&c[(8 + tmu * 4) * 4], &matrix, sizeof(C44Matrix));
            c[(21 + tmu) * 4 + 0] = source;
            c[(21 + tmu) * 4 + 1] = mode;
        }

        // Material (ISetMaterial: ambient = diffuse).
        ColorToFloat4(rs[GxRs_MatEmissive].m_value.m_data.u[0], &c[16 * 4]);
        ColorToFloat4(rs[GxRs_MatDiffuse].m_value.m_data.u[0], &c[17 * 4]);
        ColorToFloat4(rs[GxRs_MatSpecular].m_value.m_data.u[0], &c[18 * 4], rs[GxRs_MatSpecularExp].m_value.m_data.f[0]);

        if (ambientOnly) {
            c[19 * 4 + 0] = c[19 * 4 + 1] = c[19 * 4 + 2] = c[19 * 4 + 3] = 1.0f;
        }

        if (fog) {
            float start = rs[GxRs_FogStart].m_value.m_data.f[0];
            float end = rs[GxRs_FogEnd].m_value.m_data.f[0];
            float range = end - start != 0.0f ? end - start : 1.0f;
            c[20 * 4 + 0] = -1.0f / range;
            c[20 * 4 + 1] = end / range;
        } else {
            c[20 * 4 + 1] = 1.0f;
        }

        if (this->m_primVertexMask & (1 << GxVA_Color0)) {
            int32_t colorMaterial = rs[GxRs_ColorMaterial].m_value.m_data.i[0];
            c[23 * 4 + 0] = colorMaterial == 0;
            c[23 * 4 + 1] = colorMaterial == 1;
            c[23 * 4 + 2] = colorMaterial == 2;
        }

        c[23 * 4 + 3] = specular;

        for (uint32_t i = 0; lighting && !ambientOnly && i < 4; i++) {
            const CGxApiLight& light = this->m_lights[i];

            if (!light.m_enable) {
                continue;
            }

            float* l = &c[(24 + 5 * i) * 4];
            bool point = light.m_dir.w == 1.0f;
            C4Vector dir;
            dir.x = light.m_dir.x;
            dir.y = light.m_dir.y;
            dir.z = light.m_dir.z;
            dir.w = point ? 1.0f : 0.0f;
            dir = dir * viewMtx;

            l[0] = dir.x;
            l[1] = dir.y;
            l[2] = dir.z;
            l[3] = point ? 2.0f : 1.0f;
            l[4] = light.m_dirColor.x;
            l[5] = light.m_dirColor.y;
            l[6] = light.m_dirColor.z;
            l[7] = 1.0f;
            l[8] = light.m_ambColor.x;
            l[9] = light.m_ambColor.y;
            l[10] = light.m_ambColor.z;
            l[12] = light.m_specColor.x;
            l[13] = light.m_specColor.y;
            l[14] = light.m_specColor.z;
            l[16] = light.m_constantAttenuation;
            l[17] = light.m_linearAttenuation;
            l[18] = light.m_quadraticAttenuation;
            l[19] = 10000.0f;
        }

        c[50 * 4] = rs[GxRs_NormalizeNormals].m_value.m_data.u[0] != 0;

        const float* attenuation = rs[GxRs_PointScaleAttenuation].m_value.m_data.f;
        c[51 * 4 + 0] = rs[GxRs_PointScale].m_value.m_data.f[0];
        c[51 * 4 + 1] = *reinterpret_cast<const C3Vector*>(attenuation) != CGxDevice::s_pointScaleAttenuation;
        c[52 * 4 + 0] = attenuation[0];
        c[52 * 4 + 1] = attenuation[1];
        c[52 * 4 + 2] = attenuation[2];
        c[52 * 4 + 3] = (this->m_viewport.m_y.m_h - this->m_viewport.m_y.m_l) * this->DeviceCurWindow().maxY;

        if (this->m_uboOffsets[0] == UINT32_MAX || this->m_cbSource[0] != 1 || memcmp(c, this->m_ffConstants[0], sizeof(c))) {
            memcpy(this->m_ffConstants[0], c, sizeof(c));
            this->m_uboOffsets[0] = this->IUboPush(c, sizeof(c));
            this->m_cbSource[0] = 1;
        }
    }

    if (ps) {
        float c[3 * 4];
        memset(c, 0, sizeof(c));

        ColorToFloat4(rs[GxRs_FogColor].m_value.m_data.u[0], &c[0], fog ? 1.0f : 0.0f);

        for (uint32_t tmu = 0; tmu < 2; tmu++) {
            bool enabled = rs[GxRs_Texture0 + tmu].m_value.m_data.p != nullptr;
            c[4 + tmu * 2 + 0] = enabled ? static_cast<float>(rs[GxRs_ColorOp0 + tmu].m_value.m_data.u[0]) : 6.0f;
            c[4 + tmu * 2 + 1] = enabled ? static_cast<float>(rs[GxRs_AlphaOp0 + tmu].m_value.m_data.u[0]) : 6.0f;
        }

        c[8] = specular;

        if (this->m_uboOffsets[1] == UINT32_MAX || this->m_cbSource[1] != 1 || memcmp(c, this->m_ffConstants[1], sizeof(c))) {
            memcpy(this->m_ffConstants[1], c, sizeof(c));
            this->m_uboOffsets[1] = this->IUboPush(c, sizeof(c));
            this->m_cbSource[1] = 1;
        }
    }
}

// Set 0 contents for the draw: VS/PS b0 (app shadow constants or FF maps) and cb1, each pushed into the frame's UBO
// ring when it changed and bound by dynamic offset. Dynamic offsets over one set per frame need no descriptor writes
// per draw, which is why they won over a set per draw. App b0 pushes cover registers 0..highest ever written (the
// binding stays 4 KB; the ring keeps 4 KB of slack). False skips the draw (ring full).
bool CGxDeviceVulkan::IStateSyncConstants(bool ffVs, bool ffPs) {
    const CGxAppRenderState* rs = this->m_appRenderStates.m_data;
    bool lighting = rs[GxRs_Lighting].m_value.m_data.i[0] && (this->m_appMasterEnables & 0x1);
    bool ambientOnly = !rs[GxRs_Lighting].m_value.m_data.i[0] && !(this->m_primVertexMask & (1 << GxVA_Color0)) && rs[GxRs_MatDiffuse].m_value.m_data.u[0] != 0xFFFFFFFF;

    for (uint32_t stage = 0; stage < 2; stage++) {
        ShadowConstants& shadow = CGxDevice::s_shadowConstants[stage == 0 ? 1 : 0];
        bool dirty = shadow.dirtyMin <= shadow.dirtyMax;

        if (dirty && shadow.dirtyMax > this->m_cbHigh[stage]) {
            this->m_cbHigh[stage] = shadow.dirtyMax < 255 ? shadow.dirtyMax : 255;
        }

        if (!(stage == 0 ? ffVs : ffPs) && (dirty || this->m_cbSource[stage] != 0 || this->m_uboOffsets[stage] == UINT32_MAX)) {
            this->m_uboOffsets[stage] = this->IUboPush(shadow.constants, (this->m_cbHigh[stage] + 1) * 16);
            this->m_cbSource[stage] = 0;
        }

        shadow.dirtyMin = 255;
        shadow.dirtyMax = 0;
    }

    if (ffVs || ffPs) {
        this->IStateSyncFixedFunction(ffVs, ffPs, lighting || ambientOnly, ambientOnly);
    }

    // cb1 as D3D11's IStateSyncDeviceConstants: int constants (none: stock shaders only use defi), alpha-test ref,
    // user clip planes in clip space (FF planes are world space in D3D9, so they go through inv(view*proj)).
    float dev[23 * 4];
    memset(dev, 0, sizeof(dev));

    int32_t alphaRef = rs[GxRs_AlphaRef].m_value.m_data.i[0];
    dev[16 * 4] = alphaRef > 0 ? alphaRef * (1.0f / 255.0f) : 0.0f;

    uint32_t mask = rs[GxRs_ClipPlaneMask].m_value.m_data.u[0] & 0x3F;

    if (mask) {
        C44Matrix toClip;

        if (ffVs) {
            CGxMatrixStack& view = this->m_xforms[GxXform_View];
            toClip = (view.m_mtx[view.m_level] * this->m_projNative).Inverse().Transpose();
        }

        for (uint32_t i = 0; i < 6; i++) {
            if (!(mask & (1 << i))) {
                continue;
            }

            const C4Plane& plane = this->m_clipPlane[i];
            C4Vector p;
            p.x = plane.n.x;
            p.y = plane.n.y;
            p.z = plane.n.z;
            p.w = plane.d;

            if (ffVs) {
                p = p * toClip;
            }

            memcpy(&dev[(17 + i) * 4], &p, sizeof(p));
        }
    }

    this->m_clipPlaneMask = 0;

    if (this->m_uboOffsets[2] == UINT32_MAX || memcmp(dev, this->m_devConstants, sizeof(dev))) {
        memcpy(this->m_devConstants, dev, sizeof(dev));
        this->m_uboOffsets[2] = this->IUboPush(dev, sizeof(dev));
    }

    return this->m_uboOffsets[0] != UINT32_MAX && this->m_uboOffsets[1] != UINT32_MAX && this->m_uboOffsets[2] != UINT32_MAX;
}

// Sampler cache keyed like D3D11's texfilter handling: filter, wrap U/V, anisotropy, texLodBias.
VkSampler CGxDeviceVulkan::ISamplerGet(uint32_t filter, bool wrapU, bool wrapV, uint32_t anisotropy, float lodBias) {
    filter = filter < GxTexFilters_Last ? filter : GxTex_Linear;
    anisotropy = anisotropy < 1 ? 1 : (anisotropy > this->m_caps.m_maxTexAnisotropy ? this->m_caps.m_maxTexAnisotropy : anisotropy);

    if (filter == GxTex_Anisotropic && anisotropy <= 1) {
        filter = GxTex_LinearMipLinear;
    }

    uint32_t bias;
    memcpy(&bias, &lodBias, sizeof(bias));
    uint64_t key = filter | (wrapU << 3) | (wrapV << 4) | (anisotropy << 8) | (static_cast<uint64_t>(bias) << 32);
    auto found = this->m_samplers.find(key);

    if (found != this->m_samplers.end()) {
        return found->second;
    }

    VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = info.minFilter = s_texFilter[filter];
    info.mipmapMode = s_texMip[filter];
    info.addressModeU = wrapU ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = wrapV ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.mipLodBias = lodBias;
    info.anisotropyEnable = filter == GxTex_Anisotropic;
    info.maxAnisotropy = static_cast<float>(anisotropy);
    info.maxLod = filter <= GxTex_Linear ? 0.0f : VK_LOD_CLAMP_NONE;

    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(this->m_vkDevice, &info, nullptr, &sampler);
    this->m_samplers[key] = sampler;
    return sampler;
}

// Set 1 for the draw: t0..t15 / s0..s15 from GxRs_Texture0..15, shared by both stages (a VS sN lands on the same
// binding). Empty slots get the 1x1 zero texture. A new set (from the frame's pools, reset each frame) only when a
// view or sampler changed since the previous draw.
VkDescriptorSet CGxDeviceVulkan::IStateSyncTextures() {
    float lodBias = TexLodBias();
    VkImageView views[16];
    VkSampler samplers[16];

    for (uint32_t i = 0; i < 16; i++) {
        CGxTex* texture = static_cast<CGxTex*>(this->m_appRenderStates[GxRs_Texture0 + i].m_value.m_data.p);
        Image* image = texture && !texture->m_needsCreation ? static_cast<Image*>(texture->m_apiSpecificData) : nullptr;
        views[i] = image && image->m_view ? image->m_view : this->m_dummyImage.m_view;
        samplers[i] = texture ? this->ISamplerGet(texture->m_flags.m_filter, texture->m_flags.m_wrapU, texture->m_flags.m_wrapV, texture->m_flags.m_maxAnisotropy, lodBias) : this->ISamplerGet(GxTex_Linear, false, false, 1, lodBias);
    }

    if (this->m_curTexSet && !memcmp(views, this->m_curViews, sizeof(views)) && !memcmp(samplers, this->m_curSamplers, sizeof(samplers))) {
        return this->m_curTexSet;
    }

    Frame& frame = this->m_frames[this->m_frameSerial % kFrames];
    VkDescriptorSet set = VK_NULL_HANDLE;

    while (!set) {
        if (frame.m_texPool == frame.m_texPools.size()) {
            VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024 * 16 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1024 * 16 } };
            VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            poolInfo.maxSets = 1024;
            poolInfo.poolSizeCount = 2;
            poolInfo.pPoolSizes = sizes;
            VkDescriptorPool pool;

            if (vkCreateDescriptorPool(this->m_vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
                return VK_NULL_HANDLE;
            }

            frame.m_texPools.push_back(pool);
        }

        VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = frame.m_texPools[frame.m_texPool];
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &this->m_setLayouts[1];

        if (vkAllocateDescriptorSets(this->m_vkDevice, &allocInfo, &set) != VK_SUCCESS) {
            set = VK_NULL_HANDLE;
            frame.m_texPool++;
        }
    }

    VkDescriptorImageInfo imageInfo[16];
    VkDescriptorImageInfo samplerInfo[16];

    for (uint32_t i = 0; i < 16; i++) {
        // ponytail: every sampled texture is assumed SHADER_READ_ONLY (texture targets return there after their pass).
        imageInfo[i] = { VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        samplerInfo[i] = { samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }

    // Bindings 0..15 (and 16..31) have one descriptor each and the same type, so one write rolls over all 16.
    VkWriteDescriptorSet writes[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    writes[0].dstSet = writes[1].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 16;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = imageInfo;
    writes[1].dstBinding = 16;
    writes[1].descriptorCount = 16;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = samplerInfo;
    vkUpdateDescriptorSets(this->m_vkDevice, 2, writes, 0, nullptr);

    memcpy(this->m_curViews, views, sizeof(views));
    memcpy(this->m_curSamplers, samplers, sizeof(samplers));
    this->m_curTexSet = set;
    return set;
}

static VkDeviceSize PoolBase(const CGxPool* pool, uint32_t version) {
    return pool->m_usage == GxPoolUsage_Static ? 0 : static_cast<VkDeviceSize>(version) * pool->m_size;
}

void CGxDeviceVulkan::Draw(CGxBatch* batch, int32_t count) {
    if (!this->m_context || this->m_intF5C) {
        return;
    }

    this->IBeginRendering();

    if (!this->m_rendering) {
        return;
    }

    this->IRsSync(0);

    if (this->m_viewportDirty) {
        this->IXformSetViewport();
    }

    this->IStateSyncScissorRect();

    bool ffVs;
    bool ffPs;

    if (!this->IStateSyncShaders(ffVs, ffPs) || !this->IStateSyncConstants(ffVs, ffPs)) {
        return;
    }

    VkDescriptorSet texSet = this->IStateSyncTextures();

    if (!texSet) {
        return;
    }

    // Streams as D3D11's IStateSyncVertexPtrs: one binding per distinct buffer, stride = sum of its attributes
    // (predefined formats: one buffer, m_primVertexSize).
    bool predefined = this->m_primVertexFormat < GxVertexBufferFormats_Last;
    CGxBuf* streamBufs[GxVAs_Last + 1] = {};
    VkDeviceSize streamStrides[GxVAs_Last + 1] = {};
    uint32_t streamOf[GxVAs_Last] = {};
    uint32_t streamCount = 0;

    for (uint32_t i = 0; i < GxVAs_Last; i++) {
        if (!(this->m_primVertexMask & (1 << i))) {
            continue;
        }

        CGxBuf* buf = predefined ? this->m_primVertexBuf : this->m_primVertexFormatBuf[i];
        uint32_t stream;

        for (stream = 0; stream < streamCount && streamBufs[stream] != buf; stream++) {
        }

        if (stream == streamCount) {
            streamBufs[streamCount++] = buf;
        }

        streamOf[i] = stream;
        streamStrides[stream] = predefined ? this->m_primVertexSize : streamStrides[stream] + s_gxAttribSize[this->m_primVertexFormatAttrib[i].m_type < 7 ? this->m_primVertexFormatAttrib[i].m_type : 0];
    }

    // Vertex input from the VS's VKS1 table: (usage, usageIndex) -> GxVA. Inputs the decl lacks read the zero buffer
    // through one zero-stride binding after the streams.
    VkVertexInputAttributeDescription attribs[32];
    uint32_t attribCount = this->m_curVs->m_inputCount;
    uint32_t bindingCount = streamCount;

    for (uint32_t j = 0; j < attribCount; j++) {
        const uint8_t* input = this->m_curVs->m_inputs[j];
        int32_t va = GxVaFromUsage(input[0], input[1]);

        if (va >= 0 && (this->m_primVertexMask & (1 << va))) {
            const CGxVertexAttrib& attrib = this->m_primVertexFormatAttrib[va];
            attribs[j] = { input[2], streamOf[va], AttribFormat(attrib.m_type, input[3]), attrib.m_offset };
        } else {
            bindingCount = streamCount + 1;
            attribs[j] = { input[2], streamCount, s_dummyAttribFormat[input[3] < 3 ? input[3] : 0], 0 };
        }
    }

    VkBuffer buffers[GxVAs_Last + 1];
    VkDeviceSize offsets[GxVAs_Last + 1];

    for (uint32_t s = 0; s < streamCount; s++) {
        PoolApi* api = streamBufs[s] ? static_cast<PoolApi*>(streamBufs[s]->m_pool->m_apiSpecific) : nullptr;

        if (!api) {
            return;
        }

        buffers[s] = api->m_buffer;
        offsets[s] = PoolBase(streamBufs[s]->m_pool, api->m_version) + streamBufs[s]->m_index;
    }

    if (bindingCount > streamCount) {
        buffers[streamCount] = this->m_dummyUbo;
        offsets[streamCount] = 0;
        streamStrides[streamCount] = 0;
    }

    PoolApi* indexApi = count && this->m_primIndexBuf ? static_cast<PoolApi*>(this->m_primIndexBuf->m_pool->m_apiSpecific) : nullptr;

    if (count && !indexApi) {
        return;
    }

    uint32_t topologyClass = batch->m_primType == GxPrim_Points ? 0 : (batch->m_primType <= GxPrim_LineStrip ? 1 : 2);
    VkPipeline pipeline = this->IPipelineGet(topologyClass, attribs, attribCount, bindingCount);

    if (!pipeline) {
        return;
    }

    VkCommandBuffer cmd = this->m_frames[this->m_frameSerial % kFrames].m_cmd;

    if (this->m_curPipeline != pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        this->m_curPipeline = pipeline;
    }

    VkDescriptorSet sets[2] = { this->m_uboSets[this->m_frameSerial % kFrames], texSet };
    uint32_t dynamicOffsets[4] = { this->m_uboOffsets[0], this->m_uboOffsets[1], this->m_uboOffsets[2], 0 };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->m_pipelineLayout, 0, 2, sets, this->m_uboBindingCount, dynamicOffsets);

    // ponytail: every dynamic state is set on every draw; track them if command-buffer size shows up.
    VkRect2D scissor = this->m_scissorTest ? this->m_vkScissor : VkRect2D { { 0, 0 }, this->m_passExtent };
    vkCmdSetViewport(cmd, 0, 1, &this->m_vkViewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetPrimitiveTopology(cmd, s_primitiveConversion[batch->m_primType]);
    vkCmdSetCullMode(cmd, this->m_cullMode);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetDepthTestEnable(cmd, this->m_passDepth != nullptr);
    vkCmdSetDepthWriteEnable(cmd, this->m_depthWrite);
    vkCmdSetDepthCompareOp(cmd, this->m_depthCompare);
    vkCmdSetDepthBias(cmd, this->m_depthBias, 0.0f, 0.0f);
    vkCmdSetBlendConstants(cmd, this->m_blendFactor);

    if (bindingCount) {
        vkCmdBindVertexBuffers2(cmd, 0, bindingCount, buffers, offsets, nullptr, streamStrides);
    }

    if (count) {
        vkCmdBindIndexBuffer(cmd, indexApi->m_buffer, PoolBase(this->m_primIndexBuf->m_pool, indexApi->m_version), VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, batch->m_count, 1, batch->m_start + (this->m_primIndexBuf->m_index >> 1), 0, 0);
    } else {
        vkCmdDraw(cmd, batch->m_count, 1, 0, 0);
    }
}

// Same as CGxDeviceD3d::XformSetProjection: Vulkan clip space (negative-height viewport) has the same 0..1 depth range as D3D9.
void CGxDeviceVulkan::XformSetProjection(const C44Matrix& matrix) {
    CGxDevice::XformSetProjection(matrix);

    C44Matrix projNative = matrix;

    if (!(fabs(projNative.c3 - 1.0f) < 0.00000023841858f) && !(fabs(projNative.c3) < 0.00000023841858f)) {
        projNative *= 1.0f / projNative.c3;
    }

    if (projNative.d3 == 0.0f) {
        float minZ = -(projNative.d2 / (projNative.c2 + 1.0f));
        float maxZ = -(projNative.d2 / (projNative.c2 - 1.0f));
        projNative.c2 = maxZ / (maxZ - minZ);
        projNative.d2 = maxZ * minZ / (minZ - maxZ);
    } else {
        float invC2 = 1.0f / projNative.c2;
        float minZ = (-1.0f - projNative.d2) * invC2;
        float maxZ = (1.0f - projNative.d2) * invC2;
        projNative.c2 = 1.0f / (maxZ - minZ);
        projNative.d2 = minZ / (minZ - maxZ);
    }

    if (!(this->m_appMasterEnables & (1 << GxMasterEnable_NormalProjection)) && projNative.d3 != 1.0f) {
        C44Matrix scale;
        scale.a0 = 0.2f;
        scale.b1 = 0.2f;
        scale.c2 = 0.2f;
        projNative = projNative * scale;
    }

    this->m_xforms[GxXform_Projection].m_dirty = 1;
    this->m_projNative = projNative;
}


// ---------------------------------------------------------------------------------------------------------------
// Buffer pools

int32_t CGxDeviceVulkan::CreatePoolAPI(CGxPool* pool) {
    if (!this->m_vkDevice || pool->m_size <= 0) {
        return 0;
    }

    bool isStatic = pool->m_usage == GxPoolUsage_Static;
    VkBufferUsageFlags usage = (pool->m_target == GxPoolTarget_Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT) | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkDeviceSize size = isStatic ? pool->m_size : static_cast<VkDeviceSize>(pool->m_size) * kPoolVersions;

    PoolApi* api = new PoolApi;
    memset(api, 0, sizeof(*api));

    if (!this->ICreateBuffer(size, usage, !isStatic, api->m_buffer, api->m_alloc)) {
        delete api;
        return 0;
    }

    // Static pools write into a CPU copy and push the locked range through staging; the others write in place.
    if (isStatic) {
        api->m_shadow = static_cast<char*>(malloc(pool->m_size));

        if (!api->m_shadow) {
            vkDestroyBuffer(this->m_vkDevice, api->m_buffer, nullptr);
            this->IFree(api->m_alloc);
            delete api;
            return 0;
        }
    }

    pool->m_apiSpecific = api;
    return 1;
}

void CGxDeviceVulkan::IReleasePool(CGxPool* pool) {
    if (pool->m_usage == GxPoolUsage_Stream) {
        pool->unk_001C = 0;
    }

    pool->Invalidate();
    PoolApi* api = static_cast<PoolApi*>(pool->m_apiSpecific);

    if (!api) {
        return;
    }

    VkDevice device = this->m_vkDevice;
    VkBuffer buffer = api->m_buffer;
    Alloc alloc = api->m_alloc;
    this->IDefer([this, device, buffer, alloc]() mutable {
        vkDestroyBuffer(device, buffer, nullptr);
        this->IFree(alloc);
    });

    free(api->m_shadow);
    delete api;
    pool->m_apiSpecific = nullptr;
}

void CGxDeviceVulkan::IReleasePools(int32_t freePools) {
    for (CGxPool* pool = this->m_poolList.Head(); pool; pool = this->m_poolList.Next(pool)) {
        if (freePools || pool->m_usage == GxPoolUsage_Stream || pool->m_usage == GxPoolUsage_Dynamic) {
            this->IReleasePool(pool);
        }
    }
}

void CGxDeviceVulkan::PoolSizeSet(CGxPool* pool, uint32_t size) {
    this->IReleasePool(pool);
    pool->m_size = size;
    this->CreatePoolAPI(pool);
}

void CGxDeviceVulkan::PoolDestroy(CGxPool* pool) {
    if (pool) {
        this->IReleasePool(pool);
        CGxDevice::PoolDestroy(pool);
    }
}

// Stream-pool ring as CGxDeviceD3d::IBufLock. A wrap is D3D's DISCARD: the pool moves to its next copy, waiting
// for the frame that last used that copy. Dynamic pools write in place (D3D9 NOOVERWRITE, as on D3D11).
char* CGxDeviceVulkan::IBufLock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;

    if (!this->m_context) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    if (!pool->m_apiSpecific) {
        this->CreatePoolAPI(pool);
    }

    PoolApi* api = static_cast<PoolApi*>(pool->m_apiSpecific);

    if (!api) {
        return static_cast<char*>(this->m_emergencyMem[pool->m_target].Lock(buf->m_size));
    }

    if (pool->m_usage == GxPoolUsage_Stream) {
        uint32_t index = (buf->m_itemSize + pool->unk_001C - 1) - (buf->m_itemSize + pool->unk_001C - 1) % buf->m_itemSize;

        if (index + buf->m_size > static_cast<uint32_t>(pool->m_size)) {
            pool->Discard();
            buf->m_index = 0;
            pool->unk_001C = buf->m_size;

            api->m_versionSerial[api->m_version] = this->m_frameSerial;
            api->m_version = (api->m_version + 1) % kPoolVersions;

            // ponytail: more than kPoolVersions - 1 wraps in one frame overwrite data the GPU has not read yet;
            // grow the pool (or kPoolVersions) if the log line appears.
            if (api->m_versionSerial[api->m_version] == this->m_frameSerial && this->m_frameSerial != 0) {
                static int32_t s_logged;

                if (!s_logged) {
                    s_logged = 1;
                    VkLog("CGxDeviceVulkan: stream pool wrapped %u times in one frame", kPoolVersions);
                }
            }

            this->IWaitSerial(api->m_versionSerial[api->m_version]);
        } else {
            buf->m_index = index;
            pool->unk_001C = index + buf->m_size;
        }
    }

    if (api->m_shadow) {
        return api->m_shadow + buf->m_index;
    }

    return api->m_alloc.m_map + PoolBase(pool, api->m_version) + buf->m_index;
}

void CGxDeviceVulkan::IBufUnlock(CGxBuf* buf) {
    CGxPool* pool = buf->m_pool;
    EmergencyMem& emergencyMem = this->m_emergencyMem[pool->m_target];

    if (emergencyMem.m_lock) {
        emergencyMem.Unlock();
        buf->unk_001D = 0;
        this->m_intF5C = 1;
        return;
    }

    PoolApi* api = static_cast<PoolApi*>(pool->m_apiSpecific);

    if (!api || !buf->m_size) {
        buf->unk_001D = api != nullptr;
        return;
    }

    if (api->m_shadow) {
        // ponytail: a static pool rewritten mid-frame shows its new contents to the whole frame (the copy runs in the
        // upload command buffer ahead of every draw); static pools are written once in practice.
        VkBuffer staging;
        VkDeviceSize offset;
        char* dst = this->IStaging(buf->m_size, staging, offset);

        if (!dst) {
            buf->unk_001D = 0;
            return;
        }

        memcpy(dst, api->m_shadow + buf->m_index, buf->m_size);
        VkBufferCopy copy = { offset, buf->m_index, buf->m_size };
        vkCmdCopyBuffer(this->m_frames[this->m_frameSerial % kFrames].m_uploadCmd, staging, api->m_buffer, 1, &copy);
    }

    buf->unk_001D = 1;
}

char* CGxDeviceVulkan::BufLock(CGxBuf* buf) {
    CGxDevice::BufLock(buf);
    return this->IBufLock(buf);
}

int32_t CGxDeviceVulkan::BufUnlock(CGxBuf* buf, uint32_t size) {
    CGxDevice::BufUnlock(buf, size);
    this->IBufUnlock(buf);
    return 1;
}

void CGxDeviceVulkan::BufData(CGxBuf* buf, const void* data, uint32_t size, uint32_t offset) {
    CGxDevice::BufData(buf, data, size, offset);
    char* dst = this->IBufLock(buf);
    memcpy(dst + offset, data, size);
    this->IBufUnlock(buf);
}

// ---------------------------------------------------------------------------------------------------------------
// Textures: CGxTex::m_apiSpecificData is an Image*.

void CGxDeviceVulkan::ITexRelease(CGxTex* texId) {
    Image* image = static_cast<Image*>(texId->m_apiSpecificData);

    if (!image) {
        return;
    }

    this->IDestroyImage(*image, true);
    delete image;
    texId->m_apiSpecificData = nullptr;
}

void CGxDeviceVulkan::ITexCreate(CGxTex* texId) {
    this->ITexRelease(texId);

    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    if (!this->m_caps.m_texFmt[texId->m_format]) {
        texId->m_format = s_tolerableTexFmtMapping[texId->m_format];
    }

    bool cube = texId->m_target == GxTex_CubeMap;
    bool depth = texId->m_format == GxTex_D24X8;
    bool renderTarget = texId->m_flags.m_renderTarget;
    bool compressed = texId->m_format >= GxTex_Dxt1 && texId->m_format <= GxTex_Dxt5;
    VkFormat format = depth ? this->m_depthFormat : s_gxTexFmtToVk[texId->m_format];

    // Mips are generated with linear blits, where the format allows it.
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(this->m_physDevice, format, &props);
    VkFormatFeatureFlags blitNeeds = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    bool generateMips = texId->m_flags.m_generateMipMaps && !depth && !compressed && (props.optimalTilingFeatures & blitNeeds) == blitNeeds;

    uint32_t levels = mipCount - baseMip;

    if (depth) {
        levels = 1;
    } else if (generateMips) {
        levels = 1;

        for (uint32_t size = width > height ? width : height; size > 1; size >>= 1) {
            levels++;
        }
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (depth) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (renderTarget) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    Image* image = new Image;

    if (!this->ICreateImage(width, height, levels ? levels : 1, cube ? 6 : 1, format, usage, cube, *image)) {
        delete image;
        return;
    }

    texId->m_apiSpecificData = image;
    texId->m_needsCreation = 0;
}

// CGxDeviceVulkan::ITexUpload with UpdateSubresource replaced by Blit into staging + vkCmdCopyBufferToImage in the
// upload command buffer.
void CGxDeviceVulkan::ITexUpload(CGxTex* texId) {
    uint32_t texelStrideInBytes;
    const void* texels = nullptr;
    texId->m_userFunc(GxTex_Lock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    Image* image = static_cast<Image*>(texId->m_apiSpecificData);
    int32_t compressed = texId->m_dataFormat >= GxTex_Dxt1 && texId->m_dataFormat <= GxTex_Dxt5;
    int32_t destCompressed = texId->m_format >= GxTex_Dxt1 && texId->m_format <= GxTex_Dxt5;
    uint32_t faceCount = texId->m_target == GxTex_CubeMap ? 6 : 1;
    this->IBeginFrame();
    VkCommandBuffer cmd = this->m_frames[this->m_frameSerial % kFrames].m_uploadCmd;
    bool copied = false;

    for (uint32_t face = 0; face < faceCount && !texId->m_flags.m_renderTarget; face++) {
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        uint32_t level = 0;

        for (uint32_t mip = baseMip; mip != mipCount && level < image->m_levels; mip++, level++) {
            texels = nullptr;
            texId->m_userFunc(GxTex_Latch, mipWidth, mipHeight, face, mip, texId->m_userArg, texelStrideInBytes, texels);

            if (texels) {
                int32_t left = texId->m_updateRect.minX >> mip;
                int32_t top = texId->m_updateRect.minY >> mip;
                int32_t bottom = ((texId->m_updateRect.maxY - 1) >> mip) + 1;
                int32_t right = ((texId->m_updateRect.maxX - 1) >> mip) + 1;
                bottom = bottom > static_cast<int32_t>(mipHeight) ? mipHeight : bottom;
                right = right > static_cast<int32_t>(mipWidth) ? mipWidth : right;
                bottom = bottom <= top ? top + 1 : bottom;
                right = right <= left ? left + 1 : right;

                if (destCompressed) {
                    left &= ~3;
                    top &= ~3;
                    right = (right + 3) & ~3;
                    bottom = (bottom + 3) & ~3;
                    bottom = bottom > static_cast<int32_t>(mipHeight) ? mipHeight : bottom;
                    right = right > static_cast<int32_t>(mipWidth) ? mipWidth : right;
                }

                const uint8_t* src = static_cast<const uint8_t*>(texels);

                if (!texId->m_flags.m_bit15) {
                    EGxTexFormat dataFormat = texId->m_dataFormat;

                    if (compressed) {
                        src += s_texFormatBytesPerBlock[dataFormat] * (left >> 2) + texelStrideInBytes * (top >> 2);
                    } else {
                        src += ((left * s_texFormatBitDepth[dataFormat]) >> 3) + texelStrideInBytes * top;
                    }
                }

                C2iVector size;
                size.x = right - left;
                size.y = bottom - top;
                uint32_t pitch;
                uint32_t rows;

                if (destCompressed) {
                    pitch = ((size.x + 3) >> 2) * s_texFormatBytesPerBlock[texId->m_format];
                    rows = (size.y + 3) >> 2;
                } else {
                    pitch = (size.x * s_texFormatBitDepth[texId->m_format] + 7) >> 3;
                    rows = size.y;
                }

                VkBuffer staging;
                VkDeviceSize offset;
                char* dst = this->IStaging(pitch * rows, staging, offset);

                if (!dst) {
                    break;
                }

                Blit(size, BlitAlpha_0, src, texelStrideInBytes, GxGetBlitFormat(texId->m_dataFormat), dst, pitch, GxGetBlitFormat(texId->m_format));
                this->ITransition(cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                VkBufferImageCopy region = {};
                region.bufferOffset = offset;
                region.bufferRowLength = destCompressed ? (pitch / s_texFormatBytesPerBlock[texId->m_format]) * 4 : 0;
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1 };
                region.imageOffset = { left, top, 0 };
                region.imageExtent = { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), 1 };
                vkCmdCopyBufferToImage(cmd, staging, image->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                copied = true;
            }

            mipHeight = mipHeight > 1 ? mipHeight >> 1 : 1;
            mipWidth = mipWidth > 1 ? mipWidth >> 1 : 1;
        }
    }

    // Generated mips: level i-1 → i with linear blits (ITexCreate only sizes the chain when the format can blit).
    if (copied && texId->m_flags.m_generateMipMaps && !destCompressed && image->m_levels > 1 && image->m_levels > mipCount - baseMip) {
        for (uint32_t level = 1; level < image->m_levels; level++) {
            LevelBarrier(cmd, image->m_image, level - 1, image->m_layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            int32_t srcW = static_cast<int32_t>(image->m_width >> (level - 1));
            int32_t srcH = static_cast<int32_t>(image->m_height >> (level - 1));
            VkImageBlit blit = {};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, image->m_layers };
            blit.srcOffsets[1] = { srcW ? srcW : 1, srcH ? srcH : 1, 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, image->m_layers };
            blit.dstOffsets[1] = { srcW > 1 ? srcW >> 1 : 1, srcH > 1 ? srcH >> 1 : 1, 1 };
            vkCmdBlitImage(cmd, image->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }

        LevelBarrier(cmd, image->m_image, image->m_levels - 1, image->m_layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image->m_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    if (copied) {
        this->ITransition(cmd, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    texels = nullptr;
    texId->m_userFunc(GxTex_Unlock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
}

void CGxDeviceVulkan::ITexMarkAsUpdated(CGxTex* texId) {
    if (!texId->m_needsUpdate || !this->m_context) {
        return;
    }

    if (texId->m_needsCreation || !texId->m_apiSpecificData) {
        this->ITexCreate(texId);
    }

    if (!texId->m_needsCreation && texId->m_apiSpecificData) {
        if (texId->m_userFunc) {
            this->ITexUpload(texId);
        }

        CGxDevice::ITexMarkAsUpdated(texId);
    }
}

void CGxDeviceVulkan::ITexForceRecreation(int32_t all) {
    bool unbound = false;

    for (CGxTex* texId = this->m_textures.Head(); texId; texId = this->m_textures.Next(texId)) {
        if (!texId->m_apiSpecificData || !(all || texId->m_flags.m_renderTarget)) {
            continue;
        }

        static CiRect s_updateRect;

        for (uint32_t i = 0; i < 2; i++) {
            if (this->m_textureTarget[i].m_texture == texId) {
                this->m_textureTarget[i].m_apiSpecific = nullptr;
                unbound = true;
            }
        }

        this->ITexRelease(texId);
        texId->m_needsCreation = 1;
        this->TexMarkForUpdate(texId, s_updateRect, 0);
        uint32_t texelStrideInBytes;
        const void* texels;
        texId->m_userFunc(GxTex_3, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
    }

    if (unbound) {
        this->IBindRenderTargets();
    }

    if (this->m_hwnd && this->m_vkDevice) {
        this->NotifyOnTextureRecreation();
    }
}

void CGxDeviceVulkan::TexDestroy(CGxTex* texId) {
    for (uint32_t i = 0; i < 2; i++) {
        if (this->m_textureTarget[i].m_texture == texId && this->m_textureTarget[i].m_apiSpecific) {
            this->m_textureTarget[i].m_apiSpecific = nullptr;
            this->IBindRenderTargets();
        }
    }

    this->ITexRelease(texId);
    CGxDevice::TexDestroy(texId);
}

void CGxDeviceVulkan::DeviceSetBaseMipLevel(uint32_t level) {
    this->ITexForceRecreation(1);
    CGxDevice::DeviceSetBaseMipLevel(level);
}

// TODO(V3): texture copies and read-backs (vkCmdCopyImage / vkCmdBlitImage, a readback buffer).
int32_t CGxDeviceVulkan::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    return 0;
}

int32_t CGxDeviceVulkan::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    return 0;
}

void CGxDeviceVulkan::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    this->ClampRectToWindow(rect);
    pixels.SetCount((rect.maxX - rect.minX) * (rect.maxY - rect.minY));
    memset(pixels.m_data, 0, pixels.m_count * sizeof(CImVector));
}

void CGxDeviceVulkan::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    this->ClampRectToWindow(rect);
    depths.SetCount((rect.maxX - rect.minX) * (rect.maxY - rect.minY));
    memset(depths.m_data, 0, depths.m_count * sizeof(float));
}

// TODO(V3): occlusion queries (caps report none).
bool CGxDeviceVulkan::QueryBegin(CGxQuery* query) {
    return false;
}

bool CGxDeviceVulkan::QueryEnd(CGxQuery* query) {
    return false;
}

bool CGxDeviceVulkan::QueryGetParam(CGxQuery* query, EGxQueryParam param, uint32_t& data) {
    return false;
}

bool CGxDeviceVulkan::QueryGetData(CGxQuery* query, uint32_t& data) {
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Shaders: the SPIR-V pack (shader-xlate/SPIRV.md) in Shaders\Vertex\spv and Shaders\Pixel\spv of patch-Vulkan.MPQ.
// Caps keep reporting vs_3_0 / ps_3_0 (as D3D11); only the load path moves: ShaderCreate and IShaderReload load
// through the vs_4_0 / ps_4_0 profile slot with its folder name swapped to "spv" (IShaderLoad has no fallback below
// those, so SM3 bytecode is never picked up).
namespace {
struct SpvLoadPath {
    int32_t& m_profile;
    int32_t m_savedProfile;
    const char*& m_name;
    const char* m_savedName;

    SpvLoadPath(CGxDevice* device, EGxShTarget target)
        : m_profile(device->m_caps.m_shaderTargets[target])
        , m_name(target == GxSh_Vertex ? CGxDevice::s_vertexShaderTargetNames[4] : CGxDevice::s_pixelShaderTargetNames[5]) {
        this->m_savedProfile = this->m_profile;
        this->m_savedName = this->m_name;

        if (target == GxSh_Vertex || target == GxSh_Pixel) {
            this->m_profile = this->m_profile > 0 ? (target == GxSh_Vertex ? 4 : 5) : this->m_profile;
            this->m_name = "spv";
        }
    }

    ~SpvLoadPath() {
        this->m_profile = this->m_savedProfile;
        this->m_name = this->m_savedName;
    }
};
}

static uint64_t s_shaderApiSerial;

// Parses a VKS1 body ("VKS1", nInputs, nInputs x (usage, usageIndex, location, compType), SPIR-V) into out.
bool CGxDeviceVulkan::IShaderApiCreate(const void* body, uint32_t size, ShaderApi& out) {
    const uint8_t* bytes = static_cast<const uint8_t*>(body);
    memset(&out, 0, sizeof(out));
    out.m_id = ++s_shaderApiSerial;

    if (size < 8 || memcmp(bytes, "VKS1", 4)) {
        return false;
    }

    uint32_t inputs;
    memcpy(&inputs, bytes + 4, 4);
    uint32_t header = 8 + 4 * inputs;
    uint32_t magic;

    if (inputs > 32 || size < header + 20 || (memcpy(&magic, bytes + header, 4), magic != 0x07230203) || (size - header) % 4) {
        return false;
    }

    out.m_inputCount = inputs;
    memcpy(out.m_inputs, bytes + 8, 4 * inputs);

    VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size - header;
    info.pCode = reinterpret_cast<const uint32_t*>(bytes + header);

    if (vkCreateShaderModule(this->m_vkDevice, &info, nullptr, &out.m_module) != VK_SUCCESS) {
        out.m_module = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

// Pipelines built from the module stay valid after it is destroyed; their cache entries just never match again
// (keys use m_id).
void CGxDeviceVulkan::IShaderApiRelease(ShaderApi& api) {
    if (api.m_module && this->m_vkDevice) {
        vkDestroyShaderModule(this->m_vkDevice, api.m_module, nullptr);
    }

    api.m_module = VK_NULL_HANDLE;
}

// Body is VKS1 (same 'GXSH' container). A shader whose body is empty or not VKS1 stays invalid; bound, it skips the draw.
void CGxDeviceVulkan::IShaderCreate(CGxShader* shader) {
    shader->m_valid = 0;
    static uint32_t s_shaders;

    if (++s_shaders <= 3 || s_shaders % 250 == 0) {
        VkLog("CGxDeviceVulkan: shader pack load #%u at frame %llu", s_shaders, static_cast<unsigned long long>(this->m_frameSerial));
    }

    if (!this->m_context || !this->m_vkDevice) {
        return;
    }

    shader->m_loaded = 1;

    if (shader->m_apiSpecific) {
        this->IShaderApiRelease(*static_cast<ShaderApi*>(shader->m_apiSpecific));
        delete static_cast<ShaderApi*>(shader->m_apiSpecific);
        shader->m_apiSpecific = nullptr;
    }

    if (shader->m_target != GxSh_Vertex && shader->m_target != GxSh_Pixel) {
        return;
    }

    uint32_t size = shader->m_code.Count();

    if (size < 4 || memcmp(shader->m_code.m_data, "VKS1", 4)) {
        static int32_t s_logged;

        if (size && !s_logged) {
            s_logged = 1;
            VkLog("CGxDeviceVulkan: shader without VKS1 body; is the SPIR-V pack (patch-Vulkan.MPQ, spv) installed?");
        }

        return;
    }

    ShaderApi* api = new ShaderApi;

    if (!this->IShaderApiCreate(shader->m_code.m_data, size, *api)) {
        VkLog("CGxDeviceVulkan: bad VKS1 body (%u bytes)", size);
        delete api;
        return;
    }

    shader->m_apiSpecific = api;
    shader->m_valid = 1;
}

void CGxDeviceVulkan::ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations) {
    {
        SpvLoadPath spv(this, target);
        CGxDevice::ShaderCreate(shaders, target, path, name, permutations);
    }

    if (permutations == 1 && !shaders[0]->m_loaded) {
        this->IShaderCreate(shaders[0]);
    }
}

void CGxDeviceVulkan::ShaderDestroy(CGxShader*& shader) {
    if (shader->m_refCount == 1) {
        if (shader->m_apiSpecific) {
            // ponytail: destroyed at once; a pipeline in flight keeps working (modules are only read at creation).
            this->IShaderApiRelease(*static_cast<ShaderApi*>(shader->m_apiSpecific));
            delete static_cast<ShaderApi*>(shader->m_apiSpecific);
            shader->m_apiSpecific = nullptr;
        }

        shader->m_loaded = 0;
    }

    CGxDevice::ShaderDestroy(shader);
}

void CGxDeviceVulkan::IShaderReload(CGxShader* shader, const char* path, const char* name) {
    if (shader->m_target != GxSh_Vertex && shader->m_target != GxSh_Pixel) {
        return;
    }

    if (shader->m_refCount == 1) {
        if (shader->m_apiSpecific) {
            this->IShaderApiRelease(*static_cast<ShaderApi*>(shader->m_apiSpecific));
            delete static_cast<ShaderApi*>(shader->m_apiSpecific);
            shader->m_apiSpecific = nullptr;
        }

        shader->m_loaded = 0;
    }

    {
        SpvLoadPath spv(this, static_cast<EGxShTarget>(shader->m_target));
        this->IShaderLoad(&shader, static_cast<EGxShTarget>(shader->m_target), path, name, 1);
    }

    this->IShaderCreate(shader);
}

// ---------------------------------------------------------------------------------------------------------------
// Misc

void CGxDeviceVulkan::DeviceEvictResources() {
}

void* CGxDeviceVulkan::DeviceWindow() {
    return this->m_hwnd;
}

void CGxDeviceVulkan::LogCrashInfo(char* buffer, uint32_t size) {
    char line[260];
    SStrPrintf(line, sizeof(line), "Vulkan device: %s (vendor 0x%04X device 0x%04X)\r\n", this->m_physProps.deviceName, this->m_physProps.vendorID, this->m_physProps.deviceID);
    SStrPack(buffer, line, size);
}

void CGxDeviceVulkan::DeviceOverride(EGxOverride override, uint32_t value) {
    CGxDevice::DeviceOverride(override, value);

    if (override == GxOverride_PixelShader) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = value;
    } else if (override == GxOverride_Unk7) {
        this->m_caps.m_texNonPow2 = value != 0;
    }
}

void CGxDeviceVulkan::CapsWindowSize(CRect& rect) {
    rect = this->DeviceCurWindow();
}

void CGxDeviceVulkan::CapsWindowSizeInScreenCoords(CRect& rect) {
    if (this->IDevIsWindowed()) {
        const CRect& window = this->DeviceCurWindow();
        POINT points[2];
        points[0].x = 0;
        points[0].y = 0;
        points[1].x = static_cast<LONG>(window.maxX);
        points[1].y = static_cast<LONG>(window.maxY);
        MapWindowPoints(this->m_hwnd, nullptr, points, 2);
        rect.minY = static_cast<float>(points[0].y);
        rect.minX = static_cast<float>(points[0].x);
        rect.maxY = static_cast<float>(points[1].y);
        rect.maxX = static_cast<float>(points[1].x);
    } else {
        rect = this->DeviceCurWindow();
    }
}

// No stereo on Vulkan.
void CGxDeviceVulkan::StereoSetConvergence(float convergence) {
}

float CGxDeviceVulkan::StereoGetConvergence() {
    return 0.0f;
}

void CGxDeviceVulkan::StereoSetSeparation(float separation) {
}

float CGxDeviceVulkan::StereoGetSeparation() {
    return 0.0f;
}

bool CGxDeviceVulkan::StereoEnabled() {
    return false;
}

void CGxDeviceVulkan::DeviceUnk81(uint32_t a, uint32_t b) {
}

void CGxDeviceVulkan::DeviceUnk82() {
}

void CGxDeviceVulkan::DeviceUnk83(uint32_t a, uint32_t b) {
}
