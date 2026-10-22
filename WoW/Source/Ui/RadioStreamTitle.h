// Radio (port of wxl-radio): the text handling, kept apart so Tools/RadioCheck/check.cpp runs it natively.
#pragma once

#include <cstddef>
#include <cstring>

namespace Radio
{
    /// Copies the title out of ICY metadata ("StreamTitle='Artist - Song';StreamUrl='';") into out.
    /// Returns false, leaving out alone, when the metadata carries no StreamTitle.
    inline bool ParseStreamTitle(const char* meta, char* out, size_t outSize)
    {
        static const char kKey[] = "StreamTitle='";
        const char* begin = meta ? std::strstr(meta, kKey) : nullptr;
        if (!begin || !outSize) return false;
        begin += sizeof kKey - 1;
        const char* end = std::strstr(begin, "';");
        if (!end) return false;
        size_t n = static_cast<size_t>(end - begin);
        if (n > outSize - 1) n = outSize - 1;
        std::memcpy(out, begin, n);
        out[n] = 0;
        return true;
    }

    /// Only web streams: the URL comes from an addon's edit box.
    inline bool IsStreamUrl(const char* url)
    {
        return url && (!std::strncmp(url, "http://", 7) || !std::strncmp(url, "https://", 8));
    }
}
