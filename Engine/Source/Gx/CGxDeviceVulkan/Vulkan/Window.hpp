// The render window as the context sees it: the HWND the device owns, validated and measured.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

namespace gxvk::vk::window
{
    /// What one measurement produced.
    struct Found
    {
        HWND     hwnd   = nullptr;
        unsigned width  = 0;      ///< client area, from GetClientRect
        unsigned height = 0;
    };

    /**
     * Measures the window the device renders into.
     *
     * The window is validated, not trusted: IsWindow, then a client rect. A zero-size client area is
     * reported as `hwnd` set with width and height 0 so the caller can retry rather than treat a
     * minimised window as a failure -- vkCreateSwapchainKHR with a 0x0 extent is invalid usage.
     *
     * @param HWND hwnd : the device's window
     * @return Found found : hwnd null when the handle is not a window
     */
    inline Found Measure(HWND hwnd)
    {
        Found found;
        if (!hwnd) return found;

        if (!::IsWindow(hwnd))
        {
            gxvk::log::Write(gxvk::log::Warn, "vulkan: window: %p is not a window; ignoring it",
                             static_cast<void*>(hwnd));
            return found;
        }

        found.hwnd = hwnd;
        RECT client = {};
        if (::GetClientRect(hwnd, &client))
        {
            found.width  = static_cast<unsigned>(client.right - client.left);
            found.height = static_cast<unsigned>(client.bottom - client.top);
        }
        return found;
    }
}
