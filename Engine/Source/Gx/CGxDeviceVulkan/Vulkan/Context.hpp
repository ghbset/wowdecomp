// The Vulkan objects the backend owns: instance, physical device, logical device, queues, Win32
// surface and swapchain. Created at runtime, destroyed in reverse order. CGxDeviceVulkan hands its
// own HWND to Bring() and Rebind().
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/DeviceCaps.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

// CGxDeviceVulkan brings these up synchronously in DeviceSetFormat, on the window it has just
// created, and keeps the instance and the VkDevice for the life of the process: a format change or a
// graphics restart only rebuilds the surface and the swapchain on the new window.
namespace gxvk::vk::context
{
    /// What one bring-up attempt concluded.
    enum class Attempt
    {
        Done,     ///< everything that was asked for exists; stop calling Bring()
        Retry,    ///< the window is not usable yet (zero-size); call again next frame
        Declined, ///< a permanent reason -- no loader, no suitable device, a create failed
    };

    /**
     * One bring-up attempt on the device's window. Idempotent once it has returned Done or Declined;
     * on a Declined everything it created has been torn down.
     *
     * Must run on the thread that owns the window, the render thread. Win32 surface creation is not
     * documented to require it, but every other window operation in this client does.
     *
     * @param HWND hwnd : the device's window
     * @return Attempt attempt
     */
    Attempt Bring(HWND hwnd);

    /**
     * Rebuilds the swapchain on the window the device holds now. Two cases, distinguished by
     * comparing the HWND with the surface's:
     *
     *  - same window, new size: the surface is still valid, the swapchain's extent is not. The
     *    swapchain alone is rebuilt.
     *  - different window (a format change or a graphics restart made a new one): surface and
     *    swapchain both go and are rebuilt against the new HWND. The instance and logical device
     *    survive, which is the point of keeping them separate from the surface.
     *
     * Does nothing when the bring-up never completed.
     *
     * @param HWND hwnd : the device's window
     */
    void Rebind(HWND hwnd);

    /**
     * Destroys the swapchain and the surface, and leaves the instance and the device standing.
     *
     * DeviceSetFormat calls it right before the window is destroyed: a VkSurfaceKHR must not
     * outlive its window, and the device is kept so the following Rebind() builds a surface on the
     * new window without a second vkCreateDevice. Safe when nothing stands.
     */
    void ReleaseWindow();

    /**
     * The vsync of the engine's CGxFormat. Read by every swapchain built afterwards: on, FIFO; off,
     * IMMEDIATE when the surface offers it, else MAILBOX, else FIFO.
     *
     * @param bool on : the format's vsync field is non-zero
     */
    void SetVsync(bool on);

    /**
     * Whether the next Bring() asks for VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils. Off by
     * default; the device turns it on from the GX_VULKAN_VALIDATION environment variable. A layer
     * that is not installed is a log line, not a failure.
     *
     * @param bool on
     */
    void SetValidation(bool on);

    /**
     * Destroys everything, in exact reverse order of creation: swapchain, logical device (after
     * vkDeviceWaitIdle), surface, debug messenger, instance. The unwinder of a half-finished Bring().
     *
     * vulkan-1.dll is deliberately NOT unloaded: see the comment at the call site.
     */
    void Teardown();

    /**
     * With the validation layer on and anything reported: one line with the error and warning
     * totals, then one per message id whose occurrences past the first five were counted rather
     * than logged, with its total. Nothing when the layer reported nothing.
     */
    void LogValidationSummary();

    /**
     * Whether the instance and logical device exist.
     *
     * @return bool live
     */
    bool Live();

    /**
     * Everything a renderer needs from the bring-up, in one read-only view.
     *
     * A struct handed out by pointer rather than eleven accessors, because every caller needs the
     * whole set at once and a per-field accessor would let a caller read half its state from one
     * swapchain generation and half from the next.
     *
     * Lifetime: the instance- and device-level fields are valid for as long as Live(). The
     * swapchain-derived fields -- swapchain, format, extent, imageCount -- are valid only while
     * SwapchainGeneration() is unchanged, and `swapchain` is VK_NULL_HANDLE whenever there is no
     * swapchain right now, which is a normal state during a resize and not an error.
     */
    struct Handles
    {
        VkInstance       instance;
        VkPhysicalDevice physical;
        VkDevice         device;
        VkQueue          graphicsQueue;
        VkQueue          presentQueue;
        uint32_t         graphicsFamily;
        uint32_t         presentFamily;
        VkSwapchainKHR   swapchain;
        VkFormat         format;
        VkExtent2D       extent;
        uint32_t         imageCount;
        VkImageUsageFlags swapchainUsage;  ///< what the images were created for; gates a blit
        bool             textureCompressionBC; ///< BC1/2/3 images are legal on this device
        bool             largePoints;          ///< gl_PointSize above 1.0 is honoured; off, point sprites are one pixel
        bool             samplerAnisotropy;    ///< anisotropic samplers are legal on this device
        bool             occlusionQueryPrecise; ///< VK_QUERY_CONTROL_PRECISE_BIT is legal: exact sample counts
        bool             independentBlend;      ///< per-attachment blend state is legal
        bool             fillModeNonSolid;      ///< a LINE polygon mode (wireframe) is legal
        const loader::Instance* iapi;
        const loader::Device*   dapi;
    };

    /**
     * The handle view.
     *
     * @return pointer handles : null unless the instance and device exist; check `swapchain`
     *                           separately, since it comes and goes with every resize
     */
    const Handles* Get();

    /**
     * Bumped on every swapchain create and every swapchain destroy.
     *
     * A counter rather than a callback: it cannot be registered twice and cannot fire from inside a
     * teardown that is halfway done. Anything that cached an image view, a framebuffer or an image
     * count compares this against the value it built for and rebuilds when they differ.
     *
     * @return uint32 generation : starts at 0, before any swapchain has existed
     */
    uint32_t SwapchainGeneration();

    /**
     * The optional capabilities the device was created with (Context/Caps.cpp): its version, the
     * optional features asked for and enabled, the subgroup and multiview limits, the queue families
     * a later renderer may want to know about. Zeroed (a 1.0 device, nothing optional) before the
     * device exists.
     */
    const DeviceCaps& Caps();

    /**
     * The adapter's memory heaps, and the driver's budget and usage per heap when
     * VK_EXT_memory_budget is enabled. Not free (a driver query): a caller caches it.
     *
     * @param props  the heaps and types
     * @param budget VK_MAX_MEMORY_HEAPS entries: the driver's budget per heap, 0 where unknown
     * @param usage  VK_MAX_MEMORY_HEAPS entries: this process's usage per heap as the driver counts it, 0 where unknown
     * @return bool driver : true when budget and usage are the driver's; false when only `props` was filled
     */
    bool MemoryHeaps(VkPhysicalDeviceMemoryProperties& props, VkDeviceSize* budget, VkDeviceSize* usage);

    /**
     * The chosen adapter's name, vendor and driver, for the engine's crash log. Empty strings before
     * the device exists.
     *
     * @param char* name : receives "deviceName" (at least 256 bytes)
     * @param char* driver : receives "VID xxxx DID xxxx, api a.b.c, driver d.e.f.g" (at least 128 bytes)
     */
    void AdapterText(char* name, size_t nameCap, char* driver, size_t driverCap);
}
