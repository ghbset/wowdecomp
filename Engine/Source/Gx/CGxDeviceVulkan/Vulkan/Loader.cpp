// vulkan-1.dll and the three dispatch levels, all resolved at runtime.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"

namespace
{
    using gxvk::log::Write;
    using namespace gxvk::log;

    /// The system loader's file name. Bare, so the module search finds the System32 one (or SysWOW64
    /// through the redirector on a Win32 build).
    constexpr const char* kLoaderDll = "vulkan-1.dll";

    HMODULE                  g_module = nullptr;
    gxvk::vk::loader::Global g_global;
    uint32_t                 g_instanceVersion = 0;

    /**
     * One global-level entry point, by name.
     *
     * vkGetInstanceProcAddr with a null instance is the only legal way to ask for these four, and
     * the spec is explicit that asking for anything else that way returns null -- which is why this
     * helper is not reused for the instance level.
     *
     * @param string name : the function name
     * @return pointer fn : null when this loader does not have it
     */
    PFN_vkVoidFunction GlobalProc(const char* name)
    {
        return g_global.GetInstanceProcAddr ? g_global.GetInstanceProcAddr(nullptr, name) : nullptr;
    }
}

namespace gxvk::vk::loader
{
    bool Load()
    {
        if (g_module) return g_global.CreateInstance != nullptr;

        g_module = ::LoadLibraryA(kLoaderDll);
        if (!g_module)
        {
            // Declining, not failing. A machine with no Vulkan driver has no vulkan-1.dll at all,
            // and that is the single most likely reason to be here -- so the line names the file and
            // the error and says what the consequence is, rather than reading as a defect.
            Write(Warn, "vulkan: %s not present (LoadLibrary error %lu); the Vulkan device cannot be created",
                  kLoaderDll, ::GetLastError());
            return false;
        }

        // The one symbol taken with GetProcAddress. Everything else comes through it, because the
        // loader exports only a 1.0-era subset by name and resolving e.g. vkCreateInstance with
        // GetProcAddress would silently bypass the layer chain.
        g_global.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            ::GetProcAddress(g_module, "vkGetInstanceProcAddr"));
        if (!g_global.GetInstanceProcAddr)
        {
            Write(Warn, "vulkan: %s has no vkGetInstanceProcAddr; it is not a Vulkan loader. Declining.", kLoaderDll);
            ::FreeLibrary(g_module);
            g_module = nullptr;
            return false;
        }

        g_global.CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(GlobalProc("vkCreateInstance"));
        g_global.EnumerateInstanceLayerProperties = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
            GlobalProc("vkEnumerateInstanceLayerProperties"));
        g_global.EnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            GlobalProc("vkEnumerateInstanceExtensionProperties"));

        // 1.1 and later. Null here means a 1.0 loader, which the spec says to read as
        // VK_API_VERSION_1_0 -- not as an error, and not as a reason to decline.
        g_global.EnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            GlobalProc("vkEnumerateInstanceVersion"));

        bool ok = true;
        if (!g_global.CreateInstance)
        {
            Write(Warn, "vulkan: global entry point missing: %s", "vkCreateInstance");
            ok = false;
        }
        if (!g_global.EnumerateInstanceExtensionProperties)
        {
            Write(Warn, "vulkan: global entry point missing: %s", "vkEnumerateInstanceExtensionProperties");
            ok = false;
        }
        // The layer enumeration is deliberately NOT required: without it, validation simply cannot
        // be offered, and that is a developer feature.
        if (!g_global.EnumerateInstanceLayerProperties)
            Write(Debug, "vulkan: this loader has no vkEnumerateInstanceLayerProperties; validation layers cannot be requested");

        if (!ok)
        {
            Write(Warn, "vulkan: %s is too old; declining", kLoaderDll);
            ::FreeLibrary(g_module);
            g_module = nullptr;
            g_global = Global();
            return false;
        }

        g_instanceVersion = VK_API_VERSION_1_0;
        if (g_global.EnumerateInstanceVersion)
        {
            uint32_t version = 0;
            // The only documented failure is VK_ERROR_OUT_OF_HOST_MEMORY; on anything other than
            // success the 1.0 assumption above stands, which is the conservative direction.
            if (g_global.EnumerateInstanceVersion(&version) == VK_SUCCESS) g_instanceVersion = version;
        }

        char text[32];
        Write(Debug, "vulkan: loaded %s at %p, instance API %s%s", kLoaderDll, static_cast<void*>(g_module),
              ApiVersionText(g_instanceVersion, text, sizeof text),
              g_global.EnumerateInstanceVersion ? "" : " (assumed: no vkEnumerateInstanceVersion)");
        return true;
    }

    const Global& Api() { return g_global; }

    uint32_t InstanceVersion() { return g_instanceVersion; }

    bool StillMapped()
    {
        if (!g_module || !g_global.GetInstanceProcAddr) return false;

        // GetModuleHandleA does not take a reference, which is what makes it the right call here:
        // this runs on the exit path and must not change anybody's refcount.
        const HMODULE live = ::GetModuleHandleA(kLoaderDll);
        if (live != g_module) return false;

        // And the handle alone is not enough: a module handle is its base address, so a stale handle
        // could compare equal to a different mapping at the same base. Confirm the entry point we
        // actually hold still lies inside the image the headers describe.
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const char*>(g_module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        const uintptr_t base = reinterpret_cast<uintptr_t>(g_module);
        const uintptr_t end  = base + nt->OptionalHeader.SizeOfImage;
        const uintptr_t proc = reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(g_global.GetInstanceProcAddr));
        return proc >= base && proc < end;
    }

    bool LoadInstance(VkInstance instance, bool debugUtils, Instance& out)
    {
        if (!instance || !g_global.GetInstanceProcAddr) return false;

        unsigned missing = 0;

        // A macro rather than twenty near-identical lines, and it does the one thing a hand-written
        // line gets wrong: it derives the string from the field name, so the name in the log and the
        // name passed to the loader cannot drift apart.
#define GXVK_INSTANCE_PROC(field)                                                                 \
        do {                                                                                      \
            out.field = reinterpret_cast<PFN_vk##field>(                                          \
                g_global.GetInstanceProcAddr(instance, "vk" #field));                             \
            if (!out.field)                                                                       \
            {                                                                                     \
                Write(Warn, "vulkan: instance entry point missing: vk%s", #field);               \
                ++missing;                                                                        \
            }                                                                                     \
        } while (false)

        GXVK_INSTANCE_PROC(DestroyInstance);
        GXVK_INSTANCE_PROC(EnumeratePhysicalDevices);
        GXVK_INSTANCE_PROC(GetPhysicalDeviceProperties);
        GXVK_INSTANCE_PROC(GetPhysicalDeviceFeatures);
        GXVK_INSTANCE_PROC(GetPhysicalDeviceMemoryProperties);
        GXVK_INSTANCE_PROC(GetPhysicalDeviceQueueFamilyProperties);
        GXVK_INSTANCE_PROC(EnumerateDeviceExtensionProperties);
        GXVK_INSTANCE_PROC(CreateDevice);
        GXVK_INSTANCE_PROC(GetDeviceProcAddr);

        // VK_KHR_surface / VK_KHR_win32_surface. These are only non-null because the instance was
        // created with those extensions enabled -- which is the whole reason the extension list is
        // checked against vkEnumerateInstanceExtensionProperties before vkCreateInstance rather than
        // being discovered by a null pointer here.
        GXVK_INSTANCE_PROC(CreateWin32SurfaceKHR);
        GXVK_INSTANCE_PROC(DestroySurfaceKHR);
        GXVK_INSTANCE_PROC(GetPhysicalDeviceFormatProperties);
        GXVK_INSTANCE_PROC(GetPhysicalDeviceSurfaceSupportKHR);
        GXVK_INSTANCE_PROC(GetPhysicalDeviceSurfaceCapabilitiesKHR);
        GXVK_INSTANCE_PROC(GetPhysicalDeviceSurfaceFormatsKHR);
        GXVK_INSTANCE_PROC(GetPhysicalDeviceSurfacePresentModesKHR);

        if (debugUtils)
        {
            GXVK_INSTANCE_PROC(CreateDebugUtilsMessengerEXT);
            GXVK_INSTANCE_PROC(DestroyDebugUtilsMessengerEXT);
        }

#undef GXVK_INSTANCE_PROC

        if (missing)
        {
            Write(Warn, "vulkan: %u instance entry point(s) missing; declining", missing);
            return false;
        }
        return true;
    }

    bool LoadDevice(const Instance& api, VkDevice device, Device& out)
    {
        if (!device || !api.GetDeviceProcAddr) return false;

        unsigned missing = 0;

        // vkGetDeviceProcAddr, not vkGetInstanceProcAddr. Both would return something callable, but
        // the instance-level pointer goes through the loader's dispatch trampoline on every call,
        // and the device-level one is the driver's own entry for this device -- the faster one for
        // everything the frame calls thousands of times.
#define GXVK_DEVICE_PROC(field)                                                                   \
        do {                                                                                      \
            out.field = reinterpret_cast<PFN_vk##field>(                                          \
                api.GetDeviceProcAddr(device, "vk" #field));                                      \
            if (!out.field)                                                                       \
            {                                                                                     \
                Write(Warn, "vulkan: device entry point missing: vk%s", #field);                 \
                ++missing;                                                                        \
            }                                                                                     \
        } while (false)

        GXVK_DEVICE_PROC(DestroyDevice);
        GXVK_DEVICE_PROC(GetDeviceQueue);
        GXVK_DEVICE_PROC(DeviceWaitIdle);
        GXVK_DEVICE_PROC(CreateSwapchainKHR);
        GXVK_DEVICE_PROC(DestroySwapchainKHR);
        GXVK_DEVICE_PROC(GetSwapchainImagesKHR);

        // Everything below is what the frame, the memory, the pipelines and the rest of the backend
        // call. It is resolved here, with the rest, rather than lazily on the first frame: a missing
        // entry point has to be a decline at bring-up time with a name in the log, not a null call
        // inside the frame.
        GXVK_DEVICE_PROC(CreateImageView);
        GXVK_DEVICE_PROC(DestroyImageView);
        GXVK_DEVICE_PROC(CreateRenderPass);
        GXVK_DEVICE_PROC(DestroyRenderPass);
        GXVK_DEVICE_PROC(CreateFramebuffer);
        GXVK_DEVICE_PROC(DestroyFramebuffer);

        GXVK_DEVICE_PROC(CreateCommandPool);
        GXVK_DEVICE_PROC(DestroyCommandPool);
        GXVK_DEVICE_PROC(AllocateCommandBuffers);
        GXVK_DEVICE_PROC(BeginCommandBuffer);
        GXVK_DEVICE_PROC(EndCommandBuffer);
        GXVK_DEVICE_PROC(CmdBeginRenderPass);
        GXVK_DEVICE_PROC(CmdEndRenderPass);

        // CreateSemaphore is the one field whose name collides with a windows.h macro; Loader.hpp
        // undefines it, which is why the line below reads normally and the string the macro builds
        // is "vkCreateSemaphore" and not "vkCreateSemaphoreW".
        GXVK_DEVICE_PROC(CreateSemaphore);
        GXVK_DEVICE_PROC(DestroySemaphore);
        GXVK_DEVICE_PROC(CreateFence);
        GXVK_DEVICE_PROC(DestroyFence);
        GXVK_DEVICE_PROC(WaitForFences);
        GXVK_DEVICE_PROC(ResetFences);
        GXVK_DEVICE_PROC(GetFenceStatus);

        GXVK_DEVICE_PROC(AllocateMemory);
        GXVK_DEVICE_PROC(FreeMemory);
        GXVK_DEVICE_PROC(MapMemory);
        GXVK_DEVICE_PROC(UnmapMemory);
        GXVK_DEVICE_PROC(FlushMappedMemoryRanges);

        GXVK_DEVICE_PROC(CreateBuffer);
        GXVK_DEVICE_PROC(DestroyBuffer);
        GXVK_DEVICE_PROC(GetBufferMemoryRequirements);
        GXVK_DEVICE_PROC(BindBufferMemory);

        GXVK_DEVICE_PROC(CreateImage);
        GXVK_DEVICE_PROC(DestroyImage);
        GXVK_DEVICE_PROC(GetImageMemoryRequirements);
        GXVK_DEVICE_PROC(BindImageMemory);

        GXVK_DEVICE_PROC(CmdPipelineBarrier);
        GXVK_DEVICE_PROC(CmdCopyBuffer);
        GXVK_DEVICE_PROC(CmdCopyBufferToImage);
        GXVK_DEVICE_PROC(CmdClearColorImage);
        GXVK_DEVICE_PROC(CmdBlitImage);
        GXVK_DEVICE_PROC(CmdClearAttachments);

        GXVK_DEVICE_PROC(CreateShaderModule);
        GXVK_DEVICE_PROC(DestroyShaderModule);

        GXVK_DEVICE_PROC(CreateDescriptorSetLayout);
        GXVK_DEVICE_PROC(DestroyDescriptorSetLayout);
        GXVK_DEVICE_PROC(CreatePipelineLayout);
        GXVK_DEVICE_PROC(DestroyPipelineLayout);
        GXVK_DEVICE_PROC(CreateGraphicsPipelines);
        GXVK_DEVICE_PROC(DestroyPipeline);
        GXVK_DEVICE_PROC(CreateDescriptorPool);
        GXVK_DEVICE_PROC(DestroyDescriptorPool);
        GXVK_DEVICE_PROC(ResetDescriptorPool);
        GXVK_DEVICE_PROC(AllocateDescriptorSets);
        GXVK_DEVICE_PROC(UpdateDescriptorSets);
        GXVK_DEVICE_PROC(CreateSampler);
        GXVK_DEVICE_PROC(DestroySampler);

        GXVK_DEVICE_PROC(CmdBindPipeline);
        GXVK_DEVICE_PROC(CmdBindVertexBuffers);
        GXVK_DEVICE_PROC(CmdBindIndexBuffer);
        GXVK_DEVICE_PROC(CmdBindDescriptorSets);
        GXVK_DEVICE_PROC(CmdPushConstants);
        GXVK_DEVICE_PROC(CmdSetViewport);
        GXVK_DEVICE_PROC(CmdSetScissor);
        GXVK_DEVICE_PROC(CmdDrawIndexed);
        GXVK_DEVICE_PROC(CmdDraw);
        GXVK_DEVICE_PROC(CmdSetDepthBias);
        GXVK_DEVICE_PROC(CmdSetBlendConstants);

        GXVK_DEVICE_PROC(CmdCopyImage);
        GXVK_DEVICE_PROC(CmdCopyImageToBuffer);
        GXVK_DEVICE_PROC(CreateQueryPool);
        GXVK_DEVICE_PROC(DestroyQueryPool);
        GXVK_DEVICE_PROC(CmdResetQueryPool);
        GXVK_DEVICE_PROC(CmdBeginQuery);
        GXVK_DEVICE_PROC(CmdEndQuery);
        GXVK_DEVICE_PROC(GetQueryPoolResults);

        GXVK_DEVICE_PROC(CreatePipelineCache);
        GXVK_DEVICE_PROC(DestroyPipelineCache);
        GXVK_DEVICE_PROC(GetPipelineCacheData);

        GXVK_DEVICE_PROC(CreateComputePipelines);
        GXVK_DEVICE_PROC(CmdDispatch);
        GXVK_DEVICE_PROC(CmdDispatchIndirect);
        GXVK_DEVICE_PROC(CmdFillBuffer);
        GXVK_DEVICE_PROC(CmdWriteTimestamp);

        GXVK_DEVICE_PROC(QueueSubmit);
        GXVK_DEVICE_PROC(AcquireNextImageKHR);
        GXVK_DEVICE_PROC(QueuePresentKHR);

#undef GXVK_DEVICE_PROC

        if (missing)
        {
            Write(Warn, "vulkan: %u device entry point(s) missing; declining", missing);
            return false;
        }
        return true;
    }
}
