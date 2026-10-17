// The Vulkan backend's textures: a CGxTex gets a record behind CGxTex::m_apiSpecificData holding its
// VkImage, created and filled by the engine's own update protocol, and destroyed by frame serial.
//
// The protocol is CGxDeviceD3d::ITexMarkAsUpdated (0x006A3070): a pending texture (m_needsUpdate)
// with the context up is created (ITexCreate) when flagged for creation or without an API object,
// then filled (ITexUpload) when it has a user callback, then the base clears the pending rectangle.
// ITexUpload runs the callback as D3D does -- Lock once; Latch per face per mip level, the level's
// texels at the callback's pitch; Unlock once -- and where D3D locks a surface level and blits the
// bytes into it, the bytes of the update rectangle (shifted by the mip level, DXT-aligned, clamped,
// as D3D's rect arithmetic) go through the frame's staging ring into the image level, as one batch
// of copies per protocol run.
//
// The image's format is the texture's DATA format (what the callback delivers), so no conversion is
// needed: BC kept when the device has textureCompressionBC, decoded to RGBA8 when Vulkan 1.0 has no
// byte-identical format (Argb4444) or the device cannot sample the format. A render target gets an
// image of its storage format with COLOR_ATTACHMENT usage, born transparent black, which the frame
// draws into when the engine binds it (CGxDeviceVulkanTarget.cpp). D24X8 depth textures get no image.
//
// THE TRANSFER RULE: a fill of an image that nothing in this frame's main command buffer has named
// yet (no draw sampled it, no fill was recorded inline) is recorded into the frame's PRE-PASS buffer,
// submitted ahead of the main one, so every draw of the frame sees the new texels. A fill of an
// image a draw of this frame already sampled is recorded INLINE: the render pass is ended, the copy
// recorded, the pass resumed at the next draw. The glyph cache updated while text is drawn is the
// case the inline path exists for.
//
// GENERATED MIPS (CGxTexFlags::m_generateMipMaps, D3D9's AUTOGENMIPMAP): such a texture gets its
// full chain; the protocol's single level is uploaded as level 0 and the rest derived by a blit
// chain recorded under the same rule. A format that cannot be blitted keeps one level.
#include <cstdint>
#include <cstring>
#include <new>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/TexFormat.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"
#include "Engine/Source/Gx/CGxTex.h"

namespace {
    namespace context   = gxvk::vk::context;
    namespace frame     = gxvk::vk::frame;
    namespace texformat = gxvk::vk::texformat;
    namespace texture   = gxvk::vk::texture;
    namespace pipelines = gxvk::vk::pipelines;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Warn;

    constexpr uint32_t kCubeFaces  = 6;
    constexpr uint32_t kScratchMax = 64u * 1024u * 1024u;

    // Regions of one protocol run staged so far, all in one source buffer: 6 faces x 16 levels fit;
    // a fuller run is recorded and restarted.
    constexpr uint32_t kBatchMax = 128;

    struct TexRec {
        texture::Image image;                ///< VK_NULL_HANDLE when the texture cannot have one
        uint64_t       lastUsed     = 0;     ///< serial of the last frame a draw sampled it in
        uint64_t       lastRef      = 0;     ///< serial of the last frame a fill was recorded inline (the main buffer) in
        uint64_t       prePass      = 0;     ///< serial of the last frame a pre-pass fill named it in
        bool           genMips      = false; ///< a generated chain: level 0 uploaded, the rest derived
        uint32_t       start        = 0;     ///< the engine mip level that is image level 0
        uint32_t       dataFormat   = 0;     ///< EGxTexFormat the callback delivers (the storage format for a render target)
        bool           decode       = false; ///< the callback's texels are decoded to RGBA8 before the upload
        bool           renderTarget = false;
    };

    struct Region {
        uint32_t x;
        uint32_t y;
        uint32_t w;
        uint32_t h;
    };

    struct Batch {
        VkBuffer            src   = VK_NULL_HANDLE;
        uint32_t            count = 0;
        texture::RegionCopy regions[kBatchMax];
    };

    struct Stats {
        uint64_t records;
        uint64_t images;
        uint64_t noImage;         ///< records whose texture has no image (depth target, unknown format, a failed create)
        uint64_t renderTargets;
        uint64_t cubes;
        uint64_t decoded;         ///< records uploaded through the RGBA8 decoder
        uint64_t genMips;         ///< records with a generated chain
        uint64_t genMipsRefused;  ///< m_generateMipMaps on a format that cannot be blitted
        uint64_t uploads;         ///< protocol runs
        uint64_t regionsPrePass;
        uint64_t regionsInline;
        uint64_t batches;
        uint64_t chains;          ///< mip chains recorded
        uint64_t bytes;
        uint64_t refused;         ///< regions refused (no staging, outside the level, a decode failure)
        uint64_t reentered;       ///< updates asked for the texture whose fill is running
        uint64_t recreated;       ///< records dropped by ITexForceRecreation
    };
    Stats g_stats;

    uint8_t* g_scratch     = nullptr;
    uint32_t g_scratchSize = 0;

    TexRec*& RecOf(CGxTex* tex) {
        return reinterpret_cast<TexRec*&>(tex->m_apiSpecificData);
    }

    /// The last frame anything named the image: a draw, an inline fill or a pre-pass fill.
    uint64_t LastRef(const TexRec& r) {
        uint64_t last = r.lastUsed > r.lastRef ? r.lastUsed : r.lastRef;
        return r.prePass > last ? r.prePass : last;
    }

    /// The transfer rule: a fill goes inline when this frame's main buffer already named the image.
    bool MainNamed(const TexRec& r) {
        return frame::UsedThisFrame(r.lastUsed) || frame::UsedThisFrame(r.lastRef);
    }

    /// The record goes: its image retired by the last frame that named it.
    void DropRec(TexRec*& slot) {
        TexRec* r = slot;

        if (!r) {
            return;
        }

        frame::RetireImage(r->image, LastRef(*r));
        delete r;
        slot = nullptr;
    }

    /// Whether the device can sample a format from an optimally tiled image.
    bool Sampleable(VkFormat format) {
        const context::Handles* h = context::Get();

        if (!h || !h->iapi || !h->iapi->GetPhysicalDeviceFormatProperties) {
            return true;
        }

        VkFormatProperties props = {};
        h->iapi->GetPhysicalDeviceFormatProperties(h->physical, format, &props);
        return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    }

    /// Levels of a full chain down to 1x1: floor(log2(max(w, h))) + 1.
    uint32_t FullChain(uint32_t w, uint32_t h) {
        uint32_t levels = 1;

        for (uint32_t side = w > h ? w : h; side > 1; side >>= 1) {
            levels++;
        }

        return levels;
    }

    uint8_t* Scratch(uint64_t bytes) {
        if (bytes > kScratchMax) {
            return nullptr;
        }

        if (bytes <= g_scratchSize) {
            return g_scratch;
        }

        uint32_t want = g_scratchSize ? g_scratchSize : 256u * 1024u;

        while (want < bytes) {
            want *= 2;
        }

        uint8_t* fresh = new (std::nothrow) uint8_t[want];

        if (!fresh) {
            return nullptr;
        }

        delete[] g_scratch;
        g_scratch = fresh;
        g_scratchSize = want;
        return g_scratch;
    }

    /// The sampler key of a texture's flags: filter (bits 0-2), wrap U/V (3, 4), max anisotropy (9-13).
    uint32_t SamplerKey(const CGxTexFlags& flags) {
        uint32_t bits;
        memcpy(&bits, &flags, sizeof(bits));
        return bits & 0x3E1Fu;
    }

    // CGxDeviceD3d::ITexUpload's rectangle of one mip level: the update rect shifted by the level,
    // at least one texel wide and high, aligned out to the 4x4 block grid for a DXT format, clamped
    // to the level. False for an empty result.
    bool ComputeRegion(const CGxTex* tex, uint32_t mip, uint32_t levelW, uint32_t levelH, const texformat::Info& info, Region& out) {
        if (levelW == 0 || levelH == 0) {
            return false;
        }

        const CiRect& rect = tex->m_updateRect;

        if (rect.minX < 0 || rect.minY < 0 || rect.maxX <= rect.minX || rect.maxY <= rect.minY) {
            return false;
        }

        const uint32_t shift = mip & 31u;
        uint32_t x0 = static_cast<uint32_t>(rect.minX) >> shift;
        uint32_t y0 = static_cast<uint32_t>(rect.minY) >> shift;
        uint32_t x1 = static_cast<uint32_t>(rect.maxX) >> shift;
        uint32_t y1 = static_cast<uint32_t>(rect.maxY) >> shift;

        if (x1 < x0 + 1) {
            x1 = x0 + 1;
        }

        if (y1 < y0 + 1) {
            y1 = y0 + 1;
        }

        if (info.compressed) {
            const uint32_t bw = info.blockWidth ? info.blockWidth : 1u;
            const uint32_t bh = info.blockHeight ? info.blockHeight : 1u;
            x0 -= x0 % bw;
            y0 -= y0 % bh;
            x1 = (x1 + bw - 1) / bw * bw;
            y1 = (y1 + bh - 1) / bh * bh;
        }

        if (x1 > levelW) {
            x1 = levelW;
        }

        if (y1 > levelH) {
            y1 = levelH;
        }

        if (x0 >= x1 || y0 >= y1) {
            return false;
        }

        out = Region { x0, y0, x1 - x0, y1 - y0 };
        return true;
    }

    // The region's first row in the callback's texels: offset by the region's origin unless
    // CGxTexFlags::m_bit15 says the texels already start at the update rectangle.
    const uint8_t* SourceAt(const CGxTexFlags& flags, const Region& g, const void* texels, uint32_t pitch, const texformat::Info& info) {
        const uint8_t* src = static_cast<const uint8_t*>(texels);

        if (flags.m_bit15) {
            return src;
        }

        const uint32_t bw = info.blockWidth ? info.blockWidth : 1u;
        const uint32_t bh = info.blockHeight ? info.blockHeight : 1u;
        return src + static_cast<size_t>(g.y / bh) * pitch + static_cast<size_t>(g.x / bw) * info.blockBytes;
    }

    /// Records the batch under the transfer rule and empties it.
    void RecordBatch(TexRec& r, Batch& b) {
        if (b.count == 0) {
            return;
        }

        const bool inl = MainNamed(r);
        VkCommandBuffer cmd = inl ? frame::Outside() : frame::PrePass();
        const uint32_t n = cmd ? texture::RecordRegions(cmd, r.image, b.src, b.regions, b.count) : 0u;

        if (n) {
            g_stats.batches++;

            if (inl) {
                g_stats.regionsInline += n;
                r.lastRef = frame::Serial();
            } else {
                g_stats.regionsPrePass += n;
                r.prePass = frame::Serial();
            }
        }

        g_stats.refused += b.count - n;
        b.count = 0;
        b.src = VK_NULL_HANDLE;
    }

    /// One region's rows staged into the frame's ring, tightly packed, and added to the batch.
    void StageRegion(TexRec& r, Batch& b, const Region& g, uint32_t level, uint32_t face, const uint8_t* rows, uint32_t rowPitch) {
        uint64_t rowBytes = 0;
        uint32_t rowCount = 0;

        if (!texture::RegionBytes(r.image, g.w, g.h, rowBytes, rowCount) || rowBytes > rowPitch) {
            g_stats.refused++;
            return;
        }

        const VkDeviceSize total = rowBytes * rowCount;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceSize off = 0;
        void* dst = nullptr;

        if (!frame::Stage(total, buf, off, dst)) {
            g_stats.refused++;
            return;
        }

        uint8_t* out = static_cast<uint8_t*>(dst);

        if (rowBytes == rowPitch) {
            memcpy(out, rows, static_cast<size_t>(total));
        } else {
            for (uint32_t i = 0; i < rowCount; i++) {
                memcpy(out + static_cast<size_t>(i) * static_cast<size_t>(rowBytes), rows + static_cast<size_t>(i) * rowPitch, static_cast<size_t>(rowBytes));
            }
        }

        g_stats.bytes += total;

        if (b.count && (b.src != buf || b.count == kBatchMax)) {
            RecordBatch(r, b);
        }

        b.src = buf;
        texture::RegionCopy& c = b.regions[b.count++];
        c.level = level;
        c.x = g.x;
        c.y = g.y;
        c.width = g.w;
        c.height = g.h;
        c.layer = face;
        c.srcOffset = off;
    }
}

// Mirrors CGxDeviceD3d::ITexMarkAsUpdated (0x006A3070), with the image record as the API object.
// Without the draw path the texture stays pending, as D3D leaves it without a context, and is made
// at its first bind once the path is up.
void CGxDeviceVulkan::ITexMarkAsUpdated(CGxTex* texId) {
    if (!texId->m_needsUpdate || !this->m_context || !this->m_drawPath) {
        return;
    }

    if (texId == this->m_filling) {
        g_stats.reentered++;
        return;
    }

    if (texId->m_needsCreation || !texId->m_apiSpecificData) {
        this->ITexCreate(texId);
    }

    if (!texId->m_needsCreation && texId->m_apiSpecificData) {
        if (texId->m_userFunc) {
            CGxTex* outer = this->m_filling;
            this->m_filling = texId;
            this->ITexUpload(texId);
            this->m_filling = outer;
        }

        CGxDevice::ITexMarkAsUpdated(texId);
    }
}

// Mirrors CGxDeviceD3d::ITexCreate (0x006A2C00): the image for the size and level range
// ITexWHDStartEnd gives, in the data format the callback delivers. The record exists whether or not
// an image could be made, so the engine's TexIsReadyNow holds and a texture without an image draws
// untextured rather than being retried every bind.
void CGxDeviceVulkan::ITexCreate(CGxTex* texId) {
    TexRec*& slot = RecOf(texId);
    DropRec(slot);

    TexRec* r = new (std::nothrow) TexRec();

    if (!r) {
        return;
    }

    slot = r;
    texId->m_needsCreation = 0;
    g_stats.records++;

    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    r->renderTarget = texId->m_flags.m_renderTarget != 0;
    r->start = baseMip;
    r->dataFormat = r->renderTarget ? texId->m_format : texId->m_dataFormat;

    const bool cube = texId->m_target == GxTex_CubeMap;
    const texformat::Info& info = texformat::Of(r->dataFormat);
    const context::Handles* ctx = context::Get();

    if (r->renderTarget) {
        g_stats.renderTargets++;
    }

    if (!ctx || width == 0 || height == 0 || mipCount <= baseMip || r->dataFormat == GxTex_D24X8 || !info.uploadable) {
        g_stats.noImage++;
        return;
    }

    r->decode = info.needsDecode || (info.compressed && !ctx->textureCompressionBC);

    if (!r->decode && !r->renderTarget && info.format != VK_FORMAT_UNDEFINED && !Sampleable(info.format)) {
        r->decode = true;
    }

    const VkFormat format = r->decode ? VK_FORMAT_R8G8B8A8_UNORM : info.format;

    if (format == VK_FORMAT_UNDEFINED) {
        g_stats.noImage++;
        return;
    }

    uint32_t levels = mipCount - baseMip;

    if (texId->m_flags.m_generateMipMaps && !r->renderTarget) {
        if (texture::CanGenerateMips(format)) {
            const uint32_t full = FullChain(width, height);

            if (full > levels) {
                levels = full;
            }

            r->genMips = levels > 1;

            if (r->genMips) {
                g_stats.genMips++;
            }
        } else {
            g_stats.genMipsRefused++;
        }
    }

    bool made = false;

    if (r->renderTarget) {
        // A format the device cannot render into keeps a sampled-only image.
        made = texture::CreateTarget(width, height, levels, cube, format, r->image);
    }

    if (!made) {
        made = cube ? (width == height && texture::CreateCube(width, levels, format, r->image)) : texture::CreateMipped(width, height, levels, format, r->image);
    }

    if (!made) {
        r->image = texture::Image();
        g_stats.noImage++;
        return;
    }

    if (cube) {
        g_stats.cubes++;
    }

    if (r->decode) {
        g_stats.decoded++;
    }

    // Born with known contents in every level and face, through the pre-pass (nothing of this frame
    // named a new image): a level the engine never uploads samples as a colour, not as VRAM. A render
    // target is born transparent black, so a target sampled before anything drew into it adds nothing.
    static const float kTargetBorn[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    VkCommandBuffer pre = frame::PrePass();

    if (pre && texture::RecordClear(pre, r->image, r->renderTarget ? kTargetBorn : nullptr)) {
        r->prePass = frame::Serial();
    }

    g_stats.images++;
}

// Mirrors CGxDeviceD3d::ITexUpload (0x006A2D80): the callback protocol, with the texels of each
// level's update rectangle staged and recorded into the image instead of blitted into a locked
// surface. A generated chain takes level 0 alone from the callback, as D3D's AUTOGENMIPMAP texture
// can only be locked at level 0, and derives the rest.
void CGxDeviceVulkan::ITexUpload(CGxTex* texId) {
    TexRec& r = *RecOf(texId);
    g_stats.uploads++;

    uint32_t texelStrideInBytes = 0;
    const void* texels = nullptr;
    texId->m_userFunc(GxTex_Lock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    const uint32_t faceCount = texId->m_target == GxTex_CubeMap ? kCubeFaces : 1u;
    const texformat::Info& info = texformat::Of(r.dataFormat);
    const bool usable = r.image.handle != VK_NULL_HANDLE && !r.renderTarget && baseMip == r.start;

    bool levelZero = false;
    Batch batch;

    for (uint32_t face = 0; face < faceCount; face++) {
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;

        for (uint32_t mip = baseMip; mip != mipCount; mip++) {
            texels = nullptr;
            texId->m_userFunc(GxTex_Latch, mipWidth, mipHeight, face, mip, texId->m_userArg, texelStrideInBytes, texels);

            const uint32_t level = mip - baseMip;
            Region g;

            if (texels && usable && face < r.image.layers && level < r.image.levels && (!r.genMips || level == 0) && ComputeRegion(texId, mip, mipWidth, mipHeight, info, g)) {
                const uint8_t* src = SourceAt(texId->m_flags, g, texels, texelStrideInBytes, info);

                if (r.decode) {
                    uint8_t* rgba = Scratch(static_cast<uint64_t>(g.w) * g.h * 4u);

                    if (rgba && texformat::DecodeToRgba8(r.dataFormat, g.w, g.h, src, texelStrideInBytes, rgba)) {
                        StageRegion(r, batch, g, level, face, rgba, g.w * 4u);
                    } else {
                        g_stats.refused++;
                    }
                } else {
                    StageRegion(r, batch, g, level, face, src, texelStrideInBytes);
                }

                if (level == 0) {
                    levelZero = true;
                }
            }

            mipHeight >>= 1;
            mipWidth >>= 1;

            if (!mipWidth) {
                mipWidth = 1;
            }

            if (!mipHeight) {
                mipHeight = 1;
            }
        }
    }

    texels = nullptr;
    texId->m_userFunc(GxTex_Unlock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    // Before the chain, which derives from level 0.
    RecordBatch(r, batch);

    if (r.genMips && levelZero) {
        const bool inl = MainNamed(r);
        VkCommandBuffer cmd = inl ? frame::Outside() : frame::PrePass();

        if (cmd && texture::RecordGenerateMips(cmd, r.image)) {
            (inl ? r.lastRef : r.prePass) = frame::Serial();
            g_stats.chains++;
        } else {
            g_stats.refused++;
        }
    }
}

// Mirrors CGxDeviceD3d::ITexForceRecreation (0x006A2AA0): every texture with an image (every
// render target, with `all` every texture) loses it -- retired by the last frame that named it --
// and is marked for creation and a whole update, its callback told the texels were dropped
// (GxTex_3); the next bind makes it again. Then the engine's texture-recreation callbacks.
void CGxDeviceVulkan::ITexForceRecreation(int32_t all) {
    for (CGxTex* texId = this->m_textures.Head(); texId; texId = this->m_textures.Next(texId)) {
        if (!texId->m_apiSpecificData || !(all || texId->m_flags.m_renderTarget)) {
            continue;
        }

        DropRec(RecOf(texId));
        texId->m_needsCreation = 1;

        CiRect whole;
        this->TexMarkForUpdate(texId, whole, 0);

        if (texId->m_userFunc) {
            uint32_t texelStrideInBytes;
            const void* texels;
            texId->m_userFunc(GxTex_3, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
        }

        g_stats.recreated++;
    }

    if (this->m_hwnd && context::Live()) {
        this->NotifyOnTextureRecreation();
    }
}

// Mirrors CGxDeviceD3d::DeviceSetBaseMipLevel (0x0068E4A0): every texture made again at the new
// base level, which ITexWHDStartEnd reads from the base's field.
void CGxDeviceVulkan::DeviceSetBaseMipLevel(uint32_t level) {
    this->ITexForceRecreation(1);
    CGxDevice::DeviceSetBaseMipLevel(level);
}

// Mirrors CGxDeviceD3d::TexDestroy (0x006A2BB0): the image retired by serial (the frame that last
// sampled it may still be on the GPU), then the base unlinks and frees the CGxTex.
void CGxDeviceVulkan::TexDestroy(CGxTex* texId) {
    DropRec(RecOf(texId));
    CGxDevice::TexDestroy(texId);
}

// One texture stage of a draw: the bind-time update CGxDeviceD3d::ISetTexture (0x006A4900)
// performs, then the image and sampler key into the draw's stage, the image stamped with this
// frame. False when the texture has no image, or is the target the draw lands on (an image cannot
// be sampled while it is the attachment): the stage stays unbound.
int32_t CGxDeviceVulkan::ITexResolve(CGxTex* texId, uint32_t stage, pipelines::Draw& draw) {
    if (!texId || stage >= pipelines::kTextureStages) {
        return 0;
    }

    this->ITexMarkAsUpdated(texId);

    TexRec* r = RecOf(texId);

    if (!r || r->image.handle == VK_NULL_HANDLE || &r->image == frame::Target()) {
        return 0;
    }

    r->lastUsed = frame::Serial();
    pipelines::Texture& t = draw.textures[stage];
    t.view = r->image.view;
    t.imageId = r->image.id;
    t.samplerKey = SamplerKey(texId->m_flags);
    t.cube = r->image.cube;
    draw.textureMask |= 1u << stage;
    return 1;
}

// VFOG: not in 12340. CGxDevice::DeviceCopySceneDepth: the scene's depth buffer copied into a
// device-owned CGxTex (DEPTH-aspect view, nearest, clamp), bound like any texture through
// GxRs_TextureN; sampled .r = window-space depth. The CGxTex is a D24X8 texture, so the engine's
// protocol gives it a record with no image (ITexCreate); the image is made here, at the scene's
// size in the frame's depth format, and remade on a size change. A record dropped by
// ITexForceRecreation or IReleaseTextures is remade at the next call. Null while the scene's depth
// cannot be had (no frame, no pass wrote it, a query pins the pass, the format cannot be sampled).
namespace {
    CGxTex*          g_sceneDepth      = nullptr;
    CGxDeviceVulkan* g_sceneDepthOwner = nullptr;
    VkFormat         g_sceneDepthRefused = VK_FORMAT_UNDEFINED;   ///< CreateDepthCopy failed for it: not retried (logged once)
}

CGxTex* CGxDeviceVulkan::DeviceCopySceneDepth() {
    if (!this->m_context || !this->m_drawPath) {
        return nullptr;
    }

    const texture::Image* scene = frame::Scene();
    const VkFormat format = frame::DepthFormat();

    if (!scene || format == VK_FORMAT_UNDEFINED || format == g_sceneDepthRefused) {
        return nullptr;
    }

    const uint32_t width = scene->extent.width;
    const uint32_t height = scene->extent.height;

    if (g_sceneDepthOwner != this) {
        g_sceneDepth = nullptr;
        g_sceneDepthOwner = this;
    }

    if (!g_sceneDepth) {
        // Nearest, clamped, one level: a depth texel is never filtered.
        CGxTexFlags flags(GxTex_Nearest);

        if (!this->TexCreate(GxTex_2d, width, height, 0, GxTex_D24X8, GxTex_D24X8, flags, nullptr, nullptr, "VFogSceneDepth", g_sceneDepth)) {
            g_sceneDepth = nullptr;
            return nullptr;
        }
    }

    CGxTex* tex = g_sceneDepth;

    // The record (no image of its own: D24X8), made by the engine's protocol.
    this->ITexMarkAsUpdated(tex);
    TexRec* r = RecOf(tex);

    if (!r) {
        return nullptr;
    }

    if (r->image.handle != VK_NULL_HANDLE && (r->image.extent.width != width || r->image.extent.height != height || r->image.format != format)) {
        frame::RetireImage(r->image, LastRef(*r));
    }

    if (r->image.handle == VK_NULL_HANDLE) {
        tex->m_width = width;
        tex->m_height = height;

        if (!texture::CreateDepthCopy(width, height, format, r->image)) {
            r->image = texture::Image();
            // ponytail: a refused format (not sampleable) disables vfog for the session; a size
            // the device refuses would too, which no back buffer reaches.
            g_sceneDepthRefused = format;
            Write(Warn, "vulkan: VFOG: no sampled copy of the %ux%u scene depth; DeviceCopySceneDepth returns null", width, height);
            return nullptr;
        }
    }

    if (!frame::CopyDepthFromScene(r->image)) {
        return nullptr;
    }

    r->lastRef = frame::Serial();
    return tex;
}

// The texture's image, made now if the texture is pending (CGxDeviceD3d::DeviceSetRenderTarget
// creates the texture it is handed). Null when the texture has none.
texture::Image* CGxDeviceVulkan::ITexImage(CGxTex* texId) {
    if (!texId || !this->m_context) {
        return nullptr;
    }

    if (texId->m_needsCreation || !texId->m_apiSpecificData) {
        this->ITexCreate(texId);
    }

    TexRec* r = RecOf(texId);

    if (!r || r->image.handle == VK_NULL_HANDLE) {
        return nullptr;
    }

    return &r->image;
}

// The texture's image is named by this frame's main command buffer (drawn into, copied into): its
// retirement waits for the frame.
void CGxDeviceVulkan::ITexTouch(CGxTex* texId) {
    TexRec* r = texId ? RecOf(texId) : nullptr;

    if (r && r->image.handle != VK_NULL_HANDLE) {
        r->lastRef = frame::Serial();
    }
}

// Every texture's record and image, destroyed at once: the device is idle (the caller waited for
// it) or gone. The textures themselves stay the engine's, flagged for creation should a device
// come up again.
void CGxDeviceVulkan::IReleaseTextures() {
    for (CGxTex* texId = this->m_textures.Head(); texId; texId = this->m_textures.Next(texId)) {
        TexRec* r = RecOf(texId);

        if (!r) {
            continue;
        }

        if (context::Live()) {
            frame::ForgetImage(r->image);
            texture::DestroyIdle(r->image);
        }

        delete r;
        RecOf(texId) = nullptr;
        texId->m_needsCreation = 1;
    }

    delete[] g_scratch;
    g_scratch = nullptr;
    g_scratchSize = 0;

    texture::Stats t;
    texture::GetStats(t);
    Write(Debug, "vulkan: textures: %llu record(s), %llu image(s) (%llu without one), %llu render target(s), %llu cube(s), %llu decoded, %llu with a generated chain (%llu refused); %llu upload(s): %llu region(s) ahead of the frame, %llu inline, %llu batch(es), %llu chain(s), %llu KiB staged, %llu refused; %llu re-entered, %llu recreated; images made %llu / destroyed %llu, peak %u, %llu MiB asked",
          static_cast<unsigned long long>(g_stats.records), static_cast<unsigned long long>(g_stats.images),
          static_cast<unsigned long long>(g_stats.noImage), static_cast<unsigned long long>(g_stats.renderTargets),
          static_cast<unsigned long long>(g_stats.cubes), static_cast<unsigned long long>(g_stats.decoded),
          static_cast<unsigned long long>(g_stats.genMips), static_cast<unsigned long long>(g_stats.genMipsRefused),
          static_cast<unsigned long long>(g_stats.uploads), static_cast<unsigned long long>(g_stats.regionsPrePass),
          static_cast<unsigned long long>(g_stats.regionsInline), static_cast<unsigned long long>(g_stats.batches),
          static_cast<unsigned long long>(g_stats.chains), static_cast<unsigned long long>(g_stats.bytes / 1024),
          static_cast<unsigned long long>(g_stats.refused), static_cast<unsigned long long>(g_stats.reentered),
          static_cast<unsigned long long>(g_stats.recreated), static_cast<unsigned long long>(t.created),
          static_cast<unsigned long long>(t.destroyed), t.livePeak, static_cast<unsigned long long>(t.bytes >> 20));
}
