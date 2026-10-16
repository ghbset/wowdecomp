// The validation layer's messages, the extension lookups, the address-space footprint lines.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context/Internal.hpp"

namespace gxvk::vk::context::detail
{
    State g;
    ValidationTable g_validation;

    /**
     * Whether an extension name appears in an enumerated list.
     *
     * @param vector list : what vkEnumerate*ExtensionProperties returned
     * @param string name : the extension name, e.g. VK_KHR_SWAPCHAIN_EXTENSION_NAME
     * @return bool present
     */
    bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
    {
        for (const VkExtensionProperties& e : list)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    }

    /**
     * The validation layer's own output, routed into gx.log.
     *
     * Each message is one line: `vulkan: validation <ERROR|WARN|info> #n [<id name>] <message>`,
     * the message being the layer's own text (it names the objects, the VUID and the spec's
     * sentence). An id's sixth and later occurrences are counted, not logged (kValidationRepeats);
     * the fifth says so.
     *
     * Returns VK_FALSE unconditionally, which is what the spec requires of an application callback:
     * VK_TRUE is reserved for the layers' own internal tests and aborts the call.
     */
    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT        types,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void*                                  user)
    {
        (void)user;

        int         level = Debug;
        const char* tag   = "info";
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)        { level = Error; tag = "ERROR"; }
        else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) { level = Warn;  tag = "WARN"; }
        if (level == Warn && (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) tag = "perf";

        const int32_t id   = data ? data->messageIdNumber : 0;
        const char*   name = (data && data->pMessageIdName) ? data->pMessageIdName : "?";
        uint64_t      nth  = 0;
        AcquireSRWLockExclusive(&g_validation.lock);
        if (level == Error) ++g_validation.errors;
        if (level == Warn) ++g_validation.warnings;
        uint32_t i = 0;
        for (; i < g_validation.idCount; ++i)
            if (g_validation.ids[i].id == id && std::strncmp(g_validation.ids[i].name, name, sizeof g_validation.ids[i].name - 1) == 0)
                break;
        if (i == g_validation.idCount && g_validation.idCount < kValidationIds)
        {
            ValidationId& e = g_validation.ids[g_validation.idCount++];
            e.id    = id;
            e.level = level;
            std::strncpy(e.name, name, sizeof e.name - 1);
            e.name[sizeof e.name - 1] = '\0';
        }
        if (i < g_validation.idCount) nth = ++g_validation.ids[i].count;
        else ++g_validation.untracked;
        ReleaseSRWLockExclusive(&g_validation.lock);

        if (nth > kValidationRepeats) return VK_FALSE;
        Write(static_cast<gxvk::log::Level>(level), "vulkan: validation %s #%llu [%s]%s %s", tag,
              static_cast<unsigned long long>(nth ? nth : 1), name,
              nth == kValidationRepeats ? " (further occurrences of this id are counted only)" : "",
              (data && data->pMessage) ? data->pMessage : "(no message)");
        return VK_FALSE;
    }

    /// The messenger's create info, shared by the instance pNext chain and the real messenger.
    VkDebugUtilsMessengerCreateInfoEXT MessengerInfo()
    {
        VkDebugUtilsMessengerCreateInfoEXT info = {};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        // VERBOSE is left out on purpose: the verbose channel is thousands of lines per instance
        // creation and would bury everything else in gx.log.
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = &DebugCallback;
        return info;
    }

    /**
     * Logs one address-space measurement against the baseline.
     *
     * @param string phase : what was just created
     * @param Snapshot& now : the measurement
     */
    void LogFootprint(const char* phase, const footprint::Snapshot& now)
    {
        Write(Debug, "vulkan: address space after %s: committed %u MiB (%+d), reserved %u MiB (%+d), largest free block %u MiB",
              phase, footprint::Mib(now.committed), footprint::MibDelta(now.committed, g.baseline.committed),
              footprint::Mib(now.reserved), footprint::MibDelta(now.reserved, g.baseline.reserved),
              footprint::Mib(now.largestFree));
    }
}
