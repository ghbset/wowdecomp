// The Vulkan backend's CGxCaps: what D3D9 reports on a modern adapter, with the adapter's own limits
// where Vulkan can answer, and what this device does not do turned off so the engine takes no path
// that needs it.
#include <cstdint>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"

namespace context = gxvk::vk::context;

namespace {
    /// True when the format can be a sampled colour attachment with optimal tiling, the way D3D's
    /// CheckDeviceFormat(RENDERTARGET, TEXTURE) asks.
    bool RenderableColour(const context::Handles& h, VkFormat format) {
        VkFormatProperties props = {};
        h.iapi->GetPhysicalDeviceFormatProperties(h.physical, format, &props);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        return (props.optimalTilingFeatures & need) == need;
    }
}

// The counterpart of CGxDeviceD3d::ISetCaps (0x0068EE20). The constants are what that function
// derives from D3DCAPS9 on an RTX-class adapter; the rows Vulkan can answer are asked of the adapter.
void CGxDeviceVulkan::ISetCaps(const CGxFormat& format) {
    const context::Handles* h = context::Get();
    VkPhysicalDeviceProperties props = {};

    if (h) {
        h->iapi->GetPhysicalDeviceProperties(h->physical, &props);
    }

    this->m_caps.m_numTmus = 8;
    this->m_caps.m_pixelCenterOnEdge = 0;
    this->m_caps.m_texelCenterOnEdge = 1;
    this->m_caps.m_numStreams = 16;
    // The draw binds every vertex stream at its buf's byte offset in the pool (D3D's stream offset
    // capability), so the base vertex index is always 0.
    this->m_caps.m_streamOffset = 1;
    this->m_caps.m_maxIndex = 0xFFFFFF;
    this->m_caps.m_generateMipMaps = 1;

    // Texture formats 1-12 all offered, as D3D offers them on this class of adapter.
    this->m_caps.m_texFmt[0] = 0;

    for (uint32_t i = 1; i < GxTexCapsFormats; i++) {
        this->m_caps.m_texFmt[i] = 1;
    }

    this->m_caps.m_texTarget[GxTex_2d] = 1;
    this->m_caps.m_texTarget[GxTex_CubeMap] = 1;
    this->m_caps.m_texTarget[GxTex_Rectangle] = 0;
    this->m_caps.m_texTarget[GxTex_NonPow2] = 1;
    this->m_caps.m_texNonPow2 = 0;

    // Texture sizes: D3D's 16384, capped by what a VkImage can be on this adapter.
    uint32_t max2D = 16384;
    uint32_t maxCube = 16384;

    if (h) {
        if (props.limits.maxImageDimension2D < max2D) {
            max2D = props.limits.maxImageDimension2D;
        }

        if (props.limits.maxImageDimensionCube < maxCube) {
            maxCube = props.limits.maxImageDimensionCube;
        }
    }

    this->m_caps.m_texMaxSize[GxTex_2d] = max2D;
    this->m_caps.m_texMaxSize[GxTex_CubeMap] = maxCube;
    this->m_caps.m_texMaxSize[GxTex_Rectangle] = max2D;
    this->m_caps.m_texMaxSize[GxTex_NonPow2] = max2D;

    // Render-target formats: what a VkImage of the matching format can be, asked of the adapter the
    // way D3D asks CheckDeviceFormat. Argb8888 is stored B8G8R8A8 (the back buffer's own), Rgb565
    // R5G6B5, Gr1616F R16G16_SFLOAT, R32F R32_SFLOAT. D24X8 is not offered: every colour target gets
    // the frame's own depth buffer, and a depth texture cannot be sampled here, so the shadow cache
    // takes its R32F colour-target path (its PCF path wants the D24X8 texture).
    for (uint32_t i = 0; i < GxTexCapsFormats; i++) {
        this->m_caps.m_rttFormat[i] = 0;
    }

    if (h) {
        this->m_caps.m_rttFormat[GxTex_Argb8888] = RenderableColour(*h, VK_FORMAT_B8G8R8A8_UNORM);
        this->m_caps.m_rttFormat[GxTex_Rgb565] = RenderableColour(*h, VK_FORMAT_R5G6B5_UNORM_PACK16);
        this->m_caps.m_rttFormat[GxTex_Gr1616F] = RenderableColour(*h, VK_FORMAT_R16G16_SFLOAT);
        this->m_caps.m_rttFormat[GxTex_R32F] = RenderableColour(*h, VK_FORMAT_R32_SFLOAT);
        this->m_caps.m_rttAbgr16161616F = RenderableColour(*h, VK_FORMAT_R16G16B16A16_SFLOAT);
    } else {
        this->m_caps.m_rttFormat[GxTex_Argb8888] = 1;
        this->m_caps.m_rttFormat[GxTex_Rgb565] = 1;
        this->m_caps.m_rttFormat[GxTex_Gr1616F] = 1;
        this->m_caps.m_rttFormat[GxTex_R32F] = 1;
        this->m_caps.m_rttAbgr16161616F = 1;
    }

    this->m_caps.m_rttFormat[GxTex_D24X8] = 0;

    // Shader levels: vs_3_0 and ps_3_0, the profiles the client runs on D3D9, so the engine loads
    // the vs_3_0 / ps_3_0 .bls records. The format's fixed-function caps lower a level as on D3D.
    for (uint32_t i = 0; i < GxShTargets_Last; i++) {
        this->m_caps.m_shaderTargets[i] = 0;
        this->m_caps.m_shaderConstants[i] = 0;
    }

    this->m_caps.m_shaderTargets[GxSh_Pixel] = 4;
    this->m_caps.m_shaderTargets[GxSh_Vertex] = 3;
    this->m_caps.m_shaderConstants[GxSh_Vertex] = 256;

    if (format.m_fixedFunction[GxSh_Pixel] != -1 && format.m_fixedFunction[GxSh_Pixel] <= this->m_caps.m_shaderTargets[GxSh_Pixel]) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = format.m_fixedFunction[GxSh_Pixel];
    }

    if (format.m_fixedFunction[GxSh_Vertex] != -1 && format.m_fixedFunction[GxSh_Vertex] <= this->m_caps.m_shaderTargets[GxSh_Vertex]) {
        this->m_caps.m_shaderTargets[GxSh_Vertex] = format.m_fixedFunction[GxSh_Vertex];
    }

    if (this->m_caps.m_shaderTargets[GxSh_Pixel] == 4 && this->m_caps.m_shaderTargets[GxSh_Vertex] != 3) {
        this->m_caps.m_shaderTargets[GxSh_Pixel] = 3;
    }

    // Filtering: trilinear always; anisotropic from the device feature and its limit. Reporting 0
    // would change what the engine does -- GxTexCreate refuses a GxTex_Anisotropic texture outright
    // when the cap is 0 -- so the limit is reported even before a device exists.
    this->m_caps.m_texFilterTrilinear = 1;
    this->m_caps.m_maxTexAnisotropy = 16;
    this->m_caps.m_texFilterAnisotropic = 1;

    if (h) {
        uint32_t aniso = static_cast<uint32_t>(props.limits.maxSamplerAnisotropy);
        this->m_caps.m_maxTexAnisotropy = aniso ? aniso : 1;
        this->m_caps.m_texFilterAnisotropic = h->samplerAnisotropy && aniso >= 2;
    }

    this->m_caps.m_depthBias = 1;
    this->m_caps.m_colorWrite = 1;

    // The hardware cursor (a Win32 cursor from the engine's image) and occlusion queries (the
    // backend's query pool) are offered; user clip planes are not (the engine takes no clip-plane
    // path with 0: the model cache clips in software), nor multisampling (single-sampled).
    this->m_caps.m_maxClipPlanes = 0;
    this->m_caps.m_hwCursor = 1;
    this->m_caps.m_occlusionQuery = 1;

    // Points: from the device's own range, and 1.0 when large points are off.
    float maxPoint = 8192.0f;

    if (h) {
        maxPoint = h->largePoints ? props.limits.pointSizeRange[1] : 1.0f;
    }

    this->m_caps.m_pointScaleMax = maxPoint;
    this->m_caps.m_pointParameters = maxPoint > 1.0f;
    this->m_caps.m_pointSprite = maxPoint > 1.0f;

    this->m_caps.m_blendFactor = 1;
    this->m_caps.m_stereoAvailable = 0;
    this->m_caps.m_shaderAlphaRef = 1;
    this->m_caps.m_stencilBits = gxvk::vk::HasStencil(gxvk::vk::frame::DepthFormat()) ? 8 : 0;  // DECOMP
    this->m_caps.unused_0118[0] = 0;
    this->m_caps.unused_0118[1] = 0;
    this->m_caps.unused_0118[2] = 0;
    this->m_caps.unused_0118[3] = 0;
    this->m_caps.unk_0134 = this->m_caps.m_shaderTargets[GxSh_Pixel] != 4;
    this->m_caps.unk_0138 = this->m_caps.m_shaderTargets[GxSh_Pixel] != 4;

    if (!h) {
        CGxDevice::Log("CGxDeviceVulkan::ISetCaps(): no Vulkan device yet; the size, anisotropy, point and render-target rows are the table's defaults until the bring-up completes");
    }
}
