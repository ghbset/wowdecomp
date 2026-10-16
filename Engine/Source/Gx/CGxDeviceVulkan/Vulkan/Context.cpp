// The Vulkan objects the backend owns: instance, physical device, logical device, queues, Win32
// surface and swapchain. Created at runtime, destroyed in reverse order.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context/Internal.hpp"

#include <cstdio>

using namespace gxvk::vk::context::detail;

namespace gxvk::vk::context
{
    Attempt Bring(HWND hwnd)
    {
        if (g.settled) return g.device ? Attempt::Done : Attempt::Declined;

        if (++g.attempts > kMaxAttempts)
        {
            Write(Warn, "vulkan: gave up after %u frames waiting for a usable window; declining", kMaxAttempts);
            g.settled = true;
            Teardown();
            return Attempt::Declined;
        }

        // The window is checked first, before anything is created, so a client whose window is
        // minimised costs a GetClientRect per attempt and nothing else.
        const window::Found win = window::Measure(hwnd);

        if (!win.hwnd || win.width == 0 || win.height == 0)
        {
            // Only the first wait is logged; the rest would be one line per frame.
            if (g.attempts == 1)
                Write(Debug, "vulkan: no usable window yet (%s); waiting", win.hwnd ? "zero-size client area" : "no HWND");
            return Attempt::Retry;
        }

        g.baseline = footprint::Take();
        Write(Debug, "vulkan: bring-up starting on HWND %p (%ux%u client); address space before: committed %u MiB, "
                     "reserved %u MiB, largest free block %u MiB",
              static_cast<void*>(win.hwnd), win.width, win.height, footprint::Mib(g.baseline.committed),
              footprint::Mib(g.baseline.reserved), footprint::Mib(g.baseline.largestFree));

        // Each step is final: a failure here is a permanent decline, not a retry, because none of
        // these failures is a function of time. Teardown() unwinds whatever did get created.
        g.settled = true;

        if (!loader::Load())                 { Teardown(); return Attempt::Declined; }
        g.afterLoader = footprint::Take();
        LogFootprint("loading vulkan-1.dll", g.afterLoader);

        if (!CreateInstance())               { Teardown(); return Attempt::Declined; }
        g.afterInstance = footprint::Take();
        LogFootprint("vkCreateInstance", g.afterInstance);

        // The surface before the physical-device choice, because present support on THIS surface is
        // one of the three requirements that choice is made on.
        if (!CreateSurface(win.hwnd))        { Teardown(); return Attempt::Declined; }
        if (!SelectPhysicalDevice())         { Teardown(); return Attempt::Declined; }
        if (!CreateDevice())                 { Teardown(); return Attempt::Declined; }
        g.afterDevice = footprint::Take();
        LogFootprint("vkCreateDevice", g.afterDevice);

        if (!CreateSwapchain(win))
        {
            // The instance, device and surface are good; only the swapchain failed. They are kept:
            // the next rebuild (a resize, a present that finds it missing) tries again.
            Write(Warn, "vulkan: instance, device and surface stand; the swapchain does not");
        }

        const footprint::Snapshot now = footprint::Take();
        LogFootprint("the whole bring-up", now);
        return Attempt::Done;
    }

    void ReleaseWindow()
    {
        if (!g.device) return;
        DropSwapchainObject();
        DropSurface();
    }

    void Rebind(HWND hwnd)
    {
        if (!g.device) return;

        const window::Found win = window::Measure(hwnd);
        if (!win.hwnd)
        {
            Write(Warn, "vulkan: the device has no window; the surface and swapchain stay down");
            DropSwapchainObject();
            DropSurface();
            return;
        }

        // A format change or a graphics restart destroys the window and makes a new one, so the
        // surface's HWND is the thing to compare -- not the extent.
        if (win.hwnd != g.hwnd)
        {
            Write(Debug, "vulkan: the window changed (%p -> %p); rebuilding the surface as well as the swapchain",
                  static_cast<void*>(g.hwnd), static_cast<void*>(win.hwnd));
            DropSwapchainObject();
            DropSurface();
            if (!CreateSurface(win.hwnd)) return;

            // The adapter is not re-chosen. Present support is re-verified instead, because that is
            // the one property of the choice that a new window can invalidate -- a window moved to a
            // display driven by the other adapter. Re-running the whole selection would mean a new
            // VkDevice, and every object the device holds would go with it.
            VkBool32 present = VK_FALSE;
            if (g.iapi.GetPhysicalDeviceSurfaceSupportKHR(g.physical, g.presentFamily, g.surface, &present) != VK_SUCCESS
                || !present)
            {
                Write(Warn, "vulkan: queue family %u cannot present to the new window; the surface stands but no "
                            "swapchain is built. The chosen adapter is no longer the one driving this window.",
                      g.presentFamily);
                return;
            }
        }
        else
        {
            DropSwapchainObject();
        }

        if (win.width == 0 || win.height == 0)
        {
            // Once per down period: a minimised window asks for this every frame.
            if (!g.zeroSizeLogged)
            {
                g.zeroSizeLogged = true;
                Write(Debug, "vulkan: the window is zero-size (minimised); the swapchain stays down until it is restored");
            }
            return;
        }
        CreateSwapchain(win);
    }

    void LogValidationSummary()
    {
        AcquireSRWLockShared(&g_validation.lock);
        if (g_validation.idCount || g_validation.untracked)
        {
            Write(g_validation.errors ? Warn : Info, "vulkan: validation: %llu error(s), %llu warning(s) over %u message id(s)%s",
                  static_cast<unsigned long long>(g_validation.errors),
                  static_cast<unsigned long long>(g_validation.warnings), g_validation.idCount,
                  g_validation.untracked ? " (more ids than the table holds, all logged)" : "");
            for (uint32_t i = 0; i < g_validation.idCount; ++i)
            {
                const ValidationId& e = g_validation.ids[i];
                if (e.count <= kValidationRepeats) continue;
                Write(e.level == Error ? Warn : Info, "vulkan: validation: [%s] (id 0x%08X) %llu time(s), the first %u logged",
                      e.name, static_cast<unsigned>(e.id), static_cast<unsigned long long>(e.count), kValidationRepeats);
            }
        }
        ReleaseSRWLockShared(&g_validation.lock);
    }

    void Teardown()
    {
        const bool hadVulkan = g.instance || g.device || g.surface != VK_NULL_HANDLE;

        // Reverse order of creation, and each step guarded on its own handle so Teardown() is also
        // the unwinder for a half-finished Bring().
        if (g.device)
        {
            if (g.dapi.DeviceWaitIdle) g.dapi.DeviceWaitIdle(g.device);
            DropSwapchainObject();
            // Unconditionally, even though DropSwapchainObject has already bumped it when there was
            // a swapchain to drop. The generation's contract is "anything you derived from the
            // swapchain is stale", and destroying the device makes that true whether a swapchain
            // existed or not.
            ++g.swapGen;
            if (g.dapi.DestroyDevice) g.dapi.DestroyDevice(g.device, nullptr);
            g.device        = nullptr;
            g.graphicsQueue = nullptr;
            g.presentQueue  = nullptr;
            g.physical      = nullptr;
        }
        else
        {
            g.swapchain = VK_NULL_HANDLE;
            ++g.swapGen;
        }

        DropSurface();

        if (g.messenger != VK_NULL_HANDLE && g.iapi.DestroyDebugUtilsMessengerEXT)
        {
            g.iapi.DestroyDebugUtilsMessengerEXT(g.instance, g.messenger, nullptr);
            g.messenger = VK_NULL_HANDLE;
        }

        if (g.instance && g.iapi.DestroyInstance)
        {
            g.iapi.DestroyInstance(g.instance, nullptr);
            g.instance = nullptr;
        }

        // vulkan-1.dll is deliberately left loaded: FreeLibrary reclaims little and risks the
        // loader's own detach-order hazards, and a graphics restart would load it again anyway.
        //
        // The attempt state is reset so a later DeviceCreate (a gxRestart after a failed one, or
        // another device after this one is destroyed) gets a fresh bring-up rather than the memory of
        // a settled decline.
        g.settled  = false;
        g.attempts = 0;
        if (hadVulkan) Write(Debug, "vulkan: torn down");
    }

    bool Live() { return g.instance != nullptr && g.device != nullptr; }

    void SetVsync(bool on) { g.vsync = on; }

    void SetValidation(bool on) { g.validationWanted = on; }

    const Handles* Get()
    {
        if (!g.instance || !g.device) return nullptr;

        // Refilled on every call rather than kept in step with the dozen sites that assign to State.
        // The copy is a few words on a path that runs once a frame; a cached view that somebody
        // forgot to refresh after a resize is a bug that shows up once an hour and looks like a
        // driver problem.
        //
        // One static, handed out by pointer, and therefore single-threaded by contract: every caller
        // is on the render thread. A caller that wants these from another thread must copy the
        // struct, not keep the pointer.
        static Handles view;
        view.instance       = g.instance;
        view.physical       = g.physical;
        view.device         = g.device;
        view.graphicsQueue  = g.graphicsQueue;
        view.presentQueue   = g.presentQueue;
        view.graphicsFamily = g.graphicsFamily;
        view.presentFamily  = g.presentFamily;
        view.swapchain      = g.swapchain;
        view.format         = g.surfaceFormat.format;
        view.extent         = g.extent;
        view.imageCount     = g.imageCount;
        view.swapchainUsage = g.usage;
        view.textureCompressionBC = g.bc;
        view.largePoints    = g.largePoints;
        view.samplerAnisotropy = g.anisotropy;
        view.occlusionQueryPrecise = g.preciseOcclusion;
        view.independentBlend      = g.independentBlend;
        view.fillModeNonSolid      = g.wireframe;
        view.iapi           = &g.iapi;
        view.dapi           = &g.dapi;
        return &view;
    }

    uint32_t SwapchainGeneration() { return g.swapGen; }

    void AdapterText(char* name, size_t nameCap, char* driver, size_t driverCap)
    {
        if (!g.physical)
        {
            if (nameCap) name[0] = '\0';
            if (driverCap) driver[0] = '\0';
            return;
        }
        char apiText[32];
        char driverText[32];
        std::snprintf(name, nameCap, "%s", g.props.deviceName);
        std::snprintf(driver, driverCap, "VID %04x DID %04x, api %s, driver %s", g.props.vendorID, g.props.deviceID,
                      ApiVersionText(g.props.apiVersion, apiText, sizeof apiText),
                      DriverVersionText(g.props.vendorID, g.props.driverVersion, driverText, sizeof driverText));
    }
}
