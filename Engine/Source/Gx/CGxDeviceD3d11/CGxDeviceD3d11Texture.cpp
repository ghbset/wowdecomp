#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/FfShaders.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Os/Error.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Services/TextureBlob.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Gx/CGxDeviceD3d11/D3d11Util.h"

// Textures and read back (split from CGxDeviceD3d11.cpp).

// 16-bit texel data -> Argb8888 (B8G8R8A8 in memory), channels widened by bit replication; 565 gets alpha 0xFF.
// Returns 0 for any other data format so the caller falls through to the shared Blit (D3D9's tables stay stock).
static int32_t ConvertTexels16To8888(const C2iVector& size, const void* in, uint32_t inStride, EGxTexFormat inFormat, void* out, uint32_t outStride) {
    if (inFormat != GxTex_Argb4444 && inFormat != GxTex_Argb1555 && inFormat != GxTex_Rgb565) {
        return 0;
    }

    for (int32_t y = 0; y < size.y; y++) {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(in) + y * inStride);
        uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(out) + y * outStride);

        for (int32_t x = 0; x < size.x; x++) {
            uint32_t p = src[x];
            uint32_t a, r, g, b;

            if (inFormat == GxTex_Argb4444) {
                a = ((p >> 12) & 0xF) * 0x11;
                r = ((p >> 8) & 0xF) * 0x11;
                g = ((p >> 4) & 0xF) * 0x11;
                b = (p & 0xF) * 0x11;
            } else if (inFormat == GxTex_Argb1555) {
                a = (p & 0x8000) ? 0xFF : 0;
                r = (p >> 10) & 0x1F;
                g = (p >> 5) & 0x1F;
                b = p & 0x1F;
                r = (r << 3) | (r >> 2);
                g = (g << 3) | (g >> 2);
                b = (b << 3) | (b >> 2);
            } else {
                a = 0xFF;
                r = (p >> 11) & 0x1F;
                g = (p >> 5) & 0x3F;
                b = p & 0x1F;
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);
            }

            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Textures

void CGxDeviceD3d11::ITexRelease(CGxTex* texId) {
    TexApi* api = static_cast<TexApi*>(texId->m_apiSpecificData);

    if (!api) {
        return;
    }

    for (uint32_t i = 0; i < 16; i++) {
        SafeRelease(api->m_rtv[i]);
    }

    SafeRelease(api->m_srv);
    SafeRelease(api->m_dsv);
    SafeRelease(api->m_texture);
    delete api;
    texId->m_apiSpecificData = nullptr;
}

// The texture ITexCreate makes for texId as it stands now (flags, format, base mip).
void CGxDeviceD3d11::ITexDesc(CGxTex* texId, D3D11_TEXTURE2D_DESC& desc) {
    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    bool cube = texId->m_target == GxTex_CubeMap;
    bool depth = texId->m_format == GxTex_D24X8;
    // GENERATE_MIPS needs RENDER_TARGET, which BC formats cannot have: DXT textures keep their uploaded mips.
    bool compressed = texId->m_format >= GxTex_Dxt1 && texId->m_format <= GxTex_Dxt5;
    bool generateMips = texId->m_flags.m_generateMipMaps && !depth && !compressed;

    memset(&desc, 0, sizeof(desc));
    desc.Width = width ? width : 1;
    desc.Height = height ? height : 1;
    desc.MipLevels = depth ? 1 : (generateMips ? 0 : mipCount - baseMip);
    desc.ArraySize = cube ? 6 : 1;
    desc.Format = s_GxTexFmtToDxgiFmt[texId->m_format];
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

    if (depth) {
        desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    } else if (texId->m_flags.m_renderTarget || generateMips) {
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    }

    if (generateMips) {
        desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }
}

// Byte offset, row pitch and row count of subresource sub (D3D11CalcSubresource order) in a TexShadow's texels.
static uint32_t TexShadowLayout(const D3D11_TEXTURE2D_DESC& desc, uint32_t levels, EGxTexFormat format, uint32_t sub, uint32_t& pitch, uint32_t& rows) {
    bool compressed = format >= GxTex_Dxt1 && format <= GxTex_Dxt5;
    uint32_t offset = 0;

    for (uint32_t i = 0;; i++) {
        uint32_t level = i % levels;
        uint32_t width = desc.Width >> level ? desc.Width >> level : 1;
        uint32_t height = desc.Height >> level ? desc.Height >> level : 1;

        if (compressed) {
            pitch = ((width + 3) >> 2) * CGxDevice::s_texFormatBytesPerBlock[format];
            rows = (height + 3) >> 2;
        } else {
            pitch = (width * CGxDevice::s_texFormatBitDepth[format] + 7) >> 3;
            rows = height;
        }

        if (i == sub) {
            return offset;
        }

        offset += pitch * rows;
    }
}

// texId's shadow, made (zeroed) on its first upload when make is set. A shadow that no longer fits the texture is
// dropped. None for render targets, depth and file-backed textures, whose callback reloads the file on GxTex_Lock:
// those are most of the texture memory and a copy would only double it (Wow32 address space).
CGxDeviceD3d11::TexShadow* CGxDeviceD3d11::ITexShadow(CGxTex* texId, const TexApi* api, bool make) {
    TexShadow* shadow = static_cast<TexShadow*>(texId->m_apiSpecificData2);
    bool fileBacked = texId->m_userFunc == UpdateBlpTextureAsync || texId->m_userFunc == UpdateTgaTexture || texId->m_userFunc == UpdateBlobTexture;
    bool wanted = !fileBacked && !texId->m_flags.m_renderTarget && texId->m_format != GxTex_D24X8;

    if (shadow && (!wanted || shadow->m_levels != api->m_levels || memcmp(&shadow->m_wanted, &api->m_wanted, sizeof(shadow->m_wanted)) != 0)) {
        free(shadow);
        shadow = nullptr;
        texId->m_apiSpecificData2 = nullptr;
    }

    if (!shadow && wanted && make) {
        uint32_t pitch;
        uint32_t rows;
        uint32_t size = TexShadowLayout(api->m_wanted, api->m_levels, texId->m_format, api->m_levels * api->m_wanted.ArraySize, pitch, rows);
        shadow = static_cast<TexShadow*>(calloc(1, sizeof(TexShadow) + size));

        if (shadow) {
            shadow->m_wanted = api->m_wanted;
            shadow->m_levels = api->m_levels;
            texId->m_apiSpecificData2 = shadow;
        }
    }

    return shadow;
}

// 4.3.4 ITexCreate (0x0086B0C0) shape: colour textures typed, D24X8 typeless with a DSV and an SRV.
void CGxDeviceD3d11::ITexCreate(CGxTex* texId) {
    this->ITexRelease(texId);

    texId->m_format = s_GxTexFormatToUse[texId->m_format];

    bool cube = texId->m_target == GxTex_CubeMap;
    bool depth = texId->m_format == GxTex_D24X8;
    bool renderTarget = texId->m_flags.m_renderTarget;

    D3D11_TEXTURE2D_DESC desc;
    this->ITexDesc(texId, desc);

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &texture);

    if (FAILED(hr)) {
        // 4.3.4 logs the failure (D3d11Error 0x0086B0A0) with these fields.
        D3d11LogOnce("CGxDeviceD3d11: CreateTexture2D failed 0x%08X: gx format %d dxgi %d %ux%u mips %u array %u bind 0x%X misc 0x%X", static_cast<uint32_t>(hr), texId->m_format, desc.Format, desc.Width, desc.Height, desc.MipLevels, desc.ArraySize, desc.BindFlags, desc.MiscFlags);

        if (renderTarget || depth) {
            return;
        }

        EGxTexFormat from = texId->m_format;

        // Only this texture falls back; the next one of the same format tries its own format again. BC level 0 must
        // be a multiple of 4 in D3D10+ (D3D9 allowed 1x1/2x2 DXT): decode to 8888, Blit has Dxt -> Argb8888.
        if (from >= GxTex_Dxt1 && from <= GxTex_Dxt5) {
            texId->m_format = GxTex_Argb8888;
        } else {
            texId->m_format = s_tolerableTexFmtMapping[from];
        }

        if (texId->m_format == from) {
            return;
        }

        D3d11LogOnce("CGxDeviceD3d11: texture format remap %d -> %d (%ux%u, this texture only)", from, texId->m_format, desc.Width, desc.Height);
        this->ITexDesc(texId, desc);
        hr = this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &texture);

        if (FAILED(hr)) {
            D3d11LogOnce("CGxDeviceD3d11: CreateTexture2D fallback failed 0x%08X: gx format %d dxgi %d %ux%u mips %u bind 0x%X", static_cast<uint32_t>(hr), texId->m_format, desc.Format, desc.Width, desc.Height, desc.MipLevels, desc.BindFlags);
            return;
        }
    }

    D3D11_TEXTURE2D_DESC wanted = desc;
    texture->GetDesc(&desc);

    TexApi* api = new TexApi;
    memset(api, 0, sizeof(*api));
    api->m_texture = texture;
    api->m_levels = desc.MipLevels;
    api->m_wanted = wanted;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    memset(&srvDesc, 0, sizeof(srvDesc));
    srvDesc.Format = depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : desc.Format;

    if (cube) {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = desc.MipLevels;
    } else {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
    }

    hr = this->m_d3dDevice->CreateShaderResourceView(texture, &srvDesc, &api->m_srv);

    if (FAILED(hr)) {
        D3d11LogOnce("CGxDeviceD3d11: CreateShaderResourceView failed 0x%08X: gx format %d dxgi %d %ux%u mips %u dim %d", static_cast<uint32_t>(hr), texId->m_format, srvDesc.Format, desc.Width, desc.Height, desc.MipLevels, srvDesc.ViewDimension);
    }

    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
        memset(&dsvDesc, 0, sizeof(dsvDesc));
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        this->m_d3dDevice->CreateDepthStencilView(texture, &dsvDesc, &api->m_dsv);
    } else if (renderTarget) {
        // RTT MIPS: a 2D target gets one RTV per mip level (slot = level), a cube one per face at level 0.
        uint32_t views = cube ? desc.ArraySize : (desc.MipLevels < 16 ? desc.MipLevels : 16);

        for (uint32_t face = 0; face < views; face++) {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
            memset(&rtvDesc, 0, sizeof(rtvDesc));
            rtvDesc.Format = desc.Format;

            if (cube) {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.FirstArraySlice = face;
                rtvDesc.Texture2DArray.ArraySize = 1;
            } else {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                rtvDesc.Texture2D.MipSlice = face;
            }

            this->m_d3dDevice->CreateRenderTargetView(texture, &rtvDesc, &api->m_rtv[face]);
        }
    }

    texId->m_apiSpecificData = api;
    texId->m_needsCreation = 0;

    // Refill from the shadow: a full device recreate released the texture and its callback may not have the texels.
    if (TexShadow* shadow = this->ITexShadow(texId, api, false)) {
        const uint8_t* bits = reinterpret_cast<const uint8_t*>(shadow + 1);

        for (uint32_t sub = 0; sub < api->m_levels * desc.ArraySize; sub++) {
            uint32_t pitch;
            uint32_t rows;
            uint32_t offset = TexShadowLayout(shadow->m_wanted, shadow->m_levels, texId->m_format, sub, pitch, rows);
            this->m_d3dContext->UpdateSubresource(texture, sub, nullptr, bits + offset, pitch, 0);
        }

        if ((desc.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) && api->m_srv) {
            this->m_d3dContext->GenerateMips(api->m_srv);
        }
    }
}

// CGxDeviceD3d::ITexUpload with LockRect replaced by Blit into a scratch block + UpdateSubresource.
void CGxDeviceD3d11::ITexUpload(CGxTex* texId) {
    uint32_t texelStrideInBytes;
    const void* texels = nullptr;
    texId->m_userFunc(GxTex_Lock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);

    uint32_t width;
    uint32_t height;
    uint32_t baseMip;
    uint32_t mipCount;
    this->ITexWHDStartEnd(texId, width, height, baseMip, mipCount);

    TexApi* api = static_cast<TexApi*>(texId->m_apiSpecificData);
    int32_t compressed = texId->m_dataFormat >= GxTex_Dxt1 && texId->m_dataFormat <= GxTex_Dxt5;
    int32_t destCompressed = texId->m_format >= GxTex_Dxt1 && texId->m_format <= GxTex_Dxt5;
    uint32_t faceCount = texId->m_target == GxTex_CubeMap ? 6 : 1;
    TexShadow* shadow = this->ITexShadow(texId, api, true);

    for (uint32_t face = 0; face < faceCount; face++) {
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        uint32_t level = 0;

        for (uint32_t mip = baseMip; mip != mipCount && level < api->m_levels; mip++, level++) {
            texels = nullptr;
            texId->m_userFunc(GxTex_Latch, mipWidth, mipHeight, face, mip, texId->m_userArg, texelStrideInBytes, texels);

            if (texels && !texId->m_flags.m_renderTarget) {
                RECT rect;
                rect.left = texId->m_updateRect.minX >> mip;
                rect.top = texId->m_updateRect.minY >> mip;
                // 4.3.4: 0x0086C210/0x1405A0450 rounds the end up per mip; flooring dropped odd edge rows/columns.
                rect.bottom = ((texId->m_updateRect.maxY - 1) >> mip) + 1;
                rect.right = ((texId->m_updateRect.maxX - 1) >> mip) + 1;

                if (rect.bottom > static_cast<int32_t>(mipHeight)) {
                    rect.bottom = mipHeight;
                }

                if (rect.right > static_cast<int32_t>(mipWidth)) {
                    rect.right = mipWidth;
                }

                if (rect.bottom <= rect.top) {
                    rect.bottom = rect.top + 1;
                }

                if (rect.right <= rect.left) {
                    rect.right = rect.left + 1;
                }

                if (destCompressed) {
                    rect.left &= ~3;
                    rect.top &= ~3;
                    rect.right = (rect.right + 3) & ~3;
                    rect.bottom = (rect.bottom + 3) & ~3;

                    if (rect.bottom > static_cast<int32_t>(mipHeight)) {
                        rect.bottom = mipHeight;
                    }

                    if (rect.right > static_cast<int32_t>(mipWidth)) {
                        rect.right = mipWidth;
                    }
                }

                const uint8_t* src = static_cast<const uint8_t*>(texels);

                if (!texId->m_flags.m_bit15) {
                    EGxTexFormat dataFormat = texId->m_dataFormat;

                    if (compressed) {
                        src += s_texFormatBytesPerBlock[dataFormat] * (rect.left >> 2) + texelStrideInBytes * (rect.top >> 2);
                    } else {
                        src += ((rect.left * s_texFormatBitDepth[dataFormat]) >> 3) + texelStrideInBytes * rect.top;
                    }
                }

                C2iVector size;
                size.x = rect.right - rect.left;
                size.y = rect.bottom - rect.top;

                uint32_t pitch;
                uint32_t rows;

                if (destCompressed) {
                    pitch = ((size.x + 3) >> 2) * s_texFormatBytesPerBlock[texId->m_format];
                    rows = (size.y + 3) >> 2;
                } else {
                    pitch = (size.x * s_texFormatBitDepth[texId->m_format] + 7) >> 3;
                    rows = size.y;
                }

                void* scratch = malloc(pitch * rows);

                if (!scratch) {
                    goto done;
                }

                int32_t converted = (texId->m_format == GxTex_Argb8888 && ConvertTexels16To8888(size, src, texelStrideInBytes, texId->m_dataFormat, scratch, pitch))
                    || Blit(size, BlitAlpha_0, src, texelStrideInBytes, GxGetBlitFormat(texId->m_dataFormat), scratch, pitch, GxGetBlitFormat(texId->m_format));

                if (!converted) {
                    D3d11LogOnce("CGxDeviceD3d11: texture upload has no conversion %d -> %d (%dx%d mip %u)", texId->m_dataFormat, texId->m_format, size.x, size.y, mip);
                }

                if (shadow) {
                    uint32_t shadowPitch;
                    uint32_t shadowRows;
                    uint8_t* dest = reinterpret_cast<uint8_t*>(shadow + 1) + TexShadowLayout(shadow->m_wanted, shadow->m_levels, texId->m_format, D3D11CalcSubresource(level, face, api->m_levels), shadowPitch, shadowRows);
                    uint32_t x = destCompressed ? (rect.left >> 2) * s_texFormatBytesPerBlock[texId->m_format] : (rect.left * s_texFormatBitDepth[texId->m_format]) >> 3;
                    uint32_t y = destCompressed ? rect.top >> 2 : rect.top;

                    for (uint32_t row = 0; row < rows; row++) {
                        memcpy(dest + (y + row) * shadowPitch + x, static_cast<uint8_t*>(scratch) + row * pitch, pitch);
                    }
                }

                D3D11_BOX box = { static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0, static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1 };
                this->m_d3dContext->UpdateSubresource(api->m_texture, D3D11CalcSubresource(level, face, api->m_levels), &box, scratch, pitch, 0);
                free(scratch);
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

    if (texId->m_flags.m_generateMipMaps && !destCompressed && api->m_srv && api->m_levels > 1) {
        this->m_d3dContext->GenerateMips(api->m_srv);
    }

done:
    texels = nullptr;
    texId->m_userFunc(GxTex_Unlock, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
}

void CGxDeviceD3d11::ITexMarkAsUpdated(CGxTex* texId) {
    if (!this->m_context) {
        return;
    }

    // Not stock: D3D9 textures take new flags as sampler state, a D3D11 texture's mip count, bind flags and format
    // are fixed at creation. A CGxTex given new flags (TexSetFlags, or reused by the texture cache) whose texture no
    // longer matches is made again and re-uploaded. Current render targets are left alone.
    TexApi* api = static_cast<TexApi*>(texId->m_apiSpecificData);

    if (api && !texId->m_needsCreation && this->m_textureTarget[0].m_texture != texId && this->m_textureTarget[1].m_texture != texId) {
        // Every input of ITexDesc (and ITexWHDStartEnd): unchanged since the last match, nothing to rebuild.
        uint32_t flags;
        memcpy(&flags, &texId->m_flags, sizeof(flags));
        const uint32_t key[6] = { texId->m_width, texId->m_height, static_cast<uint32_t>(texId->m_target), static_cast<uint32_t>(texId->m_format), flags, this->m_baseMipLevel };
        static_assert(sizeof(key) == sizeof(api->m_wantedKey), "key size");
        if (memcmp(key, api->m_wantedKey, sizeof(key)) != 0) {
            D3D11_TEXTURE2D_DESC wanted;
            this->ITexDesc(texId, wanted);

            if (memcmp(&wanted, &api->m_wanted, sizeof(wanted)) == 0) {
                memcpy(api->m_wantedKey, key, sizeof(key));
            } else {
                D3d11LogOnce("CGxDeviceD3d11: texture recreated for new flags: %ux%u -> %ux%u, dxgi %d -> %d, mips %u -> %u, bind 0x%X -> 0x%X, misc 0x%X -> 0x%X", api->m_wanted.Width, api->m_wanted.Height, wanted.Width, wanted.Height, api->m_wanted.Format, wanted.Format, api->m_wanted.MipLevels, wanted.MipLevels, api->m_wanted.BindFlags, wanted.BindFlags, api->m_wanted.MiscFlags, wanted.MiscFlags);
                this->ITexRelease(texId);
                texId->m_needsCreation = 1;
                static CiRect s_updateRect;
                this->TexMarkForUpdate(texId, s_updateRect, 0);
            }
        }
    }

    if (!texId->m_needsUpdate) {
        return;
    }

    if (texId->m_needsCreation || !texId->m_apiSpecificData) {
        this->ITexCreate(texId);

        // An empty rect is ITexForceRecreation's "refilled from the shadow". The shadow did not fit the new texture
        // (base mip change): refill it through the callback instead.
        if (!texId->m_apiSpecificData2 && texId->m_updateRect.maxX <= texId->m_updateRect.minX) {
            texId->m_updateRect = CiRect(0, 0, texId->m_height, texId->m_width);
        }
    }

    if (!texId->m_needsCreation && texId->m_apiSpecificData) {
        if (texId->m_userFunc && texId->m_updateRect.maxX > texId->m_updateRect.minX) {
            this->ITexUpload(texId);
        }

        CGxDevice::ITexMarkAsUpdated(texId);
    }
}

void CGxDeviceD3d11::ITexForceRecreation(int32_t all) {
    // 4.3.4 (0x0086C590) walks every texture, created or not. Skipping those without a D3D object (never bound yet,
    // or released by an earlier recreation) left them with whatever partial update rect they had and without the
    // GxTex_3 regen notice, so after a device recreate they came back with only part of their texels.
    for (CGxTex* texId = this->m_textures.Head(); texId; texId = this->m_textures.Next(texId)) {
        if (!(all || texId->m_flags.m_renderTarget)) {
            continue;
        }

        static CiRect s_updateRect;

        // Unbind it if it is a current target, so no stale view stays bound.
        for (uint32_t i = 0; i < 2; i++) {
            if (this->m_textureTarget[i].m_texture == texId) {
                this->m_textureTarget[i].m_apiSpecific = nullptr;
            }
        }

        this->ITexRelease(texId);
        texId->m_needsCreation = 1;
        int8_t pending = texId->m_needsUpdate;
        this->TexMarkForUpdate(texId, s_updateRect, 0);

        // Shadowed: ITexCreate refills it, so nothing to upload until the app updates it again. An update still
        // pending is not in the shadow yet: that one keeps the full re-upload through the callback.
        if (texId->m_apiSpecificData2 && !pending) {
            texId->m_updateRect = CiRect(0, 0, 0, 0);
        }

        uint32_t texelStrideInBytes;
        const void* texels;

        if (texId->m_userFunc) {
            texId->m_userFunc(GxTex_3, texId->m_width, texId->m_height, 0, 0, texId->m_userArg, texelStrideInBytes, texels);
        }
    }

    if (this->m_hwnd && this->m_d3dDevice) {
        this->NotifyOnTextureRecreation();
    }
}

void CGxDeviceD3d11::TexDestroy(CGxTex* texId) {
    // Not stock: IStateSyncTextures dereferences every bound CGxTex* on every draw; D3D9 only touches a slot when it
    // changes. CWorld::Destroy -> CShadowCache::Destroy frees the shadow maps while GxRs_Texture4 (and 5..7 above
    // SHADOW_MEDIUM) still hold them, and glue never rebinds those slots, so after a logout every glue draw read a
    // freed CGxTex, ran ITexMarkAsUpdated/ITexCreate on whatever reused the memory and bound its SRV. Forget it here.
    for (uint32_t i = 0; i < 16; i++) {
        if (this->m_appRenderStates[GxRs_Texture0 + i].m_value.m_data.p == texId) {
            this->m_appRenderStates[GxRs_Texture0 + i].m_value.m_data.p = nullptr;
        }

        if (this->m_hwRenderStates[GxRs_Texture0 + i].m_data.p == texId) {
            this->m_hwRenderStates[GxRs_Texture0 + i].m_data.p = nullptr;
        }
    }

    for (uint32_t i = 0; i < this->m_pushedStates.Count(); i++) {
        CGxPushedRenderState& pushed = this->m_pushedStates[i];

        if (pushed.m_which >= GxRs_Texture0 && pushed.m_which < GxRs_Texture0 + 16 && pushed.m_value.m_data.p == texId) {
            pushed.m_value.m_data.p = nullptr;
        }
    }

    // m_texture too: a new CGxTex at the same address set as a target would hit DeviceSetRenderTarget's early return
    // and never get its RTV bound.
    for (uint32_t i = 0; i < 2; i++) {
        if (this->m_textureTarget[i].m_texture == texId) {
            bool bound = this->m_textureTarget[i].m_apiSpecific != nullptr;
            this->m_textureTarget[i].m_texture = nullptr;
            this->m_textureTarget[i].m_apiSpecific = nullptr;

            if (bound && this->m_d3dContext) {
                this->IBindRenderTargets();
            }
        }
    }

    this->ITexRelease(texId);
    free(texId->m_apiSpecificData2);
    texId->m_apiSpecificData2 = nullptr;
    CGxDevice::TexDestroy(texId);
}

void CGxDeviceD3d11::DeviceSetBaseMipLevel(uint32_t level) {
    this->ITexForceRecreation(1);
    CGxDevice::DeviceSetBaseMipLevel(level);
}

void CGxDeviceD3d11::DeviceCopyTex(CGxTex* source, uint32_t sourcePlane, CGxTex* dest, uint32_t destPlane) {
    if (!this->m_context || !source || !dest) {
        return;
    }

    if (source->m_needsCreation || !source->m_apiSpecificData) {
        this->ITexCreate(source);
    }

    if (dest->m_needsCreation || !dest->m_apiSpecificData) {
        this->ITexCreate(dest);
    }

    TexApi* src = static_cast<TexApi*>(source->m_apiSpecificData);
    TexApi* dst = static_cast<TexApi*>(dest->m_apiSpecificData);

    if (!src || !dst) {
        return;
    }

    bool same = src->m_wanted.Format == dst->m_wanted.Format && src->m_wanted.Width == dst->m_wanted.Width && src->m_wanted.Height == dst->m_wanted.Height;

    // Other format or size: D3D9 StretchRect converts and scales, CopySubresourceRegion would drop it.
    if (!same && src->m_srv && src->m_wanted.ArraySize == 1 && this->IBlit(src->m_srv, src->m_wanted.Width, src->m_wanted.Height, nullptr, dest, 0, destPlane, nullptr, true)) {
        return;
    }

    if (!this->ITexMatchFormat(dest, src->m_wanted.Format)) {
        return;
    }

    dst = static_cast<TexApi*>(dest->m_apiSpecificData);

    if (dst) {
        this->m_d3dContext->CopySubresourceRegion(dst->m_texture, D3D11CalcSubresource(0, destPlane, dst->m_levels), 0, 0, 0, src->m_texture, D3D11CalcSubresource(0, sourcePlane, src->m_levels), nullptr);
    }
}

// CopySubresourceRegion needs both sides in one format group and drops the copy silently otherwise (black). A copy
// target in another format is made again in the source's (it holds nothing a copy is not about to overwrite).
bool CGxDeviceD3d11::ITexMatchFormat(CGxTex* texId, DXGI_FORMAT format) {
    TexApi* api = static_cast<TexApi*>(texId->m_apiSpecificData);

    if (!api) {
        return false;
    }

    if (api->m_wanted.Format == format) {
        return true;
    }

    for (uint32_t f = GxTex_Abgr8888; f < GxTexFormats_Last; f++) {
        if (s_GxTexFmtToDxgiFmt[f] == format && f != GxTex_D24X8) {
            D3d11LogOnce("CGxDeviceD3d11: copy target %ux%u remapped dxgi %d -> %d (gx format %d -> %u) to match its source", api->m_wanted.Width, api->m_wanted.Height, api->m_wanted.Format, format, texId->m_format, f);
            texId->m_format = static_cast<EGxTexFormat>(f);
            this->ITexCreate(texId);
            api = static_cast<TexApi*>(texId->m_apiSpecificData);
            return api && api->m_wanted.Format == format;
        }
    }

    D3d11LogOnce("CGxDeviceD3d11: copy dropped, target dxgi %d cannot take source dxgi %d", api->m_wanted.Format, format);
    return false;
}

// Back buffer region -> texture (D3D9 StretchRect with equal rects).
int32_t CGxDeviceD3d11::TexCopy(CGxTex* dest, const C2iVector& destPos, const C2iVector& sourcePos, const C2iVector& size, uint32_t level, uint32_t plane) {
    if (!this->m_context) {
        return 0;
    }

    if (dest->m_needsCreation || !dest->m_apiSpecificData) {
        this->ITexCreate(dest);
    }

    ID3D11Texture2D* source = this->IResolvedBackBuffer();

    if (dest->m_needsCreation || !dest->m_apiSpecificData || !source) {
        return 0;
    }

    D3D11_TEXTURE2D_DESC sourceDesc;
    source->GetDesc(&sourceDesc);
    TexApi* api = static_cast<TexApi*>(dest->m_apiSpecificData);

    // Another format (a 10-bit back buffer into an 8888 texture): convert through the blit when the target can be
    // drawn to, else remap the target to the back buffer's format as before.
    if (api->m_wanted.Format != sourceDesc.Format && (api->m_wanted.BindFlags & D3D11_BIND_RENDER_TARGET)) {
        CiRect sourceRect = { sourcePos.y, sourcePos.x, sourcePos.y + size.y, sourcePos.x + size.x };
        CiRect destRect = { destPos.y, destPos.x, destPos.y + size.y, destPos.x + size.x };

        if (this->IBlitBackBuffer(dest, level, plane, &sourceRect, &destRect, false)) {
            return 1;
        }
    }

    if (!this->ITexMatchFormat(dest, sourceDesc.Format)) {
        return 0;
    }

    api = static_cast<TexApi*>(dest->m_apiSpecificData);

    // A box past the back buffer edge makes CopySubresourceRegion drop the whole copy.
    D3D11_BOX box = { static_cast<UINT>(sourcePos.x), static_cast<UINT>(sourcePos.y), 0, static_cast<UINT>(sourcePos.x + size.x), static_cast<UINT>(sourcePos.y + size.y), 1 };
    box.right = box.right < sourceDesc.Width ? box.right : sourceDesc.Width;
    box.bottom = box.bottom < sourceDesc.Height ? box.bottom : sourceDesc.Height;

    if (box.left >= box.right || box.top >= box.bottom) {
        return 0;
    }

    this->m_d3dContext->CopySubresourceRegion(api->m_texture, D3D11CalcSubresource(level, plane, api->m_levels), destPos.x, destPos.y, 0, source, 0, &box);

    return 1;
}

// Equal-size stretches are copies; scaled ones need a blit shader.
int32_t CGxDeviceD3d11::TexStretch(CGxTex* dest, const CiRect* destRect, const CiRect* sourceRect, uint32_t level, uint32_t face) {
    if (!this->m_context) {
        return 0;
    }

    if (destRect && sourceRect && destRect->maxX - destRect->minX == sourceRect->maxX - sourceRect->minX && destRect->maxY - destRect->minY == sourceRect->maxY - sourceRect->minY) {
        C2iVector destPos;
        destPos.x = destRect->minX;
        destPos.y = destRect->minY;
        C2iVector sourcePos;
        sourcePos.x = sourceRect->minX;
        sourcePos.y = sourceRect->minY;
        C2iVector size;
        size.x = sourceRect->maxX - sourceRect->minX;
        size.y = sourceRect->maxY - sourceRect->minY;

        return this->TexCopy(dest, destPos, sourcePos, size, level, face);
    }

    // Scaled, or a null rect (whole back buffer / whole target level): StretchRect through the blit shader.
    if (dest->m_needsCreation || !dest->m_apiSpecificData) {
        this->ITexCreate(dest);
    }

    if (dest->m_needsCreation || !dest->m_apiSpecificData) {
        return 0;
    }

    return this->IBlitBackBuffer(dest, level, face, sourceRect, destRect, true);
}

int32_t CGxDeviceD3d11::IBlitBackBuffer(CGxTex* dest, uint32_t level, uint32_t face, const CiRect* sourceRect, const CiRect* destRect, bool linear) {
    ID3D11Texture2D* source = this->IResolvedBackBuffer();

    if (!source) {
        return 0;
    }

    if (!this->m_backBufferSrv && FAILED(this->m_d3dDevice->CreateShaderResourceView(source, nullptr, &this->m_backBufferSrv))) {
        this->m_backBufferSrv = nullptr;
        D3d11LogOnce("CGxDeviceD3d11: back buffer SRV failed, blits from it dropped");
        return 0;
    }

    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);

    return this->IBlit(this->m_backBufferSrv, desc.Width, desc.Height, sourceRect, dest, level, face, destRect, linear);
}

// D3D9 StretchRect: one triangle into `dest` (level, face), sampling `source` over sourceRect (null = all of it).
// Draws with its own state, then marks everything IStateSync caches as stale.
int32_t CGxDeviceD3d11::IBlit(ID3D11ShaderResourceView* source, uint32_t sourceWidth, uint32_t sourceHeight, const CiRect* sourceRect, CGxTex* dest, uint32_t level, uint32_t face, const CiRect* destRect, bool linear, ID3D11PixelShader* ps) {
    TexApi* api = static_cast<TexApi*>(dest->m_apiSpecificData);

    if (!api || !source || !sourceWidth || !sourceHeight) {
        return 0;
    }

    D3D11_TEXTURE2D_DESC desc;
    api->m_texture->GetDesc(&desc);

    if (!(desc.BindFlags & D3D11_BIND_RENDER_TARGET) || level >= desc.MipLevels || face >= desc.ArraySize) {
        D3d11LogOnce("CGxDeviceD3d11: blit dropped, target %ux%u dxgi %d is not a render target", desc.Width, desc.Height, desc.Format);
        return 0;
    }

    if (!this->m_blitVs) {
        static const float s_zero[4] = {};
        D3D11_BUFFER_DESC cbDesc;
        memset(&cbDesc, 0, sizeof(cbDesc));
        cbDesc.ByteWidth = 16;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA init = { s_zero, 0, 0 };

        if (FAILED(this->m_d3dDevice->CreateVertexShader(g_blitVs.data, g_blitVs.size, nullptr, &this->m_blitVs))
            || FAILED(this->m_d3dDevice->CreatePixelShader(g_blitPs.data, g_blitPs.size, nullptr, &this->m_blitPs))
            || FAILED(this->m_d3dDevice->CreateBuffer(&cbDesc, &init, &this->m_blitConstantBuffer))) {
            D3d11LogOnce("CGxDeviceD3d11: blit shaders failed");
            SafeRelease(this->m_blitVs);
            SafeRelease(this->m_blitPs);
            SafeRelease(this->m_blitConstantBuffer);
            return 0;
        }
    }

    // Cached views (ITexCreate): a cube's per face at level 0, a 2D target's per mip level.
    ID3D11RenderTargetView* rtv = desc.ArraySize > 1 ? (level == 0 ? api->m_rtv[face] : nullptr) : (level < 16 ? api->m_rtv[level] : nullptr);
    bool ownRtv = false;

    if (!rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        memset(&rtvDesc, 0, sizeof(rtvDesc));
        rtvDesc.Format = desc.Format;

        if (desc.ArraySize > 1) {
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.MipSlice = level;
            rtvDesc.Texture2DArray.FirstArraySlice = face;
            rtvDesc.Texture2DArray.ArraySize = 1;
        } else {
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtvDesc.Texture2D.MipSlice = level;
        }

        if (FAILED(this->m_d3dDevice->CreateRenderTargetView(api->m_texture, &rtvDesc, &rtv))) {
            return 0;
        }

        ownRtv = true;
    }

    uint32_t destWidth = desc.Width >> level ? desc.Width >> level : 1;
    uint32_t destHeight = desc.Height >> level ? desc.Height >> level : 1;
    bool hasDest = destRect && destRect->maxX > destRect->minX && destRect->maxY > destRect->minY;
    bool hasSource = sourceRect && sourceRect->maxX > sourceRect->minX && sourceRect->maxY > sourceRect->minY;

    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = hasDest ? static_cast<float>(destRect->minX) : 0.0f;
    viewport.TopLeftY = hasDest ? static_cast<float>(destRect->minY) : 0.0f;
    viewport.Width = hasDest ? static_cast<float>(destRect->maxX - destRect->minX) : static_cast<float>(destWidth);
    viewport.Height = hasDest ? static_cast<float>(destRect->maxY - destRect->minY) : static_cast<float>(destHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    float uv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

    if (hasSource) {
        uv[0] = static_cast<float>(sourceRect->minX) / sourceWidth;
        uv[1] = static_cast<float>(sourceRect->minY) / sourceHeight;
        uv[2] = static_cast<float>(sourceRect->maxX) / sourceWidth;
        uv[3] = static_cast<float>(sourceRect->maxY) / sourceHeight;
    }

    UploadConstants(this->m_d3dContext, this->m_blitConstantBuffer, uv, sizeof(uv));

    D3D11_SAMPLER_DESC samplerDesc;
    memset(&samplerDesc, 0, sizeof(samplerDesc));
    samplerDesc.Filter = linear ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11SamplerState* sampler = static_cast<ID3D11SamplerState*>(this->IStateGet(State_Sampler, &samplerDesc, sizeof(samplerDesc)));

    // Target first, so a source that is the bound RTV (the back buffer) is unbound before it is read.
    ID3D11ShaderResourceView* noSrvs[16] = {};
    this->m_d3dContext->PSSetShaderResources(0, 16, noSrvs);
    this->m_d3dContext->OMSetRenderTargets(1, &rtv, nullptr);
    this->m_d3dContext->RSSetViewports(1, &viewport);
    this->m_d3dContext->RSSetState(nullptr);
    this->m_d3dContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    this->m_d3dContext->OMSetDepthStencilState(nullptr, 0);
    this->m_d3dContext->IASetInputLayout(nullptr);
    this->m_d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    this->m_d3dContext->VSSetShader(this->m_blitVs, nullptr, 0);
    this->m_d3dContext->VSSetConstantBuffers(2, 1, &this->m_blitConstantBuffer);
    this->m_d3dContext->PSSetShader(ps ? ps : this->m_blitPs, nullptr, 0);
    this->m_d3dContext->PSSetShaderResources(0, 1, &source);
    this->m_d3dContext->PSSetSamplers(0, 1, &sampler);
    this->m_d3dContext->Draw(3, 0);
    this->m_d3dContext->PSSetShaderResources(0, 1, noSrvs);

    if (ownRtv) {
        rtv->Release();
    }

    // Back to exactly the targets that were bound (a DeviceResolveDepthBuffer / RestoreDepthSurface DSV included);
    // the SRV cache was dropped above, and the next IStateSync resends the rest.
    this->m_d3dContext->OMSetRenderTargets(this->m_curRtv ? 1 : 0, this->m_curRtv ? &this->m_curRtv : nullptr, this->m_curDsv);
    memset(this->m_curSrv, 0, sizeof(this->m_curSrv));
    this->m_curVs = nullptr;
    this->m_curPs = nullptr;
    this->m_curLayoutKey = 0;
    this->m_curTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    this->m_stateDirty |= Dirty_Raster | Dirty_Blend | Dirty_Depth;
    this->m_samplerDirty |= 1;
    this->m_viewportDirty = 1;
    this->m_scissorRectDirty = 1;

    return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Read back

void CGxDeviceD3d11::DeviceReadPixels(CiRect& rect, TSGrowableArray<CImVector>& pixels) {
    this->ClampRectToWindow(rect);

    uint32_t width = rect.maxX - rect.minX;
    uint32_t height = rect.maxY - rect.minY;
    pixels.SetCount(width * height);

    if (!this->m_context || !width || !height) {
        return;
    }

    ID3D11Texture2D* source = this->IResolvedBackBuffer();

    if (!source) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;

    if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &staging))) {
        return;
    }

    D3D11_BOX box = { static_cast<UINT>(rect.minX), static_cast<UINT>(rect.minY), 0, static_cast<UINT>(rect.maxX), static_cast<UINT>(rect.maxY), 1 };
    this->m_d3dContext->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped;

    if (SUCCEEDED(this->m_d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData);
        CImVector* dest = pixels.Ptr();

        if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            for (uint32_t y = 0; y < height; y++, row += mapped.RowPitch, dest += width) {
                memcpy(dest, row, sizeof(CImVector) * width);
            }
        } else if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            // 10-bit (gxColorBits 30) and RGBA back buffers, to the BGRA CImVector the callers expect.
            bool tenBit = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;

            for (uint32_t y = 0; y < height; y++, row += mapped.RowPitch, dest += width) {
                const uint32_t* src = reinterpret_cast<const uint32_t*>(row);

                for (uint32_t x = 0; x < width; x++) {
                    uint32_t v = src[x];
                    CImVector& out = dest[x];

                    if (tenBit) {
                        out.r = static_cast<uint8_t>((v & 0x3FF) >> 2);
                        out.g = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
                        out.b = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
                        out.a = static_cast<uint8_t>((v >> 30) * 0x55);
                    } else {
                        out.r = static_cast<uint8_t>(v);
                        out.g = static_cast<uint8_t>(v >> 8);
                        out.b = static_cast<uint8_t>(v >> 16);
                        out.a = static_cast<uint8_t>(v >> 24);
                    }
                }
            }
        } else {
            memset(pixels.Ptr(), 0xFF, sizeof(CImVector) * width * height);
        }

        this->m_d3dContext->Unmap(staging, 0);
    }

    staging->Release();
}

// Works where 12340 D3D9 returns zeros (0x006A1950): copies the readable depth to a staging texture. Depth-stencil
// resources can only be copied whole, so the full surface is copied and the rect cut out on the CPU.
// TODO(phase 5): MSAA depth (needs the resolve from the depth contract); returns zeros for now.
void CGxDeviceD3d11::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    this->ClampRectToWindow(rect);

    uint32_t width = rect.maxX - rect.minX;
    uint32_t height = rect.maxY - rect.minY;
    depths.SetCount(width * height);
    memset(depths.Ptr(), 0, sizeof(float) * width * height);

    if (!this->m_context || !this->m_depthTexture || !width || !height) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc;
    this->m_depthTexture->GetDesc(&desc);

    if (desc.SampleDesc.Count > 1) {
        return;
    }

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;

    if (FAILED(this->m_d3dDevice->CreateTexture2D(&desc, nullptr, &staging))) {
        return;
    }

    this->m_d3dContext->CopyResource(staging, this->m_depthTexture);

    D3D11_MAPPED_SUBRESOURCE mapped;

    if (SUCCEEDED(this->m_d3dContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        float* dest = depths.Ptr();

        for (uint32_t y = 0; y < height && rect.minY + y < desc.Height; y++) {
            const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * (rect.minY + y);

            for (uint32_t x = 0; x < width && rect.minX + x < desc.Width; x++) {
                uint32_t px = rect.minX + x;
                float value;

                switch (desc.Format) {
                    case DXGI_FORMAT_R32_TYPELESS:
                        value = reinterpret_cast<const float*>(row)[px];
                        break;

                    case DXGI_FORMAT_R16_TYPELESS:
                        value = reinterpret_cast<const uint16_t*>(row)[px] / 65535.0f;
                        break;

                    default:
                        value = (reinterpret_cast<const uint32_t*>(row)[px] & 0xFFFFFF) / 16777215.0f;
                        break;
                }

                dest[y * width + x] = value;
            }
        }

        this->m_d3dContext->Unmap(staging, 0);
    }

    staging->Release();
}

