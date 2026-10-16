// The logical device, its features and its queue.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context/Internal.hpp"

namespace gxvk::vk::context::detail
{
    /**
     * Creates the logical device with one queue per distinct family, and resolves the device level.
     *
     * Six features are requested, each only when the adapter reports it. Nothing else, because an
     * unused enabled feature is an unused cost -- some drivers allocate differently for
     * robustBufferAccess and the like -- and because asking for a feature the adapter lacks fails the
     * create.
     *
     * No device layers either: enabledLayerCount on VkDeviceCreateInfo has been deprecated and
     * ignored since Vulkan 1.0.x, and instance layers already cover the device.
     *
     * @return bool ok
     */
    bool CreateDevice()
    {
        const float priority = 1.0f;

        VkDeviceQueueCreateInfo queues[2] = {};
        uint32_t queueCount = 0;

        queues[queueCount].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queues[queueCount].queueFamilyIndex = g.graphicsFamily;
        queues[queueCount].queueCount       = 1;
        queues[queueCount].pQueuePriorities = &priority;
        ++queueCount;

        // One VkDeviceQueueCreateInfo per family, never two for the same family: the spec forbids a
        // duplicate queueFamilyIndex in the array, and the unified case is exactly when that would
        // happen.
        if (g.presentFamily != g.graphicsFamily)
        {
            queues[queueCount].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queues[queueCount].queueFamilyIndex = g.presentFamily;
            queues[queueCount].queueCount       = 1;
            queues[queueCount].pQueuePriorities = &priority;
            ++queueCount;
        }

        // The swapchain, then what the optional capabilities add by name (Caps.cpp PlanCaps: the
        // memory budget, gl_Layer from the vertex stage, FP16 on a 1.1 device).
        constexpr uint32_t kMaxExtensions = 6;
        const char* extensions[kMaxExtensions] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        uint32_t    extensionCount = 1;

        // Features: ask only for what the adapter reports, because asking for anything else fails
        // the create.
        VkPhysicalDeviceFeatures features = {};
        {
            VkPhysicalDeviceFeatures have = {};
            g.iapi.GetPhysicalDeviceFeatures(g.physical, &have);
            // textureCompressionBC: nearly every texture the client loads is a DXT-compressed BLP,
            // and uploading those as BC1/BC2/BC3 is only legal with the feature on. Every desktop GPU
            // has it; the query exists so that a machine without it gets decoded RGBA8 images and a
            // log line instead of a device that fails to come up.
            features.textureCompressionBC = have.textureCompressionBC;
            g.bc = have.textureCompressionBC == VK_TRUE;
            // robustBufferAccess: an out-of-bounds buffer read returns zero (or in-buffer bytes)
            // instead of faulting the device. The translated shaders index the bone palette with a
            // register the engine fills, a vertex buffer is bound whole and addressed by the
            // engine's own offsets: every one of those is a read this feature turns from a lost
            // device into a wrong pixel. Every desktop driver supports it.
            features.robustBufferAccess = have.robustBufferAccess;
            // largePoints: the weather sprites (sand, snow) are point lists whose vertex module
            // writes gl_PointSize, and without the feature every size above 1.0 is clamped to one
            // pixel.
            features.largePoints = have.largePoints;
            g.largePoints        = have.largePoints == VK_TRUE;
            // samplerAnisotropy: the engine's GxTex_Anisotropic filter with the texture's own
            // maxAnisotropy; without the feature such a sampler is trilinear.
            features.samplerAnisotropy = have.samplerAnisotropy;
            g.anisotropy               = have.samplerAnisotropy == VK_TRUE;
            // occlusionQueryPrecise: D3D9's occlusion query returns the exact count of samples that
            // passed, and its one user (the sun / moon glare, DayNight::DNGlare) divides that count
            // by the quad's projected area to scale the glare.
            features.occlusionQueryPrecise = have.occlusionQueryPrecise;
            g.preciseOcclusion             = have.occlusionQueryPrecise == VK_TRUE;
            // independentBlend: per-attachment blend state, for a multi-attachment pass class.
            features.independentBlend = have.independentBlend;
            g.independentBlend        = have.independentBlend == VK_TRUE;
            // fillModeNonSolid: the engine's wireframe (GxMasterEnable_PolygonFill off) is a LINE
            // polygon mode, legal only with the feature; without it such a draw stays filled.
            features.fillModeNonSolid = have.fillModeNonSolid;
            g.wireframe               = have.fillModeNonSolid == VK_TRUE;
            if (!g.bc)
                Write(Warn, "vulkan: the adapter has no textureCompressionBC; DXT textures are decoded to RGBA8, four to eight times larger");
            if (!g.preciseOcclusion)
                Write(Warn, "vulkan: the adapter has no occlusionQueryPrecise; occlusion queries may count any passing draw as one sample (the sun glare dims)");
            if (!g.largePoints)
                Write(Warn, "vulkan: the adapter has no largePoints; point sprites (the weather's sand and snow) will draw one pixel wide");
        }

        // The optional capabilities (Caps.cpp): every optional feature the adapter reports, the
        // extension features chained, their extensions named.
        const VkPhysicalDeviceFeatures plainFeatures = features;
        CapsChain capsChain;
        PlanCaps(features, capsChain, nullptr, extensions, extensionCount, kMaxExtensions);
        const bool richer = capsChain.head != nullptr || extensionCount > 1
                            || std::memcmp(&plainFeatures, &features, sizeof features) != 0;

        VkDeviceCreateInfo info = {};
        info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pNext                   = capsChain.head;
        info.queueCreateInfoCount    = queueCount;
        info.pQueueCreateInfos       = queues;
        info.enabledExtensionCount   = extensionCount;
        info.ppEnabledExtensionNames = extensions;
        info.pEnabledFeatures        = &features;

        VkResult r = g.iapi.CreateDevice(g.physical, &info, nullptr, &g.device);
        bool chainUsed = richer;
        if (r != VK_SUCCESS && richer)
        {
            // The richer device refused (a driver that reports a feature it will not enable): the
            // device as it was, its six features and the swapchain extension only.
            Write(Warn, "vulkan: vkCreateDevice with the optional capabilities failed (%s); creating the plain device",
                  ResultName(r));
            g.device = nullptr;
            const char* plainNames[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
            features = plainFeatures;
            info.pNext                   = nullptr;
            info.enabledExtensionCount   = 1;
            info.ppEnabledExtensionNames = plainNames;
            r = g.iapi.CreateDevice(g.physical, &info, nullptr, &g.device);
            chainUsed = false;
        }
        if (r != VK_SUCCESS)
        {
            Write(Warn, "vulkan: vkCreateDevice failed: %s", ResultName(r));
            g.device = nullptr;
            return false;
        }
        CapsCreated(chainUsed);

        if (!loader::LoadDevice(g.iapi, g.device, g.dapi))
        {
            if (g.dapi.DestroyDevice) g.dapi.DestroyDevice(g.device, nullptr);
            g.device = nullptr;
            return false;
        }

        g.dapi.GetDeviceQueue(g.device, g.graphicsFamily, 0, &g.graphicsQueue);
        g.dapi.GetDeviceQueue(g.device, g.presentFamily, 0, &g.presentQueue);

        Write(Debug, "vulkan: logical device created with %u queue(s): graphics family %u%s, present family %u%s. Extensions: %u (%s first)",
              queueCount, g.graphicsFamily, g.graphicsQueue ? "" : " (queue handle null!)", g.presentFamily,
              g.presentFamily == g.graphicsFamily ? " (same family)" : "", info.enabledExtensionCount,
              VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        return true;
    }
}
