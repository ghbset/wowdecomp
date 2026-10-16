// The Vulkan context's shared internals: its state and the helpers every part of Vulkan/Context/ uses.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/DeviceCaps.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Footprint.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Window.hpp"

#include <cstring>
#include <vector>

// The context lives in Vulkan/Context/, one file per stage of its bring-up, over this one header.
// Nothing here is visible outside it: the public face is Vulkan/Context.hpp.
//
//   Debug      validation messages, extension lookups, footprint lines
//   Instance   the instance
//   Surface    the window's surface
//   Physical   the physical device and its queue
//   Logical    the logical device
//   Caps       the optional capabilities
//   Swapchain  the swapchain and its rebuild
namespace gxvk::vk::context::detail
{
    namespace loader    = gxvk::vk::loader;
    namespace footprint = gxvk::vk::footprint;
    namespace window    = gxvk::vk::window;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;

    using gxvk::vk::ApiVersionText;
    using gxvk::vk::ColorSpaceName;
    using gxvk::vk::DeviceTypeName;
    using gxvk::vk::DriverVersionText;
    using gxvk::vk::FormatName;
    using gxvk::vk::PresentModeName;
    using gxvk::vk::ResultName;

    /// Everything this module owns, in one place so Teardown() cannot miss a field.
    ///
    /// Dispatchable handles (VkInstance, VkPhysicalDevice, VkDevice, VkQueue) are pointers;
    /// non-dispatchable ones (VkSurfaceKHR, VkSwapchainKHR, VkDebugUtilsMessengerEXT) are pointers on
    /// x64 and uint64_t on a Win32 build. VK_NULL_HANDLE is right for both.
    struct State
    {
        VkInstance               instance  = nullptr;
        VkPhysicalDevice         physical  = nullptr;
        VkDevice                 device    = nullptr;
        VkQueue                  graphicsQueue = nullptr;
        VkQueue                  presentQueue  = nullptr;

        VkSurfaceKHR             surface   = VK_NULL_HANDLE;
        VkSwapchainKHR           swapchain = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

        uint32_t graphicsFamily = UINT32_MAX;
        uint32_t presentFamily  = UINT32_MAX;

        HWND               hwnd        = nullptr;
        VkSurfaceFormatKHR surfaceFormat = { VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        VkPresentModeKHR   presentMode = VK_PRESENT_MODE_FIFO_KHR;
        VkExtent2D         extent      = { 0, 0 };
        uint32_t           imageCount  = 0;
        VkImageUsageFlags  usage       = 0;  ///< the imageUsage the swapchain was created with
        bool               bc          = false; ///< textureCompressionBC was enabled on the device
        bool               largePoints = false; ///< largePoints was enabled on the device (gl_PointSize above 1)
        bool               anisotropy  = false; ///< samplerAnisotropy was enabled on the device
        bool               preciseOcclusion = false; ///< occlusionQueryPrecise was enabled on the device
        bool               independentBlend = false; ///< independentBlend was enabled: per-attachment blend state
        bool               wireframe   = false; ///< fillModeNonSolid was enabled: a LINE polygon mode is legal

        bool vsync            = true;  ///< the engine's format asks for vsync (SetVsync)
        bool zeroSizeLogged   = false; ///< the "window is zero-size" line was written for the current down period
        bool validationWanted = false; ///< SetValidation: ask for the Khronos validation layer
        bool capsWanted       = true;  ///< probe and enable the optional capabilities (Caps.cpp)

        /// The chosen adapter's properties, for AdapterText and the caps.
        VkPhysicalDeviceProperties props = {};

        /// The optional capabilities (Caps.cpp): what was probed, asked for and enabled.
        DeviceCaps caps;
        /// vkGetPhysicalDeviceMemoryProperties2 (a 1.1 instance) for the memory budget; null without it.
        PFN_vkGetPhysicalDeviceMemoryProperties2 memoryProperties2 = nullptr;

        bool debugUtils = false;   ///< VK_EXT_debug_utils was enabled on the instance
        bool validation = false;   ///< VK_LAYER_KHRONOS_validation was enabled on the instance

        loader::Instance iapi;
        loader::Device   dapi;

        footprint::Snapshot baseline;     ///< before LoadLibraryA
        footprint::Snapshot afterLoader;
        footprint::Snapshot afterInstance;
        footprint::Snapshot afterDevice;

        bool     settled  = false; ///< Bring() reached Done or Declined; stop calling it
        unsigned attempts = 0;

        /// Bumped on every swapchain create and destroy. See context::SwapchainGeneration().
        uint32_t swapGen = 0;
    };
    extern State g;

    /// How many times Bring() may answer Retry before it gives up and declines.
    ///
    /// DeviceSetFormat calls it right after ICreateWindow, and ScenePresent calls it again while the
    /// window is not measurable (minimised, or a zero-size client rect mid-resize). 600 is about ten
    /// seconds of presents at 60 fps: long enough to cover an alt-tab during loading, short enough
    /// that a permanently unusable window stops being retried.
    constexpr unsigned kMaxAttempts = 600;

    /// The validation messages, per message id: the first kValidationRepeats of an id are logged
    /// whole, the rest counted, so an error a draw raises does not write a line per draw (a frame is
    /// tens of thousands of them) and bury the first occurrence. LogValidationSummary() names the
    /// counted ones. The layer may call from any thread that calls Vulkan; the table is locked.
    constexpr uint32_t kValidationRepeats = 5;
    constexpr uint32_t kValidationIds     = 128;
    struct ValidationId
    {
        int32_t     id       = 0;
        int         level    = 0;
        uint64_t    count    = 0;
        char        name[96] = {};
    };
    struct ValidationTable
    {
        SRWLOCK      lock = SRWLOCK_INIT;
        ValidationId ids[kValidationIds];
        uint32_t     idCount   = 0;
        uint64_t     untracked = 0;   ///< messages of ids past the table, all logged
        uint64_t     errors    = 0;
        uint64_t     warnings  = 0;
    };
    extern ValidationTable g_validation;

    /// One enumerated adapter, with everything the choice is made on.
    struct Candidate
    {
        VkPhysicalDevice           handle = nullptr;
        VkPhysicalDeviceProperties props  = {};
        uint32_t index          = 0;
        uint32_t graphicsFamily = UINT32_MAX;
        uint32_t presentFamily  = UINT32_MAX;
        uint64_t deviceLocal    = 0;       ///< bytes across its DEVICE_LOCAL heaps
        bool     unified        = false;   ///< one family does both graphics and present
        const char* reject      = nullptr; ///< non-null when the adapter cannot be used
    };

    // Debug.cpp
    bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name);
    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);
    VkDebugUtilsMessengerCreateInfoEXT MessengerInfo();
    void LogFootprint(const char* phase, const footprint::Snapshot& now);

    // Instance.cpp
    bool CreateInstance();

    // Surface.cpp
    bool CreateSurface(HWND hwnd);

    // Physical.cpp
    unsigned TypeRank(VkPhysicalDeviceType t);
    const char* WhyBetter(const Candidate& a, const Candidate& b);
    void Examine(Candidate& c);
    bool SelectPhysicalDevice();

    // Logical.cpp
    bool CreateDevice();

    // Caps.cpp: the optional capabilities.
    /// The instance version to ask for: the loader's up to 1.2, or 1.0 with the probe off. Notes it in g.caps.
    uint32_t InstanceVersion();
    /// What the device is created with beyond the six core features: the extension feature structs (pNext,
    /// chained in front of `tail`), the extensions (appended to `names`, `count` updated, at most `room`),
    /// the core features (set in `features`). Probes the chosen adapter first; nothing with the probe off.
    struct CapsChain
    {
        VkPhysicalDevice16BitStorageFeatures      storage16 = {};
        VkPhysicalDeviceShaderFloat16Int8Features float16   = {};
        VkPhysicalDeviceMultiviewFeatures         multiview = {};
        void*                                     head      = nullptr;
    };
    void PlanCaps(VkPhysicalDeviceFeatures& features, CapsChain& chain, void* tail, const char** names, uint32_t& count,
                  uint32_t room);
    /// After vkCreateDevice: what was enabled, the budget's entry point, the log line.
    void CapsCreated(bool chainUsed);

    // Swapchain.cpp
    VkSurfaceFormatKHR PickFormat(const std::vector<VkSurfaceFormatKHR>& formats, const char*& why);
    bool CreateSwapchain(const window::Found& win);
    void DropSwapchainObject();
    void DropSurface();
}
