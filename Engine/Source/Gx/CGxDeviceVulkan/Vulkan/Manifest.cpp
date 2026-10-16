// The translated-shader manifest: the parser and the tables.
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Manifest.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
    using namespace gxvk::vk::manifest;
    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;

    /// The tables' bounds, generous against the client's archives (about 1600 records, 2700 names);
    /// a manifest past them is truncated with a warning rather than refused.
    constexpr uint32_t kMaxRecords = 4096;
    constexpr uint32_t kMaxNames   = 8192;
    constexpr uint16_t kNoName     = 0xFFFF;

    /// The md5 index: open addressing, twice the records, a power of two.
    constexpr uint32_t kHashSlots = 2 * kMaxRecords;
    static_assert((kHashSlots & (kHashSlots - 1)) == 0, "the probe masks with kHashSlots - 1");

    const char* kSemanticNames[kSemanticCount] = {
        "position0", "normal0", "color0", "color1",
        "texcoord0", "texcoord1", "texcoord2", "texcoord3", "texcoord4", "texcoord5", "texcoord6", "texcoord7",
        "blendweight0", "blendindices0", "fog0", "psize0",
    };

    struct Name
    {
        char     name[kNameChars];
        uint16_t record;
    };

    struct State
    {
        Record*   records     = nullptr;   ///< kMaxRecords, heap
        Name*     names       = nullptr;   ///< kMaxNames, heap
        uint16_t* hash        = nullptr;   ///< kHashSlots: record index + 1, 0 empty
        uint32_t  recordCount = 0;
        uint32_t  nameCount   = 0;
        char      folder[512] = {};
        bool      loaded      = false;
        Stats     stats;
    };
    State s;

    /// The md5's first eight hex digits as the slot.
    uint32_t HashOf(const char* md5)
    {
        uint32_t h = 0;
        for (uint32_t i = 0; i < 8 && md5[i]; ++i)
        {
            const char c = md5[i];
            const uint32_t v = c >= 'a' ? static_cast<uint32_t>(c - 'a' + 10) : static_cast<uint32_t>(c - '0');
            h = (h << 4) | (v & 15);
        }
        h ^= h >> 13;
        h *= 0x9E3779B1u;
        return h & (kHashSlots - 1);
    }

    void HashInsert(uint32_t index)
    {
        uint32_t p = HashOf(s.records[index].md5);
        while (s.hash[p] != 0) p = (p + 1) & (kHashSlots - 1);
        s.hash[p] = static_cast<uint16_t>(index + 1);
    }

    /// Splits one line into at most `cap` tokens on spaces and tabs, dropping a '#' comment.
    uint32_t Tokenise(char* line, char** tokens, uint32_t cap)
    {
        if (char* hash = std::strchr(line, '#')) *hash = '\0';
        uint32_t n = 0;
        char*    p = line;
        while (*p && n < cap)
        {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
            if (!*p) break;
            tokens[n++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
            if (*p) *p++ = '\0';
        }
        return n;
    }

    bool ParseSemantic(const char* token, uint32_t& out)
    {
        for (uint32_t i = 0; i < kSemanticCount; ++i)
        {
            if (std::strcmp(token, kSemanticNames[i]) == 0)
            {
                out = i;
                return true;
            }
        }
        return false;
    }

    /// Copies a bounded string, refusing one that would not fit: a truncated md5 or path names a
    /// different file.
    template <size_t N>
    bool CopyBounded(char (&dst)[N], const char* src)
    {
        const size_t len = std::strlen(src);
        if (len + 1 > N) return false;
        std::memcpy(dst, src, len + 1);
        return true;
    }

    /// 32 hex characters, lowercased in place.
    bool NormaliseMd5(char* token)
    {
        if (std::strlen(token) != 32) return false;
        for (char* p = token; *p; ++p)
        {
            if (*p >= 'A' && *p <= 'F') *p = static_cast<char>(*p - 'A' + 'a');
            const bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
            if (!hex) return false;
        }
        return true;
    }

    bool ParseRecord(char** t, uint32_t n, Record& r)
    {
        if (n < 7) return false;
        Record out = {};
        out.firstName = kNoName;
        out.companion = kNoRecord;
        if (!NormaliseMd5(t[1]) || !CopyBounded(out.md5, t[1])) return false;

        if (std::strcmp(t[2], "vertex") == 0)     out.kind = Kind::Vertex;
        else if (std::strcmp(t[2], "pixel") == 0) out.kind = Kind::Pixel;
        else return false;

        if (std::strcmp(t[3], "vs_3_0") == 0)      out.profile = Profile::Vs30;
        else if (std::strcmp(t[3], "ps_3_0") == 0) out.profile = Profile::Ps30;
        else if (std::strcmp(t[3], "vs_2_0") == 0) out.profile = Profile::Vs20;
        else if (std::strcmp(t[3], "ps_2_0") == 0) out.profile = Profile::Ps20;
        else if (std::strcmp(t[3], "ff") == 0 && out.kind == Kind::Pixel) out.profile = Profile::Ff;
        else return false;

        if (!CopyBounded(out.spv, t[4])) return false;

        bool haveV = false, haveP = false;
        for (uint32_t i = 5; i < n; ++i)
        {
            if (std::strncmp(t[i], "vconst=", 7) == 0)
            {
                const long v = std::strtol(t[i] + 7, nullptr, 10);
                if (v != 64 && v != 256) return false;
                out.vconst = static_cast<uint16_t>(v);
                haveV      = true;
            }
            else if (std::strncmp(t[i], "pconst=", 7) == 0)
            {
                const long p = std::strtol(t[i] + 7, nullptr, 10);
                if (p <= 0 || p > 16) return false;
                out.pconst = static_cast<uint16_t>(p);
                haveP      = true;
            }
        }
        if (!haveV || !haveP) return false;
        r = out;
        return true;
    }

    /// One attribute line applied to the current record. False for a line that did not parse.
    bool ParseAttribute(char** t, uint32_t n, Record& r, uint32_t recordIndex)
    {
        if (std::strcmp(t[0], "name") == 0)
        {
            if (n != 2) return false;
            if (s.nameCount >= kMaxNames)
            {
                ++s.stats.truncated;
                return true;
            }
            Name& e = s.names[s.nameCount];
            if (!CopyBounded(e.name, t[1])) return false;
            e.record = static_cast<uint16_t>(recordIndex);
            if (r.firstName == kNoName) r.firstName = static_cast<uint16_t>(s.nameCount);
            ++s.nameCount;
            ++s.stats.names;
            return true;
        }
        if (std::strcmp(t[0], "sampler") == 0)
        {
            if (n != 3) return false;
            char*      end  = nullptr;
            const long slot = std::strtol(t[1], &end, 10);
            if (!end || *end || slot < 0 || slot > 15) return false;
            const bool cube = std::strcmp(t[2], "cube") == 0;
            if (!cube && std::strcmp(t[2], "2d") != 0) return false;
            r.samplerMask |= static_cast<uint16_t>(1u << slot);
            if (cube) r.cubeMask |= static_cast<uint16_t>(1u << slot);
            return true;
        }
        if (std::strcmp(t[0], "input") == 0)
        {
            // The register token is informative only: the location is fixed by semantic.
            if (n != 3) return false;
            uint32_t sem = 0;
            if (!ParseSemantic(t[1], sem)) return false;
            r.inputMask |= 1u << sem;
            return true;
        }
        if (std::strcmp(t[0], "output") == 0 || std::strcmp(t[0], "reads") == 0)
        {
            if (n != 2) return false;
            uint32_t sem = 0;
            if (!ParseSemantic(t[1], sem)) return false;
            if (t[0][0] == 'o') r.outputMask |= 1u << sem;
            else                r.readsMask  |= 1u << sem;
            return true;
        }
        if (std::strcmp(t[0], "flag") == 0)
        {
            if (n != 2) return false;
            if (std::strcmp(t[1], "fixed_fog") == 0)         r.fixedFog = true;
            else if (std::strcmp(t[1], "point_size") == 0)   r.pointSize = true;
            else if (std::strcmp(t[1], "point_sprite") == 0) r.pointSprite = true;
            else return false;
            return true;
        }
        if (std::strcmp(t[0], "companion") == 0)
        {
            // Kept as the md5 for now: the companion blocks come after the vertex rows that name
            // them; the index is resolved once the whole file has been read.
            if (n != 2 || r.kind != Kind::Vertex) return false;
            if (!NormaliseMd5(t[1]) || !CopyBounded(r.companionMd5, t[1])) return false;
            return true;
        }
        // Variant lines a fuller translator may emit: not used here.
        ++s.stats.ignored;
        return true;
    }

    void LinkCompanions()
    {
        for (uint32_t i = 0; i < s.recordCount; ++i)
        {
            Record& r = s.records[i];
            if (r.kind != Kind::Vertex || r.companionMd5[0] == '\0') continue;
            const Record* c = ByMd5(r.companionMd5);
            if (c && c->kind == Kind::Pixel && c->profile == Profile::Ff)
            {
                r.companion = IndexOf(c);
                ++s.stats.companions;
            }
            else
            {
                ++s.stats.companionsUnresolved;
                Write(Warn, "vulkan: manifest: %s names companion %s, which is not an ff pixel record of this file", FirstName(r), r.companionMd5);
            }
        }
    }

    bool ReadWhole(const char* path, char*& out, size_t& size)
    {
        out  = nullptr;
        size = 0;
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f) return false;
        bool ok = false;
        if (std::fseek(f, 0, SEEK_END) == 0)
        {
            const long end = std::ftell(f);
            if (end > 0 && std::fseek(f, 0, SEEK_SET) == 0)
            {
                out = new (std::nothrow) char[static_cast<size_t>(end) + 1];
                if (out && std::fread(out, 1, static_cast<size_t>(end), f) == static_cast<size_t>(end))
                {
                    out[end] = '\0';
                    size     = static_cast<size_t>(end);
                    ok       = true;
                }
            }
        }
        std::fclose(f);
        if (!ok)
        {
            delete[] out;
            out = nullptr;
        }
        return ok;
    }
}

namespace gxvk::vk::manifest
{
    bool Load(const char* folder)
    {
        Unload();
        if (!folder || !*folder) return false;
        std::snprintf(s.folder, sizeof s.folder, "%s", folder);

        char path[640];
        std::snprintf(path, sizeof path, "%s\\manifest.txt", folder);
        char*  text = nullptr;
        size_t size = 0;
        if (!ReadWhole(path, text, size))
        {
            Write(Warn, "vulkan: manifest: no \"%s\": the client's stock shaders stay invalid and the engine draws them through its fixed-function paths (run Tools/VulkanShaders/translate_shaders.py)", path);
            return false;
        }

        s.records = new (std::nothrow) Record[kMaxRecords];
        s.names   = new (std::nothrow) Name[kMaxNames];
        s.hash    = new (std::nothrow) uint16_t[kHashSlots];
        if (!s.records || !s.names || !s.hash)
        {
            Write(Error, "vulkan: manifest: cannot allocate the tables");
            delete[] text;
            Unload();
            return false;
        }
        std::memset(s.hash, 0, sizeof(uint16_t) * kHashSlots);

        // Line by line; a `record` line opens a block, the attribute lines apply to it.
        bool  inRecord = false;
        char* line     = text;
        while (line && *line)
        {
            char* next = std::strchr(line, '\n');
            if (next) *next++ = '\0';

            char*    tokens[8];
            uint32_t n = Tokenise(line, tokens, 8);
            if (n == 0)
            {
                line = next;
                continue;
            }
            if (std::strcmp(tokens[0], "record") == 0)
            {
                inRecord = false;
                if (s.recordCount >= kMaxRecords)
                {
                    ++s.stats.truncated;
                }
                else
                {
                    Record r;
                    if (ParseRecord(tokens, n, r))
                    {
                        s.records[s.recordCount] = r;
                        HashInsert(s.recordCount);
                        ++s.recordCount;
                        ++s.stats.records;
                        if (r.kind == Kind::Vertex) ++s.stats.vertex;
                        else
                        {
                            ++s.stats.pixel;
                            if (r.profile == Profile::Ff) ++s.stats.ff;
                        }
                        inRecord = true;
                    }
                    else
                        ++s.stats.skipped;
                }
            }
            else if (inRecord)
            {
                if (!ParseAttribute(tokens, n, s.records[s.recordCount - 1], s.recordCount - 1)) ++s.stats.skipped;
            }
            else
                ++s.stats.skipped;
            line = next;
        }
        delete[] text;

        LinkCompanions();
        s.loaded = s.recordCount > 0;
        Write(s.loaded ? Info : Warn, "vulkan: manifest: \"%s\": %u record(s) (%u vertex, %u pixel of which %u generated), %u name(s), %u companion(s) (%u unresolved), %u line(s) skipped, %u ignored, %u truncated",
              path, s.stats.records, s.stats.vertex, s.stats.pixel, s.stats.ff, s.stats.names, s.stats.companions,
              s.stats.companionsUnresolved, s.stats.skipped, s.stats.ignored, s.stats.truncated);
        return s.loaded;
    }

    void Unload()
    {
        delete[] s.records;
        delete[] s.names;
        delete[] s.hash;
        s = State();
    }

    bool Loaded() { return s.loaded; }

    const char* Folder() { return s.folder; }

    const Record* ByMd5(const char* md5)
    {
        if (!s.loaded || !md5 || std::strlen(md5) != 32) return nullptr;
        for (uint32_t p = HashOf(md5);; p = (p + 1) & (kHashSlots - 1))
        {
            const uint16_t v = s.hash[p];
            if (v == 0) return nullptr;
            if (std::strcmp(s.records[v - 1].md5, md5) == 0) return &s.records[v - 1];
        }
    }

    const Record* At(uint32_t index)
    {
        return s.loaded && index < s.recordCount ? &s.records[index] : nullptr;
    }

    uint32_t Count() { return s.loaded ? s.recordCount : 0; }

    uint16_t IndexOf(const Record* record)
    {
        if (!record || !s.records || record < s.records || record >= s.records + s.recordCount) return kNoRecord;
        return static_cast<uint16_t>(record - s.records);
    }

    const Record* Companion(const Record& r)
    {
        return r.companion == kNoRecord ? nullptr : At(r.companion);
    }

    const char* FirstName(const Record& r)
    {
        return r.firstName != kNoName && r.firstName < s.nameCount ? s.names[r.firstName].name : r.md5;
    }

    const char* SemanticName(uint32_t semantic)
    {
        return semantic < kSemanticCount ? kSemanticNames[semantic] : "?";
    }

    void GetStats(Stats& out) { out = s.stats; }
}
