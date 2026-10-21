// The pipelines' shared internals: the key, the tables, the state and the helpers every part of
// Vulkan/Pipelines/ uses.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Pipelines.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Manifest.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Shaders.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

// The pipelines live in Vulkan/Pipelines/, one file per responsibility, over this one header.
// Nothing here is visible outside it: the public face is Vulkan/Pipelines.hpp.
//
//   Programs     the backend's own shader modules (Shaders/Spirv.inc) and the translated ones
//   Layouts      the set-0 layouts (one per sampler-slot set), the set-1 layout, the pipeline layouts
//   Build        the blend and topology tables, the vertex input of each program family, the
//                pipeline of a key, the pipeline table
//   Samplers     the sampler cache, keyed on CGxTexFlags
//   Descriptors  the per-slot set-0 pools and their per-frame cache, the 1x1 white images
//   Regs         the per-slot uniform ring the translated interface's constant slices are written into
//   Pipelines.cpp  Init, Shutdown and the record of one draw
namespace gxvk::vk::pipelines::detail
{
    namespace context  = gxvk::vk::context;
    namespace frame    = gxvk::vk::frame;
    namespace loader   = gxvk::vk::loader;
    namespace manifest = gxvk::vk::manifest;
    namespace memory   = gxvk::vk::memory;
    namespace shaders  = gxvk::vk::shaders;
    namespace texture  = gxvk::vk::texture;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;
    using gxvk::vk::pipelines::kMaxStreams;
    using gxvk::vk::pipelines::kAttribCount;
    using gxvk::vk::pipelines::kTextureStages;
    using gxvk::vk::pipelines::kPixelRows;
    using gxvk::vk::pipelines::kPixelRowsMax;
    using gxvk::vk::pipelines::kVertexRowsSmall;
    using gxvk::vk::pipelines::kVertexRowsLarge;
    using gxvk::vk::pipelines::Attribute;
    using gxvk::vk::pipelines::Draw;
    using gxvk::vk::pipelines::Texture;
    using gxvk::vk::pipelines::VertexProgram;
    using gxvk::vk::pipelines::FragmentProgram;

    /// The pipeline table's first capacity: a session's distinct combinations are some hundreds
    /// with the translated families. A full table doubles (entries and index), so no key is refused.
    constexpr uint32_t kFirstPipelines = 1024;

    /// The push constants of Shaders/Push.glsl: a mat4, a vec4 and three vec4 of texture transform,
    /// 128 bytes (Vulkan's guaranteed maxPushConstantsSize). Every pipeline layout carries the
    /// range; the translated modules do not read it.
    constexpr uint32_t kPushBytes = 16 * sizeof(float) + 4 * sizeof(float) + 12 * sizeof(float);

    /// Samplers kept at once.
    constexpr uint32_t kMaxSamplers = 64;

    /// The sampler key of the white images: nearest, clamp.
    constexpr uint32_t kWhiteSamplerKey = 0;

    /// Distinct sampler-slot sets (set-0 layouts) kept at once.
    constexpr uint32_t kMaxLayouts = 32;

    /// Set-0 descriptor sets one frame may allocate, and the combined image samplers behind them.
    constexpr uint32_t kSetsPerSlot        = 4096;
    constexpr uint32_t kDescriptorsPerSlot = 4 * kSetsPerSlot;

    /// The per-slot cache over those sets: twice the sets, a power of two, for short probes.
    constexpr uint32_t kCacheSlots = 2 * kSetsPerSlot;
    static_assert((kCacheSlots & (kCacheSlots - 1)) == 0, "the probe masks with kCacheSlots - 1");

    /// The uniform ring: chunks per frame slot, each a mapped UNIFORM_BUFFER with one set-1
    /// descriptor over it; a frame that fills a chunk moves to the next.
    constexpr uint32_t     kRegsChunkBytes = 4u * 1024u * 1024u;
    constexpr uint32_t     kMaxRegsChunks  = 16;

    /// One uniform slice of the translated interface: p[16], misc, v[rows] (Shaders: the Regs
    /// block); for a pixel record with pconst > 16, v[256] then px[pconst - 16] at
    /// kSliceBytesLarge. The set-1 descriptor's range is kSliceBytesMax; a module reads the first
    /// bytes of it that its block declares.
    constexpr uint32_t kSliceHeadBytes  = (kPixelRows + 1) * 16;
    constexpr uint32_t kSliceBytesSmall = kSliceHeadBytes + kVertexRowsSmall * 16;
    constexpr uint32_t kSliceBytesLarge = kSliceHeadBytes + kVertexRowsLarge * 16;
    constexpr uint32_t kSliceBytesMax   = kSliceBytesLarge + (kPixelRowsMax - kPixelRows) * 16;

    /// The constant stream a vertex without a colour or a texcoord reads at stride 0: 16 bytes of
    /// D3DCOLOR white at 0 (opaque white), 16 zero bytes at 16 (a zero texcoord).
    constexpr uint32_t kConstantColourOffset   = 0;
    constexpr uint32_t kConstantTexcoordOffset = 16;
    constexpr uint32_t kConstantBytes          = 32;

    constexpr uint32_t kVertexPrograms   = static_cast<uint32_t>(VertexProgram::Count);
    constexpr uint32_t kFragmentPrograms = static_cast<uint32_t>(FragmentProgram::Count);

    /// The engine attribute indices (EGxVertexAttrib) the programs read.
    constexpr uint32_t kAttribPosition  = 0;
    constexpr uint32_t kAttribColor0    = 4;
    constexpr uint32_t kAttribTexCoord0 = 6;
    constexpr uint32_t kAttribTexCoord1 = 7;

    /// Whether a program is of the translated family (reads set 1, the semantic interface).
    inline bool TranslatedFamily(VertexProgram v) { return v == VertexProgram::Translated || v == VertexProgram::FixedFunctionSemantic; }
    inline bool TranslatedFamily(FragmentProgram f) { return f == FragmentProgram::Translated || f == FragmentProgram::FixedFunctionTranslated; }

    /// What a pipeline is built from. Compared bytewise, so every byte is set (zeroed first).
    struct Key
    {
        VkRenderPass pass;
        uint32_t     passGen;
        uint32_t     samples;         ///< rasterizationSamples (the pass's: the MSAA scene's or 1)
        uint32_t     streamCount;
        uint32_t     stride[kMaxStreams];
        Attribute    attribs[kAttribCount];
        uint32_t     attribMask;
        uint32_t     topology;
        uint32_t     blend;
        uint32_t     depthTest;       ///< 0 / 1
        uint32_t     depthWrite;      ///< 0 / 1
        uint32_t     depthFunc;
        uint32_t     cull;
        uint32_t     colourMask;
        uint32_t     wireframe;       ///< 0 / 1
        uint32_t     vertex;
        uint32_t     fragment;
        uint16_t     vertexRecord;    ///< manifest index, kNoRecord when not Translated
        uint16_t     fragmentRecord;
        uint32_t     layoutIndex;     ///< the set-0 layout (sampler-slot set)
        uint32_t     stencil;         ///< Draw::stencil (0: off)
        uint32_t     stencilMasks;    ///< Draw::stencilMasks without the reference (dynamic state), 0 when off
        uint32_t     alphaToCoverage; ///< 0 / 1 (gxAlphaToCoverage, MSAA passes only)
        uint32_t     normal;          ///< GBUFFER: 0 one colour attachment, 1 a masked normal attachment, 2 a written one
    };

    struct Entry
    {
        Key        key;
        VkPipeline pipeline = VK_NULL_HANDLE;   ///< VK_NULL_HANDLE: the key was refused, remembered so it is not retried
    };

    /// What is bound on the command buffer being recorded; stale across command buffers and frames.
    struct Bound
    {
        VkCommandBuffer  cmd      = nullptr;
        uint64_t         serial   = 0;
        VkPipeline       pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout   = VK_NULL_HANDLE;
        VkDescriptorSet  set0     = VK_NULL_HANDLE;
        VkDescriptorSet  set1     = VK_NULL_HANDLE;
        uint32_t         offset1  = 0;
        bool             have1    = false;
        VkBuffer         vbs[kMaxStreams + 1]     = {};
        VkDeviceSize     offsets[kMaxStreams + 1] = {};
        uint32_t         vbCount  = 0;
        VkBuffer         ib       = VK_NULL_HANDLE;
        VkViewport       viewport = {};
        VkRect2D         scissor  = {};
        bool             placed   = false;
        float            push[kPushBytes / sizeof(float)] = {};
        bool             pushed   = false;
        float            depthBias        = 0.0f;
        bool             biasSet          = false;
        float            blendConstant[4] = {};
        bool             blendSet         = false;
        uint32_t         stencilRef       = 0;
        bool             stencilRefSet    = false;
    };

    struct SamplerEntry
    {
        uint32_t  key     = 0;
        VkSampler sampler = VK_NULL_HANDLE;
    };

    /// One set-0 layout: binding s for each slot s of the mask, and the pipeline layout over it
    /// and the set-1 layout.
    struct LayoutEntry
    {
        uint32_t              mask   = 0;
        VkDescriptorSetLayout set0   = VK_NULL_HANDLE;
        VkPipelineLayout      layout = VK_NULL_HANDLE;
    };

    /// One set-0 descriptor set written this frame, for one (mask, images, samplers) tuple.
    struct CacheEntry
    {
        uint64_t        serial = 0;   ///< the frame it was written in; another serial means empty
        uint32_t        mask   = 0;
        uint32_t        imageId[kTextureStages]    = {};
        uint32_t        samplerKey[kTextureStages] = {};
        VkDescriptorSet set    = VK_NULL_HANDLE;
    };

    /// One frame slot's set-0 pool and the cache over the sets allocated from it this frame, and
    /// its uniform ring.
    struct SlotSets
    {
        VkDescriptorPool pool      = VK_NULL_HANDLE;
        uint64_t         serial    = 0;        ///< the frame the pool was last reset for
        uint32_t         allocated = 0;        ///< sets taken from the pool this frame
        CacheEntry*      cache     = nullptr;  ///< kCacheSlots entries, heap

        memory::Buffer  regs[kMaxRegsChunks];
        VkDescriptorSet regsSet[kMaxRegsChunks] = {};
        uint32_t        regsCount  = 0;        ///< chunks made
        uint32_t        regsUsed   = 0;        ///< chunks written this frame (the current one included)
        uint32_t        regsCursor = 0;        ///< next free byte of the current chunk
        uint64_t        regsSerial = 0;        ///< the frame the ring was last reset for
        /// The last slice written this frame, to bind again rather than write twice.
        uint8_t         lastSlice[kSliceBytesMax] = {};
        uint32_t        lastSliceBytes  = 0;
        VkDescriptorSet lastSliceSet    = VK_NULL_HANDLE;
        uint32_t        lastSliceOffset = 0;
        uint64_t        lastVertexGen   = 0;       ///< Draw::vertexRegsGen of lastSlice's vertex rows
    };

    struct State
    {
        bool                  ready  = false;
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        VkShaderModule        vert[kVertexPrograms]   = {};   ///< the backend's own; Translated stays null
        VkShaderModule        frag[kFragmentPrograms] = {};
        VkDescriptorSetLayout regsLayout = VK_NULL_HANDLE;    ///< set 1: one dynamic uniform buffer, both stages
        VkDescriptorPool      regsPool   = VK_NULL_HANDLE;    ///< the set-1 sets, for the session
        VkPipelineCache       cache      = VK_NULL_HANDLE;
        memory::Buffer        constant;                       ///< the constant stream (kConstantBytes)
        Entry*                entries    = nullptr;           ///< entryCap, heap, dense
        uint32_t              entryCount = 0;
        uint32_t              entryCap   = 0;
        uint32_t*             index      = nullptr;           ///< 2 * entryCap slots, open-addressed: dense index + 1, 0 empty
        uint32_t              lastHit    = 0;                 ///< dense index + 1 of the last key found, 0 none
        uint32_t              passGen    = 0;                 ///< the generation the entries were built for
        Bound                 bound;

        LayoutEntry  layouts[kMaxLayouts];
        uint32_t     layoutCount = 0;

        SamplerEntry samplers[kMaxSamplers];
        uint32_t     samplerCount = 0;
        float        anisoMax     = 1.0f;                     ///< maxSamplerAnisotropy when the feature is on, else 1

        uint32_t     regsAlign = 256;                         ///< minUniformBufferOffsetAlignment
        bool         wireframe = false;                       ///< fillModeNonSolid: a LINE polygon mode is legal

        SlotSets       slots[frame::kFramesInFlight];
        texture::Image white;                                 ///< 1x1 opaque white, made on first need
        texture::Image whiteCube;                             ///< 1x1 opaque white cube map, made on first need
        bool           whiteFailed     = false;
        bool           whiteCubeFailed = false;

        gxvk::vk::pipelines::Stats stats;
            bool warnedKey      = false;
        bool warnedSamplers = false;
        bool warnedSets     = false;
        bool warnedLayouts  = false;
        bool warnedRegs     = false;
    };
    extern State s;

    // Programs.cpp
    bool CreatePrograms();
    void DestroyPrograms();
    VkShaderModule VertexModule(const Key& k);
    VkShaderModule FragmentModule(const Key& k);

    // Layouts.cpp
    bool CreateRegsLayout();
    bool LayoutFor(uint32_t samplerMask, uint32_t& index);
    void DestroyLayouts();

    // Build.cpp
    const Entry* Lookup(const Key& k);
    bool GrowPipelines();
    void DropPipelines();

    // Samplers.cpp
    VkSampler Sampler(uint32_t key);
    void DestroySamplers();

    // Descriptors.cpp
    bool CreateDescriptorObjects();
    void DestroyDescriptorObjects();
    VkDescriptorSet DescriptorFor(uint32_t slot, uint64_t serial, const LayoutEntry& layout, const Texture* stages);
    const texture::Image* White(bool cube);

    // Regs.cpp
    bool RegsSlice(uint32_t slot, uint64_t serial, const Draw& draw, VkDescriptorSet& set, uint32_t& offset);
    void DestroyRegs(SlotSets& slot);
}
