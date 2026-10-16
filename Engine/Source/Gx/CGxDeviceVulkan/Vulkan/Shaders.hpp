// SPIR-V modules read from the translated-shader folder, created once and cached by path.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

// The translated modules live where the manifest does (<folder>/vertex/<md5>.spv,
// <folder>/pixel/<md5>.spv), named by the manifest's spv paths. This module reads one, checks it is
// SPIR-V, creates the VkShaderModule once and remembers it by path; a failure is remembered too, so a
// missing file costs one open per session and not one per draw. Render thread only.
namespace gxvk::vk::shaders
{
    /**
     * Prepares the cache for the live device. Idempotent.
     *
     * @param const char* folder : the folder the manifest's spv paths are relative to
     * @return bool ok : false without a live context
     */
    bool Init(const char* folder);

    /// Destroys every module. After the device is idle, after the pipelines are gone.
    void Shutdown();

    /**
     * The module for <folder>/<relative>, created on first use.
     *
     * @param const char* relative : a manifest spv path, forward or back slashes
     * @return VkShaderModule module : VK_NULL_HANDLE when the file is missing or not SPIR-V (logged
     *         once per path), or when the cache is full
     */
    VkShaderModule Get(const char* relative);

    struct Stats
    {
        uint32_t loaded = 0;
        uint32_t failed = 0;
        uint64_t bytes  = 0;
    };
    void GetStats(Stats& out);
}
