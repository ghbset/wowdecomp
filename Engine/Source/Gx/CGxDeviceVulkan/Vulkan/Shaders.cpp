// SPIR-V modules read from the translated-shader folder, created once and cached by path.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Shaders.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Manifest.hpp"

#include <cstdio>
#include <cstring>
#include <new>

namespace
{
    namespace context = gxvk::vk::context;
    namespace loader  = gxvk::vk::loader;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Warn;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;

    /// The most paths the cache remembers: the translated set a scene asks for is a few hundred
    /// of the archives' ~1600.
    constexpr uint32_t kMaxModules = 4096;

    constexpr size_t kMaxPath = 95;

    /// The first word of every SPIR-V module in the host's byte order.
    constexpr uint32_t kSpirvMagic = 0x07230203u;

    struct Entry
    {
        char           path[kMaxPath + 1] = {};
        VkShaderModule module             = VK_NULL_HANDLE;   ///< VK_NULL_HANDLE when the load failed
    };

    struct State
    {
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        char                  folder[512] = {};
        Entry*                entries    = nullptr;   ///< kMaxModules, heap
        uint32_t              count      = 0;
        bool                  fullLogged = false;
        bool                  ready      = false;
        gxvk::vk::shaders::Stats stats;
    };
    State s;

    Entry* Find(const char* path)
    {
        for (uint32_t i = 0; i < s.count; ++i)
            if (std::strncmp(s.entries[i].path, path, kMaxPath) == 0) return &s.entries[i];
        return nullptr;
    }

    /// Reads a whole file into a word array (4-aligned, as vkCreateShaderModule wants its code).
    bool ReadWords(const char* path, uint32_t*& words, size_t& bytes, const char*& why)
    {
        words = nullptr;
        bytes = 0;
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f)
        {
            why = "the file could not be opened";
            return false;
        }
        bool ok = false;
        do
        {
            if (std::fseek(f, 0, SEEK_END) != 0) { why = "seek failed"; break; }
            const long size = std::ftell(f);
            if (size < 4 || size % 4 != 0) { why = "the size is not a whole number of SPIR-V words"; break; }
            if (std::fseek(f, 0, SEEK_SET) != 0) { why = "seek failed"; break; }
            words = new (std::nothrow) uint32_t[static_cast<size_t>(size) / 4];
            if (!words) { why = "no memory for the file"; break; }
            if (std::fread(words, 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) { why = "the read came up short"; break; }
            bytes = static_cast<size_t>(size);
            ok    = true;
        } while (false);
        std::fclose(f);
        if (!ok)
        {
            delete[] words;
            words = nullptr;
        }
        return ok;
    }
}

namespace gxvk::vk::shaders
{
    bool Init(const char* folder)
    {
        const context::Handles* h = context::Get();
        if (!h || !h->device || !h->dapi)
        {
            Write(Warn, "vulkan: shaders: Init() without a live context");
            return false;
        }
        if (s.ready)
        {
            if (s.device == h->device) return true;
            Write(Warn, "vulkan: shaders: the context reports a different VkDevice; forgetting %u module(s) rather than destroying them", s.count);
            delete[] s.entries;
            s = State();
        }
        s.entries = new (std::nothrow) Entry[kMaxModules];
        if (!s.entries)
        {
            Write(Error, "vulkan: shaders: cannot allocate the module table");
            return false;
        }
        std::snprintf(s.folder, sizeof s.folder, "%s", folder ? folder : "");
        s.device = h->device;
        s.dapi   = h->dapi;
        s.ready  = true;
        Write(Debug, "vulkan: shaders: ready (translated modules are read from \"%s\" on first use, at most %u cached)", s.folder, kMaxModules);
        return true;
    }

    void Shutdown()
    {
        if (!s.ready) return;
        if (s.dapi && s.device && context::Live())
            for (uint32_t i = 0; i < s.count; ++i)
                if (s.entries[i].module != VK_NULL_HANDLE) s.dapi->DestroyShaderModule(s.device, s.entries[i].module, nullptr);
        delete[] s.entries;
        s = State();
    }

    VkShaderModule Get(const char* relative)
    {
        if (!s.ready || !relative || !*relative) return VK_NULL_HANDLE;
        if (std::strlen(relative) > kMaxPath)
        {
            Write(Warn, "vulkan: shaders: refusing the path \"%s\": longer than %u", relative, static_cast<unsigned>(kMaxPath));
            return VK_NULL_HANDLE;
        }
        if (Entry* e = Find(relative)) return e->module;

        if (s.count >= kMaxModules)
        {
            if (!s.fullLogged)
            {
                s.fullLogged = true;
                Write(Warn, "vulkan: shaders: the cache holds %u paths; \"%s\" and anything after it are refused (logged once)", kMaxModules, relative);
            }
            return VK_NULL_HANDLE;
        }

        // The entry is claimed before the load, so a failure is remembered by path and never retried.
        Entry& e = s.entries[s.count++];
        std::strncpy(e.path, relative, kMaxPath);
        e.path[kMaxPath] = '\0';

        char path[640];
        std::snprintf(path, sizeof path, "%s\\%s", s.folder, relative);
        for (char* p = path; *p; ++p)
            if (*p == '/') *p = '\\';

        uint32_t*   words = nullptr;
        size_t      bytes = 0;
        const char* why   = "";
        // Data\\custom.MPQ first, the loose file beside the exe after (Manifest ReadPacked).
        char*  packed = nullptr;
        size_t packedSize = 0;
        if (gxvk::vk::manifest::ReadPacked(relative, packed, packedSize))
        {
            if (packedSize >= 4 && packedSize % 4 == 0 && (words = new (std::nothrow) uint32_t[packedSize / 4]))
            {
                std::memcpy(words, packed, packedSize);
                bytes = packedSize;
            }
            delete[] packed;
        }
        if (!words && !ReadWords(path, words, bytes, why))
        {
            ++s.stats.failed;
            Write(Warn, "vulkan: shaders: \"%s\": %s", path, why);
            return VK_NULL_HANDLE;
        }
        if (words[0] != kSpirvMagic)
        {
            ++s.stats.failed;
            Write(Warn, "vulkan: shaders: \"%s\": first word is 0x%08X, not the SPIR-V magic", path, static_cast<unsigned>(words[0]));
            delete[] words;
            return VK_NULL_HANDLE;
        }

        VkShaderModuleCreateInfo info = {};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes;
        info.pCode    = words;
        VkShaderModule module = VK_NULL_HANDLE;
        const VkResult r = s.dapi->CreateShaderModule(s.device, &info, nullptr, &module);
        delete[] words;
        if (r != VK_SUCCESS)
        {
            ++s.stats.failed;
            Write(Warn, "vulkan: shaders: vkCreateShaderModule failed for \"%s\": %s", path, ResultName(r));
            return VK_NULL_HANDLE;
        }
        e.module = module;
        ++s.stats.loaded;
        s.stats.bytes += bytes;
        return module;
    }

    void GetStats(Stats& out) { out = s.stats; }
}
