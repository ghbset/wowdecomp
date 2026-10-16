// The window's surface.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context/Internal.hpp"

namespace gxvk::vk::context::detail
{
    /**
     * Creates the Win32 surface on an HWND.
     *
     * The HINSTANCE is read off the window itself (GWLP_HINSTANCE) rather than assumed: it is the
     * module that registered the window's class (the executable, for "GxWindowClassVulkan"), and
     * the window's own answer cannot drift from it. The spec only requires a valid HINSTANCE, but a
     * layer or driver may look the class up with it.
     *
     * @param HWND hwnd : the window to present into
     * @return bool ok
     */
    bool CreateSurface(HWND hwnd)
    {
        VkWin32SurfaceCreateInfoKHR info = {};
        info.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hinstance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrA(hwnd, GWLP_HINSTANCE));
        if (!info.hinstance) info.hinstance = ::GetModuleHandleA(nullptr);
        info.hwnd      = hwnd;

        const VkResult r = g.iapi.CreateWin32SurfaceKHR(g.instance, &info, nullptr, &g.surface);
        if (r != VK_SUCCESS)
        {
            Write(Warn, "vulkan: vkCreateWin32SurfaceKHR failed: %s", ResultName(r));
            g.surface = VK_NULL_HANDLE;
            return false;
        }
        g.hwnd = hwnd;
        return true;
    }
}
