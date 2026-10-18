// vulkan-1.dll and the three dispatch levels, all resolved at runtime.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

// windows.h (pulled in by Vk.hpp, which needs it for HWND) defines CreateSemaphore as a macro for
// CreateSemaphoreA/W, and loader::Device below has a field of that name. Undefining it here is the
// honest fix rather than renaming the field: the field name is what the resolver macro in Loader.cpp
// turns into the "vkCreateSemaphore" string, so a field called anything else would have to be resolved
// by a hand-written line whose string could drift from it. Nothing in the Gx tree calls the Win32
// CreateSemaphore, and the W/A entry points themselves are untouched and still reachable by their
// real names.
#undef CreateSemaphore

// Three levels, resolved in order, because that is the layering the Vulkan loader defines and not a
// style choice:
//
//   global   -- vkGetInstanceProcAddr(NULL, name). The only four functions that exist before an
//               instance does. Asking for anything else here legally returns null.
//   instance -- vkGetInstanceProcAddr(instance, name). Dispatches through the loader's trampoline,
//               which is what makes a call work across several physical devices.
//   device   -- vkGetDeviceProcAddr(device, name). The pointer is the driver's own entry point for
//               THIS device, with no loader trampoline and no dispatch-table lookup in front of it.
//               For anything called per frame that is the correct layer and the faster one; it is
//               also the only layer at which a device-level function is guaranteed to be non-null
//               for a device whose extension list contains it.
//
// vulkan-1.dll itself is reached with LoadLibraryA, never with an import, for two independent reasons:
//
//  - it is a system component the machine may not have. An import would mean the client fails to
//    start at all on a machine with no Vulkan driver, even with another gxApi selected. A
//    LoadLibraryA that returns null is a log line and a failed DeviceCreate, which the engine turns
//    into its usual "unable to start up 3D acceleration" path.
//  - the version on the machine is not knowable at build time. The loader is forward-compatible but
//    not backward-compatible: a 1.0-era vulkan-1.dll has no vkEnumerateInstanceVersion, and that
//    absence has to be a readable diagnostic rather than an unresolved import.
//
// The x64 loader is C:\Windows\System32\vulkan-1.dll (SysWOW64 holds the 32-bit one, which a Win32
// build of this tree gets through the file-system redirector). The name passed to LoadLibraryA is the
// bare "vulkan-1.dll" on purpose: the system directory is on the default search path for a module
// name, and hard-coding the path would be wrong under a different Windows layout.
namespace gxvk::vk::loader
{
    /// The four entry points that exist before an instance does.
    struct Global
    {
        PFN_vkGetInstanceProcAddr                  GetInstanceProcAddr                  = nullptr;
        PFN_vkCreateInstance                       CreateInstance                       = nullptr;
        PFN_vkEnumerateInstanceLayerProperties     EnumerateInstanceLayerProperties     = nullptr;
        PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
        /// Vulkan 1.1 and later only; null on a 1.0 loader, which is not an error.
        PFN_vkEnumerateInstanceVersion             EnumerateInstanceVersion             = nullptr;
    };

    /// Everything the bring-up calls on an instance or a physical device.
    struct Instance
    {
        PFN_vkDestroyInstance                            DestroyInstance                            = nullptr;
        PFN_vkEnumeratePhysicalDevices                   EnumeratePhysicalDevices                   = nullptr;
        PFN_vkGetPhysicalDeviceProperties                GetPhysicalDeviceProperties                = nullptr;
        /// Which optional features the adapter has, so vkCreateDevice can be asked for the ones it
        /// supports and only those: enabling an unsupported feature fails the create outright.
        PFN_vkGetPhysicalDeviceFeatures                  GetPhysicalDeviceFeatures                  = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties          GetPhysicalDeviceMemoryProperties          = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties     GetPhysicalDeviceQueueFamilyProperties     = nullptr;
        PFN_vkEnumerateDeviceExtensionProperties         EnumerateDeviceExtensionProperties         = nullptr;
        PFN_vkCreateDevice                               CreateDevice                               = nullptr;
        PFN_vkGetDeviceProcAddr                          GetDeviceProcAddr                          = nullptr;

        PFN_vkCreateWin32SurfaceKHR                      CreateWin32SurfaceKHR                      = nullptr;
        PFN_vkDestroySurfaceKHR                          DestroySurfaceKHR                          = nullptr;
        /// Which operations a VkFormat supports on this adapter: the caps (render-target formats) and,
        /// later, the texture blits ask it before they rely on an optional format feature.
        PFN_vkGetPhysicalDeviceFormatProperties         GetPhysicalDeviceFormatProperties          = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceSupportKHR         GetPhysicalDeviceSurfaceSupportKHR         = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR    GetPhysicalDeviceSurfaceCapabilitiesKHR    = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceFormatsKHR         GetPhysicalDeviceSurfaceFormatsKHR         = nullptr;
        PFN_vkGetPhysicalDeviceSurfacePresentModesKHR    GetPhysicalDeviceSurfacePresentModesKHR    = nullptr;

        /// Both null unless VK_EXT_debug_utils was enabled; absence is not an error.
        PFN_vkCreateDebugUtilsMessengerEXT               CreateDebugUtilsMessengerEXT               = nullptr;
        PFN_vkDestroyDebugUtilsMessengerEXT              DestroyDebugUtilsMessengerEXT              = nullptr;
    };

    /// Everything the bring-up and the per-frame loop call on the logical device, through
    /// vkGetDeviceProcAddr.
    ///
    /// Every field here is required: LoadDevice() logs and declines on a miss. That is the right
    /// policy for this table because all of it is Vulkan 1.0 core plus VK_KHR_swapchain, and a
    /// device that was created with the swapchain extension enabled cannot legally be short of any
    /// of them. The alternative -- marking the per-frame group optional and discovering a null
    /// pointer at frame 1 -- would turn a loader problem into a crash sixty times a second.
    ///
    /// The table holds every device entry point the backend calls or will call (memory, textures,
    /// shaders, pipelines, queries, compute), all resolved at bring-up: every entry is 1.0 core, and
    /// a loader that lacks one is found out at the first bring-up rather than at the first draw.
    struct Device
    {
        PFN_vkDestroyDevice        DestroyDevice        = nullptr;
        PFN_vkGetDeviceQueue       GetDeviceQueue       = nullptr;
        PFN_vkDeviceWaitIdle       DeviceWaitIdle       = nullptr;
        PFN_vkCreateSwapchainKHR   CreateSwapchainKHR   = nullptr;
        PFN_vkDestroySwapchainKHR  DestroySwapchainKHR  = nullptr;
        PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;

        // --- Frame/: the objects derived from a swapchain ---------------------------------------
        PFN_vkCreateImageView      CreateImageView      = nullptr;
        PFN_vkDestroyImageView     DestroyImageView     = nullptr;
        PFN_vkCreateRenderPass     CreateRenderPass     = nullptr;
        PFN_vkCreateRenderPass2    CreateRenderPass2    = nullptr;   ///< optional (1.2 core): the MSAA scene's depth resolve
        PFN_vkDestroyRenderPass    DestroyRenderPass    = nullptr;
        PFN_vkCreateFramebuffer    CreateFramebuffer    = nullptr;
        PFN_vkDestroyFramebuffer   DestroyFramebuffer   = nullptr;

        // --- Frame/: command recording ----------------------------------------------------------
        //
        // Two entry points a reader might expect are deliberately absent. vkResetCommandBuffer:
        // the pool is created with VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, which makes
        // vkBeginCommandBuffer on a buffer in the executable state an implicit reset, so the
        // explicit call would be one more required entry point for no behavioural difference.
        // vkFreeCommandBuffers: vkDestroyCommandPool frees every buffer allocated from the pool,
        // and nothing here outlives its pool.
        PFN_vkCreateCommandPool      CreateCommandPool      = nullptr;
        PFN_vkDestroyCommandPool     DestroyCommandPool     = nullptr;
        PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
        PFN_vkBeginCommandBuffer     BeginCommandBuffer     = nullptr;
        PFN_vkEndCommandBuffer       EndCommandBuffer       = nullptr;
        PFN_vkCmdBeginRenderPass     CmdBeginRenderPass     = nullptr;
        PFN_vkCmdEndRenderPass       CmdEndRenderPass       = nullptr;

        // --- Frame/: synchronisation ------------------------------------------------------------
        //
        // Binary semaphores and fences only: no vkGetSemaphoreCounterValue, no vkWaitSemaphores.
        // Timeline semaphores are Vulkan 1.2, and the vendored headers are allowed to be newer than
        // the vulkan-1.dll on the machine.
        PFN_vkCreateSemaphore      CreateSemaphore      = nullptr;
        PFN_vkDestroySemaphore     DestroySemaphore     = nullptr;
        PFN_vkCreateFence          CreateFence          = nullptr;
        PFN_vkDestroyFence         DestroyFence         = nullptr;
        PFN_vkWaitForFences        WaitForFences        = nullptr;
        PFN_vkResetFences          ResetFences          = nullptr;
        /// Polls a frame slot's fence without blocking, so a retirement can run as soon as the GPU is
        /// done rather than at the next frame's open.
        PFN_vkGetFenceStatus       GetFenceStatus       = nullptr;

        // --- Frame/: the queue operations -------------------------------------------------------
        PFN_vkQueueSubmit          QueueSubmit          = nullptr;
        PFN_vkAcquireNextImageKHR  AcquireNextImageKHR  = nullptr;
        PFN_vkQueuePresentKHR      QueuePresentKHR      = nullptr;

        // --- Memory/: device memory --------------------------------------------------------------
        //
        // vkAllocateMemory is a scarce call and not merely an expensive one: the spec's guaranteed
        // floor for maxMemoryAllocationCount is 4096, and several desktop drivers report exactly
        // that. So the allocator sub-allocates out of large blocks rather than allocating per
        // resource, and that is a property of the design and not an optimisation to add later.
        PFN_vkAllocateMemory            AllocateMemory            = nullptr;
        PFN_vkFreeMemory                FreeMemory                = nullptr;
        PFN_vkMapMemory                 MapMemory                 = nullptr;
        PFN_vkUnmapMemory               UnmapMemory               = nullptr;
        /// Needed only for a memory type without HOST_COHERENT. Resolved unconditionally because a
        /// HOST_VISIBLE type that is not also HOST_COHERENT is normal on AMD and discovering that
        /// mid-upload is too late to go looking for an entry point.
        PFN_vkFlushMappedMemoryRanges   FlushMappedMemoryRanges   = nullptr;

        // --- Memory/: buffers --------------------------------------------------------------------
        PFN_vkCreateBuffer              CreateBuffer              = nullptr;
        PFN_vkDestroyBuffer             DestroyBuffer             = nullptr;
        PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
        PFN_vkBindBufferMemory          BindBufferMemory          = nullptr;

        // --- images ------------------------------------------------------------------------------
        PFN_vkCreateImage               CreateImage               = nullptr;
        PFN_vkDestroyImage              DestroyImage              = nullptr;
        PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
        PFN_vkBindImageMemory           BindImageMemory           = nullptr;

        // --- the transfer commands ---------------------------------------------------------------
        PFN_vkCmdPipelineBarrier        CmdPipelineBarrier        = nullptr;
        PFN_vkCmdCopyBuffer             CmdCopyBuffer             = nullptr;
        PFN_vkCmdCopyBufferToImage      CmdCopyBufferToImage      = nullptr;
        PFN_vkCmdClearColorImage        CmdClearColorImage        = nullptr;
        PFN_vkCmdBlitImage              CmdBlitImage              = nullptr;

        // --- Pipelines.cpp: SPIR-V modules -------------------------------------------------------
        PFN_vkCreateShaderModule        CreateShaderModule        = nullptr;
        PFN_vkDestroyShaderModule       DestroyShaderModule       = nullptr;

        // --- Pipelines.cpp: the objects a draw binds ---------------------------------------------
        //
        // All Vulkan 1.0 core, on purpose: no vkCmdPushDescriptorSetKHR (push descriptors are an
        // extension), no vkCmdBeginRendering (dynamic rendering is 1.3). The draw path goes through
        // a render pass, a descriptor pool and a pipeline cache keyed on the pass, which is the
        // shape that runs on every loader this backend can meet.
        PFN_vkCreateDescriptorSetLayout  CreateDescriptorSetLayout  = nullptr;
        PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
        PFN_vkCreatePipelineLayout       CreatePipelineLayout       = nullptr;
        PFN_vkDestroyPipelineLayout      DestroyPipelineLayout      = nullptr;
        PFN_vkCreateGraphicsPipelines    CreateGraphicsPipelines    = nullptr;
        PFN_vkDestroyPipeline            DestroyPipeline            = nullptr;
        PFN_vkCreateDescriptorPool       CreateDescriptorPool       = nullptr;
        PFN_vkDestroyDescriptorPool      DestroyDescriptorPool      = nullptr;
        PFN_vkResetDescriptorPool        ResetDescriptorPool        = nullptr;
        PFN_vkAllocateDescriptorSets     AllocateDescriptorSets     = nullptr;
        PFN_vkUpdateDescriptorSets       UpdateDescriptorSets       = nullptr;
        PFN_vkCreateSampler              CreateSampler              = nullptr;
        PFN_vkDestroySampler             DestroySampler             = nullptr;

        // --- Pipelines.cpp: the per-draw commands ------------------------------------------------
        PFN_vkCmdBindPipeline            CmdBindPipeline            = nullptr;
        PFN_vkCmdBindVertexBuffers       CmdBindVertexBuffers       = nullptr;
        PFN_vkCmdBindIndexBuffer         CmdBindIndexBuffer         = nullptr;
        PFN_vkCmdBindDescriptorSets      CmdBindDescriptorSets      = nullptr;
        PFN_vkCmdPushConstants           CmdPushConstants           = nullptr;
        PFN_vkCmdSetViewport             CmdSetViewport             = nullptr;
        PFN_vkCmdSetScissor              CmdSetScissor              = nullptr;
        PFN_vkCmdDrawIndexed             CmdDrawIndexed             = nullptr;
        PFN_vkCmdDraw                    CmdDraw                    = nullptr;
        /// The two dynamic states some pipelines add (GxRs_PolygonOffset, the blend factor of
        /// GxBlend_ConstantAlpha); 1.0 core.
        PFN_vkCmdSetDepthBias            CmdSetDepthBias            = nullptr;
        PFN_vkCmdSetBlendConstants       CmdSetBlendConstants       = nullptr;
        PFN_vkCmdSetStencilReference     CmdSetStencilReference     = nullptr;

        // --- the engine's SceneClear inside the frame's render pass ------------------------------
        //
        // D3D9's Clear clears the viewport (and scissor) rectangle of the current target in place,
        // between draws; vkCmdClearAttachments is the 1.0 core command that does exactly that inside
        // a render pass.
        PFN_vkCmdClearAttachments        CmdClearAttachments        = nullptr;

        // --- render targets, copies, read-back, queries ------------------------------------------
        PFN_vkCmdCopyImage               CmdCopyImage               = nullptr;
        PFN_vkCmdCopyImageToBuffer       CmdCopyImageToBuffer       = nullptr;
        PFN_vkCreateQueryPool            CreateQueryPool            = nullptr;
        PFN_vkDestroyQueryPool           DestroyQueryPool           = nullptr;
        PFN_vkCmdResetQueryPool          CmdResetQueryPool          = nullptr;
        PFN_vkCmdBeginQuery              CmdBeginQuery              = nullptr;
        PFN_vkCmdEndQuery                CmdEndQuery                = nullptr;
        PFN_vkGetQueryPoolResults        GetQueryPoolResults        = nullptr;

        // --- Pipelines.cpp: the pipeline cache ---------------------------------------------------
        PFN_vkCreatePipelineCache        CreatePipelineCache        = nullptr;
        PFN_vkDestroyPipelineCache       DestroyPipelineCache       = nullptr;
        PFN_vkGetPipelineCacheData       GetPipelineCacheData       = nullptr;

        // --- compute, indirect dispatch, buffer fills, timestamps --------------------------------
        PFN_vkCreateComputePipelines     CreateComputePipelines     = nullptr;
        PFN_vkCmdDispatch                CmdDispatch                = nullptr;
        PFN_vkCmdDispatchIndirect        CmdDispatchIndirect        = nullptr;
        PFN_vkCmdFillBuffer              CmdFillBuffer              = nullptr;
        PFN_vkCmdWriteTimestamp          CmdWriteTimestamp          = nullptr;
    };

    /**
     * Loads vulkan-1.dll and resolves the global level. Idempotent.
     *
     * @return bool ok : false when the DLL is absent, carries no vkGetInstanceProcAddr, or is too
     *         old to supply one of the three remaining global entry points -- each of which is
     *         logged by name rather than collapsed into one failure
     */
    bool Load();

    /**
     * The global level. Every field is null before a successful Load().
     *
     * @return Global api
     */
    const Global& Api();

    /**
     * The instance-level API version the loader itself supports.
     *
     * vkEnumerateInstanceVersion is a 1.1 function, so its absence is exactly the signal that the
     * loader is 1.0 -- which the spec says to read as VK_API_VERSION_1_0 rather than as a failure.
     *
     * @return uint32 version : packed as VK_MAKE_API_VERSION
     */
    uint32_t InstanceVersion();

    /**
     * Whether the entry points resolved so far still point into the loaded loader image.
     *
     * The reason this exists is process exit. A Vulkan call from a static destructor may run after
     * the loader and the ICD -- DLLs too, with no defined detach order relative to the executable's
     * statics -- have been unmapped. Calling into an unmapped image would be a crash on exit, so a
     * destructor that reaches Vulkan asks first.
     *
     * The check is cheap and real rather than a guess: it asks the OS for the module handle by name
     * and confirms that the resolved vkGetInstanceProcAddr still lies inside that module's mapped
     * image. It cannot prove the ICD's DllMain has not run, so it is a necessary condition and not a
     * sufficient one.
     *
     * @return bool mapped : false when the loader is gone, or was never loaded
     */
    bool StillMapped();

    /**
     * Resolves the instance level. Logs every function it could not get, by name.
     *
     * @param VkInstance instance : a created instance
     * @param bool debugUtils : whether VK_EXT_debug_utils was enabled, so the two messenger entry
     *        points are required rather than optional
     * @param Instance& out : receives the table; untouched fields stay null
     * @return bool ok : false when a required function was missing
     */
    bool LoadInstance(VkInstance instance, bool debugUtils, Instance& out);

    /**
     * Resolves the device level through vkGetDeviceProcAddr. Logs every miss by name.
     *
     * @param Instance& api : a loaded instance table, for GetDeviceProcAddr
     * @param VkDevice device : a created logical device
     * @param Device& out : receives the table
     * @return bool ok : false when a required function was missing
     */
    bool LoadDevice(const Instance& api, VkDevice device, Device& out);
}
