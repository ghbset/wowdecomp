// The graphics pipelines a draw binds, cached by what the draw needs; the samplers, the per-frame
// descriptor sets and the uniform slices a draw binds; and the record of one draw.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

namespace gxvk::vk::manifest { struct Record; }

// Two families of programs, combined per draw:
//
//   The backend's own (Shaders/*.vert, *.frag), in their own interface -- position, colour,
//   texcoord 0 and texcoord 1 at locations 0..3, the transform and parameters pushed:
//     vertex    FixedFunction  the position through the matrix the draw pushes (World * View * ProjNative),
//                              the texcoords through the stages' texture transforms
//               Ui             the engine's "UI" vertex shader: the position against c0..c3 pushed
//     fragment  FixedFunction  D3D9's texture stages 0 and 1 as the engine configures them, then the alpha test
//               Ui             the engine's "UI" pixel shader: texture * vertex colour, alpha test
//               Desaturate     the engine's "Desaturate" pixel shader: the texture's luminance * colour
//               Msdf           the MSDF font shader (Gxu "UI_MSDF"): reads pixel c0 from the set-1 slice
//
//   The translated (Tools/VulkanShaders: the client's own DX9 shaders), in the translated interface --
//   vertex inputs and varyings at fixed locations per semantic, the constant registers in a uniform
//   slice at set 1, one combined image sampler per D3D sampler slot at set 0 binding <slot>:
//     vertex    Translated             a manifest vertex record's module
//               FixedFunctionSemantic  the fixed-function vertex stage in that interface, for a
//                                      translated pixel module drawn with no vertex shader
//     fragment  Translated             a manifest pixel record's module (a generated companion included)
//               FixedFunctionTranslated the fixed-function pixel stage in that interface, for a
//                                      translated vertex module drawn with no pixel shader
//
// A pipeline is built for each distinct combination of vertex layout, programs (and records),
// topology, blend mode, depth and culling state, colour write mask, fill mode, sampler-slot set and
// render pass, on first use, and kept for the device's life. The viewport, the scissor, the depth
// bias and the blend constants are dynamic. Everything is Vulkan 1.0 core.
namespace gxvk::vk::pipelines
{
    /// Streams one draw may bind: one per distinct buffer of the engine's attribute table.
    constexpr uint32_t kMaxStreams = 8;

    /// The engine's vertex attributes (EGxVertexAttrib): position, blend weight, blend indices,
    /// normal, colour 0, colour 1, texcoord 0..7.
    constexpr uint32_t kAttribCount = 14;

    /// D3D's texture stages / sampler slots.
    constexpr uint32_t kTextureStages = 16;

    /// The constant register files of the translated interface: pixel c0..c15, vertex c0..c63 or
    /// c0..c255 by the vertex record's `vconst`. A pixel record with `pconst` above 16 (our own
    /// custom.MPQ shaders) also reads c16..c(pconst-1), placed after a full 256-row vertex window.
    constexpr uint32_t kPixelRows       = 16;
    constexpr uint32_t kPixelRowsMax    = 256;
    constexpr uint32_t kVertexRowsSmall = 64;
    constexpr uint32_t kVertexRowsLarge = 256;

    enum class VertexProgram : uint32_t
    {
        FixedFunction = 0,
        Ui            = 1,
        FixedFunctionSemantic = 2,
        Translated    = 3,
        Count
    };

    enum class FragmentProgram : uint32_t
    {
        FixedFunction = 0,
        Ui            = 1,
        Desaturate    = 2,
        FixedFunctionTranslated = 3,
        Translated    = 4,
        Msdf          = 5,
        Count
    };

    struct Stream
    {
        VkBuffer     buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;     ///< byte offset the stream starts at inside the buffer
        uint32_t     stride = 0;
    };

    /// One vertex attribute: where in which stream, and how it is encoded.
    struct Attribute
    {
        uint32_t binding = 0;        ///< the stream
        uint32_t offset  = 0;        ///< inside the vertex
        VkFormat format  = VK_FORMAT_UNDEFINED;
    };

    /// One texture stage: the image's view and id (texture::Image), its sampler key (CGxTexFlags
    /// bits 0-4 and 9-13) and whether the image is a cube map.
    struct Texture
    {
        VkImageView view       = VK_NULL_HANDLE;
        uint32_t    imageId    = 0;
        uint32_t    samplerKey = 0;
        bool        cube       = false;
    };

    /// One draw, as the device reads it off the engine.
    struct Draw
    {
        Stream    streams[kMaxStreams];
        uint32_t  streamCount = 0;
        Attribute attribs[kAttribCount];   ///< by EGxVertexAttrib; set where attribMask has the bit
        uint32_t  attribMask  = 0;

        bool     indexed     = false;
        VkBuffer indexBuffer = VK_NULL_HANDLE;   ///< 16-bit indices, bound at offset 0
        uint32_t firstIndex  = 0;
        uint32_t count       = 0;                ///< indices when indexed, vertices otherwise

        uint32_t topology = 3;       ///< EGxPrim
        uint32_t blend    = 0;       ///< EGxBlend

        /// The render states that shape the pipeline, as D3D9 would see them after the master
        /// enables: the depth test (its compare op by the engine's GxRs_DepthFunc value: 0
        /// LESS_OR_EQUAL, 1 EQUAL, 2 GREATER_OR_EQUAL, 3 LESS), the depth write, the culling (0
        /// none, 1 the clockwise face, 2 the counter-clockwise face), the colour write mask (R 1, G
        /// 2, B 4, A 8), the fill mode.
        bool     depthTest  = false;
        bool     depthWrite = false;
        uint32_t depthFunc  = 0;
        uint32_t cull       = 0;
        uint32_t colourMask = 0xF;
        bool     wireframe  = false;
        /// gxAlphaToCoverage: an alpha-tested opaque / alpha-key draw into the MSAA scene. The
        /// shaders see it as a negative alpha reference and sharpen their alpha around it.
        bool     alphaToCoverage = false;

        /// DECOMP: GxRs_Stencil*, 0 when the test is off: bit 0 on, the EGxStencilCmp at bit 1, the
        /// EGxStencilOp fail/zfail/pass at bits 5/9/13 (each 4 bits); masks = read | write << 8 | ref << 16.
        uint32_t stencil      = 0;
        uint32_t stencilMasks = 0;

        /// The dynamic states: D3D9's DEPTHBIAS, as Vulkan's constant depth bias factor (units of
        /// the depth buffer's smallest step); D3D9's BLENDFACTOR, for the ConstantAlpha blend.
        float depthBias        = 0.0f;
        float blendConstant[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        VertexProgram           vertex         = VertexProgram::FixedFunction;
        FragmentProgram         fragment       = FragmentProgram::FixedFunction;
        const manifest::Record* vertexRecord   = nullptr;   ///< with VertexProgram::Translated
        const manifest::Record* fragmentRecord = nullptr;   ///< with FragmentProgram::Translated

        /// The push constants (Shaders/Push.glsl): the transform (the row-major matrix, or c0..c3),
        /// the parameters (alpha reference, textured stages, colour ops, alpha ops) and the texture
        /// transforms of stages 0 and 1 (identity by default).
        float xform[16] = {};
        float params[4] = {};
        float tex[12]   = { 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };

        Texture  textures[kTextureStages];   ///< set where textureMask has the bit
        uint32_t textureMask = 0;

        /// The translated interface's constants: the vertex register file (vertexRows rows of four
        /// floats), the pixel register file (kPixelRows rows) and the misc row (alpha reference / 255,
        /// 0, colour op, alpha op). Read when either program is of the translated family.
        const float* vertexRegs = nullptr;
        uint32_t     vertexRows = kVertexRowsSmall;
        /// Bumped whenever the vertex register file changes (0: unknown, always compared): the
        /// slice skips its memcmp of the vertex rows when it matches the last slice's (audit P3).
        uint64_t     vertexRegsGen = 0;
        const float* pixelRegs  = nullptr;
        float        misc[4]    = {};

        /// The whole pixel register file (kPixelRowsMax rows) and the rows the pixel record reads
        /// (its `pconst`): rows kPixelRows.. come from here when pixelRows > kPixelRows.
        const float* pixelRegsFull = nullptr;
        uint32_t     pixelRows     = kPixelRows;

        VkViewport viewport = {};
        VkRect2D   scissor  = {};
    };

    /**
     * The shader modules, the descriptor set layouts, the pipeline cache, the per-slot descriptor
     * pools, the uniform ring and the constant stream. After memory::Init(), texture::Init() and
     * shaders::Init(). Idempotent.
     */
    bool Init();

    /// Everything, after the device is idle. Before shaders::Shutdown(), texture::Shutdown() and
    /// memory::Shutdown().
    void Shutdown();

    /**
     * Records one draw into an open render pass: the pipeline for the draw's key (built on first
     * use), the descriptor sets of its textures and constants, the vertex and index buffers, the
     * viewport and scissor, the push constants, the draw call. Binds only what changed since the
     * last draw of the same command buffer and frame.
     *
     * @param VkCommandBuffer cmd : the frame's main command buffer, its pass open
     * @param VkRenderPass pass : the pass it is open on (the frame's RenderPass())
     * @param uint32 passGeneration : frame::PassGeneration(); a change discards every pipeline
     * @param uint32 samples : frame::PassSamples(), the pass's sample count
     * @param uint64 serial : frame::Serial()
     * @param uint32 slot : frame::Slot(), which per-slot pools the frame uses
     * @return bool drawn : false when the draw could not be issued (logged once per reason)
     */
    bool Record(VkCommandBuffer cmd, VkRenderPass pass, uint32_t passGeneration, uint32_t samples, uint64_t serial, uint32_t slot, const Draw& draw);

    struct Stats
    {
        uint64_t drawn          = 0;
        uint64_t skipped        = 0;   ///< draws refused for any reason below or a missing position
        uint64_t refusedKeys    = 0;   ///< draws whose pipeline could not be built (logged once per reason)
        uint64_t cubeMismatch   = 0;   ///< draws whose texture is not the kind the sampler slot wants
        uint64_t pipelineBuilds = 0;
        uint32_t pipelines      = 0;   ///< live
        uint32_t samplers       = 0;   ///< live
        uint32_t layouts        = 0;   ///< distinct sampler-slot sets
        uint64_t descriptorSets = 0;   ///< set-0 sets allocated over the session
        uint64_t descriptorHits = 0;   ///< draws that found their set written this frame
        uint64_t descriptorFull = 0;   ///< draws refused because the frame's pool was exhausted
        uint64_t regsSlices     = 0;   ///< uniform slices written
        uint64_t regsReused     = 0;   ///< draws that bound the previous slice again
        uint64_t regsBytes      = 0;
        uint64_t regsFull       = 0;   ///< draws refused because the frame's uniform ring was exhausted
    };
    void GetStats(Stats& out);
}
