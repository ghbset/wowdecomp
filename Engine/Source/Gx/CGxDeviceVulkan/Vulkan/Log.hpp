// The Vulkan tier's log: one function over the engine's gx.log (CGxDevice::Log). The level becomes
// a tag in front of the line, and the line lands in Logs\gx.log beside the engine's own device
// messages, which is where someone reading a failed bring-up looks first.
#pragma once

namespace gxvk::log
{
    enum Level
    {
        Debug,
        Info,
        Warn,
        Error,
    };

    /**
     * One line in gx.log, printf-formatted, prefixed with the level's tag.
     *
     * @param Level level : Debug, Info, Warn or Error
     * @param string format : printf format
     */
    void Write(Level level, const char* format, ...);
}
