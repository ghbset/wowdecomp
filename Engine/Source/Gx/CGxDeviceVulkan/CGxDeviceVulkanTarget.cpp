// The Vulkan backend's render targets: the engine's render-target textures as the frame's targets,
// the copies off the back buffer into a texture (TexCopy, TexStretch) and the back buffer's pixels
// for a screenshot (DeviceReadPixels), all against the frame's scene image, which is the engine's
// back buffer here.
#include <cstdint>
#include <cstring>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"

namespace frame   = gxvk::vk::frame;
namespace texture = gxvk::vk::texture;

namespace {
    /// A rectangle clipped to an extent; false when nothing is left.
    bool ClipRect(int32_t minX, int32_t minY, int32_t maxX, int32_t maxY, VkExtent2D extent, VkRect2D& out) {
        if (minX < 0) {
            minX = 0;
        }

        if (minY < 0) {
            minY = 0;
        }

        if (maxX > static_cast<int32_t>(extent.width)) {
            maxX = static_cast<int32_t>(extent.width);
        }

        if (maxY > static_cast<int32_t>(extent.height)) {
            maxY = static_cast<int32_t>(extent.height);
        }

        if (maxX <= minX || maxY <= minY) {
            return false;
        }

        out.offset.x = minX;
        out.offset.y = minY;
        out.extent.width = static_cast<uint32_t>(maxX - minX);
        out.extent.height = static_cast<uint32_t>(maxY - minY);
        return true;
    }

    /// The extent of one mip level of an image.
    VkExtent2D LevelExtent(const texture::Image& image, uint32_t level) {
        VkExtent2D e;
        e.width = image.extent.width >> level;
        e.height = image.extent.height >> level;

        if (!e.width) {
            e.width = 1;
        }

        if (!e.height) {
            e.height = 1;
        }

        return e;
    }
}

// The image and layer behind a texture bound as a target or copied into: a cube map's face is its
// array layer. False when the texture has no image.
int32_t CGxDeviceVulkan::ITargetImage(CGxTex* texId, texture::Image*& image, uint32_t plane, uint32_t& layer) {
    image = this->ITexImage(texId);

    if (!image) {
        return 0;
    }

    layer = texId->m_target == GxTex_CubeMap ? plane : 0u;
    return layer < image->layers;
}

// Mirrors CGxDeviceD3d::DeviceSetRenderTarget (0x0068F770): nothing when the target is unchanged;
// the base's record; the texture made if it is pending; then the frame's target -- the colour
// texture's image (one face of a cube map), or the scene when none is bound. The frame pairs every
// target with a depth buffer of its size, as D3D makes a depth-stencil surface for a colour target
// without a depth texture; a depth texture bound alone (D24X8, which gets no image) leaves the
// scene as the colour target. Then the viewport is re-placed for the new target's size.
void CGxDeviceVulkan::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    if (!this->m_context) {
        return;
    }

    CGxDevice::TextureTarget& target = this->m_textureTarget[buffer];

    if (target.m_texture == texture && target.m_plane == plane) {
        return;
    }

    CGxDevice::DeviceSetRenderTarget(buffer, texture, plane);

    if (texture) {
        this->ITexImage(texture);
    }

    const CGxDevice::TextureTarget& colour = this->m_textureTarget[GxBuffers_Color];
    texture::Image* image = nullptr;
    uint32_t layer = 0;

    if (colour.m_texture && this->ITargetImage(colour.m_texture, image, colour.m_plane, layer)) {
        // RTT MIPS: a non-cube plane is the mip level (Gx.h GxRenderTargetSet).
        frame::SetTarget(image, layer, colour.m_texture->m_target == GxTex_CubeMap ? 0u : colour.m_plane);
        this->ITexTouch(colour.m_texture);
    } else {
        frame::SetTarget(nullptr, 0);
    }

    GxXformSetViewport(this->m_viewport.m_x.m_l, this->m_viewport.m_x.m_h, this->m_viewport.m_y.m_l, this->m_viewport.m_y.m_h, this->m_viewport.m_z.m_l, this->m_viewport.m_z.m_h);

    this->m_viewportDirty = 1;
    this->m_scissorRectDirty = 1;
}

// Mirrors CGxDeviceD3d::TexCopy (0x006A30D0): D3D's StretchRect from the back buffer into one level
// and face of the texture, same-size rectangles, no filtering.
int32_t CGxDeviceVulkan::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    if (!this->m_context || !dest) {
        return 0;
    }

    texture::Image* image = nullptr;
    uint32_t layer = 0;

    if (!this->ITargetImage(dest, image, plane, layer) || level >= image->levels) {
        return 0;
    }

    const texture::Image* scene = frame::Scene();

    if (!scene) {
        return 0;
    }

    VkRect2D src;
    VkRect2D dst;

    if (!ClipRect(sourcePos.x, sourcePos.y, sourcePos.x + size.x, sourcePos.y + size.y, scene->extent, src)
        || !ClipRect(destPos.x, destPos.y, destPos.x + static_cast<int32_t>(src.extent.width), destPos.y + static_cast<int32_t>(src.extent.height), LevelExtent(*image, level), dst)) {
        return 0;
    }

    // The source shrinks with the destination so the copy stays 1:1.
    src.extent = dst.extent;
    this->ITexTouch(dest);
    return frame::CopyFromScene(*image, layer, level, src, dst, false);
}

// Mirrors CGxDeviceD3d::TexStretch (0x006A31E0): D3D's StretchRect from the back buffer into one
// level and face of the texture; a null rectangle means the whole surface; linear filtering unless
// the two rectangles are the same size.
int32_t CGxDeviceVulkan::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    if (!this->m_context || !dest) {
        return 0;
    }

    texture::Image* image = nullptr;
    uint32_t layer = 0;

    if (!this->ITargetImage(dest, image, face, layer) || level >= image->levels) {
        return 0;
    }

    const texture::Image* scene = frame::Scene();

    if (!scene) {
        return 0;
    }

    const VkExtent2D levelExtent = LevelExtent(*image, level);
    VkRect2D src;
    VkRect2D dst;

    if (sourceRect) {
        if (!ClipRect(sourceRect->minX, sourceRect->minY, sourceRect->maxX, sourceRect->maxY, scene->extent, src)) {
            return 0;
        }
    } else {
        src.offset.x = 0;
        src.offset.y = 0;
        src.extent = scene->extent;
    }

    if (destRect) {
        if (!ClipRect(destRect->minX, destRect->minY, destRect->maxX, destRect->maxY, levelExtent, dst)) {
            return 0;
        }
    } else {
        dst.offset.x = 0;
        dst.offset.y = 0;
        dst.extent = levelExtent;
    }

    const bool linear = !(destRect && sourceRect && dst.extent.width == src.extent.width && dst.extent.height == src.extent.height);
    this->ITexTouch(dest);
    return frame::CopyFromScene(*image, layer, level, src, dst, linear);
}

// Mirrors CGxDeviceD3d::DeviceReadPixels (0x0068FED0): the rectangle clamped to the window, the
// back buffer's pixels read back as the engine's CImVector (b, g, r, a bytes: the scene's B8G8R8A8
// texels as they lie). Black when the read-back cannot be done.
void CGxDeviceVulkan::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    this->ClampRectToWindow(rect);

    const int32_t width = rect.maxX - rect.minX;
    const int32_t height = rect.maxY - rect.minY;

    if (width <= 0 || height <= 0) {
        pixels.SetCount(0);
        return;
    }

    const uint32_t count = static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
    pixels.SetCount(count);
    memset(pixels.Ptr(), 0, count * sizeof(CImVector));

    const texture::Image* scene = frame::Scene();

    if (!this->m_context || !scene) {
        return;
    }

    VkRect2D area;

    if (!ClipRect(rect.minX, rect.minY, rect.maxX, rect.maxY, scene->extent, area) || area.extent.width != static_cast<uint32_t>(width) || area.extent.height != static_cast<uint32_t>(height)) {
        return;
    }

    frame::ReadScene(area, reinterpret_cast<uint8_t*>(pixels.Ptr()));
}
