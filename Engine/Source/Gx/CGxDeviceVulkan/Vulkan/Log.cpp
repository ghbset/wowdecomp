// The Vulkan tier's log: one function over the engine's gx.log (CGxDevice::Log).
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"

#include <cstdarg>
#include <cstdio>

namespace gxvk::log
{
    void Write(Level level, const char* format, ...)
    {
        static const char* const kTags[] = { "debug", "info", "WARN", "ERROR" };

        char line[2048];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof line, format, args);
        va_end(args);

        // CGxDevice::Log is a no-op until GxLogOpen() has run, which ConsoleDeviceInitialize does
        // before the device is created; nothing here needs to know.
        CGxDevice::Log("[%s] %s", kTags[level], line);
    }
}
