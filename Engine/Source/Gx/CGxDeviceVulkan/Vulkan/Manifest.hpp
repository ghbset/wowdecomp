// The translated-shader manifest: what Tools/VulkanShaders produced from the client's own DX9
// shaders, read once into fixed tables and looked up by the md5 of a shader's bytes.
#pragma once

#include <cstdint>

// The client's DX9 shader bytecode is translated offline (Tools/VulkanShaders) into one SPIR-V module
// per byte-distinct record, and the device finds those modules through manifest.txt, keyed by the MD5
// of the record's bytes -- the bytes the engine keeps in CGxShader::m_code. The manifest's text
// format: blocks separated by a blank line, lines of space-separated tokens, '#' to end of line a
// comment.
//
//   record <md5> <vertex|pixel> <vs_3_0|ps_3_0|vs_2_0|ps_2_0|ff> <spv path> vconst=<64|256> pconst=<16..256>
//   name <Name:perm>               repeatable; also the bare <Name> for record 0 of a container
//   sampler <slot 0..15> <2d|cube> pixel only, repeatable
//   input <semantic> <vN>          vertex only, repeatable
//   output <semantic>              vertex only, repeatable
//   reads <semantic>               pixel only, repeatable
//   flag <fixed_fog|point_size|point_sprite>  optional
//   companion <md5>                vertex only: the `ff` pixel record to bind when the draw has no pixel shader
//
// Profile `ff` is a pixel record the translator GENERATED rather than translated: the fixed-function
// pixel cascade in the translated varying interface, for the vs_2_0 weather records the engine draws
// with no pixel shader. It has no bytecode (its md5 is of its GLSL text), so it is never matched at a
// create; a vertex record's `companion` line is the only way to reach it. Any other line kind is
// counted and ignored.
//
// No Vulkan, no engine, no device: a parser over a text file.
namespace gxvk::vk::manifest
{
    /// The semantics the contract names, as bit positions.
    enum Semantic : uint32_t
    {
        kPosition0     = 0,
        kNormal0       = 1,
        kColor0        = 2,
        kColor1        = 3,
        kTexcoord0     = 4,  ///< texcoord0..texcoord7 are 4..11
        kBlendWeight0  = 12,
        kBlendIndices0 = 13,
        kFog0          = 14,
        kPsize0        = 15,
        kSemanticCount = 16,
    };

    enum class Kind : uint8_t
    {
        Vertex = 0,
        Pixel  = 1,
    };

    enum class Profile : uint8_t
    {
        Vs30, Ps30, Vs20, Ps20, Ff,
    };

    /// "No record".
    constexpr uint16_t kNoRecord = 0xFFFF;

    /// The longest `name` token kept, NUL included ("Combiners_Opaque_AddAlpha_Alpha:15" is 34).
    constexpr uint32_t kNameChars = 40;

    /// The longest spv path kept, NUL included ("pixel/" + 32 hex + ".spv" is 42).
    constexpr uint32_t kPathChars = 64;

    /// One byte-distinct record the translator produced.
    struct Record
    {
        char     md5[33];          ///< lowercase hex, NUL-terminated
        char     spv[kPathChars];  ///< relative to the manifest's folder, forward slashes
        Kind     kind;
        Profile  profile;
        uint16_t vconst;           ///< 64 or 256: rows of regs.v[] the vertex module declares
        uint16_t pconst;           ///< 16..256: pixel rows the module reads (above 16: c16.. in regs.px[])
        uint16_t samplerMask;      ///< pixel: bit s set for `sampler s ...`
        uint16_t cubeMask;         ///< pixel: bit s set when that sampler is a cube
        uint32_t inputMask;        ///< vertex: bit Semantic set for each `input`
        uint32_t outputMask;       ///< vertex: bit Semantic set for each `output`
        uint32_t readsMask;        ///< pixel: bit Semantic set for each `reads`
        bool     fixedFog;         ///< `flag fixed_fog`
        bool     pointSize;        ///< `flag point_size`: the vertex module writes gl_PointSize; the draw is a point list
        bool     pointSprite;      ///< `flag point_sprite`: the ff module samples stage 0 at gl_PointCoord
        bool     normalOut;        ///< `flag normal_out`: the pixel module writes oC1, the view-space normal (GBUFFER)
        uint16_t firstName;        ///< index of its first name entry, or 0xFFFF
        uint16_t companion;        ///< vertex: index of its `ff` companion record, or kNoRecord
        char     companionMd5[33]; ///< vertex: the `companion` line's md5 as written, "" when none
    };

    /**
     * Reads <folder>/manifest.txt once into the tables. A missing file is a logged warning: with no
     * manifest every lookup answers null and only the backend's own programs draw. A malformed line
     * is skipped and counted; the file is never refused whole for one bad line.
     *
     * @param const char* folder : the folder holding manifest.txt and vertex/, pixel/
     * @return bool loaded : at least one record was read
     */
    bool Load(const char* folder);

    /// Forgets the tables, so Load() may read again.
    void Unload();

    /// Whether Load() found a manifest with at least one record.
    bool Loaded();

    /// The folder Load() was given.
    const char* Folder();

    /**
     * Reads VulkanShaders\\<relative> out of the client's archives (Data\\custom.MPQ). False when the
     * archives lack it, or when GX_VULKAN_SHADERS is set (a dev folder then wins), so the caller
     * reads the loose file instead.
     *
     * @param out : new[]'d, NUL-terminated past size; the caller delete[]s it
     */
    bool ReadPacked(const char* relative, char*& out, size_t& size);

    /**
     * The record with this content hash.
     *
     * @param const char* md5 : 32 lowercase hex characters
     * @return const Record* record : null when unknown or when nothing is loaded
     */
    const Record* ByMd5(const char* md5);

    /// The record at an index (what IndexOf() gave), or null.
    const Record* At(uint32_t index);

    /// How many records the table holds.
    uint32_t Count();

    /// A record's index in the table, or kNoRecord.
    uint16_t IndexOf(const Record* record);

    /// The `ff` companion a vertex record names, or null.
    const Record* Companion(const Record& r);

    /// A record's first name, for a log line, or its md5 when it has none.
    const char* FirstName(const Record& r);

    /// A semantic's spelling in the contract, for a log line.
    const char* SemanticName(uint32_t semantic);

    struct Stats
    {
        uint32_t records    = 0;
        uint32_t vertex     = 0;
        uint32_t pixel      = 0;
        uint32_t ff         = 0;   ///< pixel records of profile ff
        uint32_t names      = 0;
        uint32_t skipped    = 0;   ///< lines that did not parse
        uint32_t ignored    = 0;   ///< lines of a kind this reader does not use
        uint32_t truncated  = 0;   ///< records or names dropped because a table was full
        uint32_t companions = 0;   ///< vertex records whose companion resolved
        uint32_t companionsUnresolved = 0;
    };
    void GetStats(Stats& out);
}
