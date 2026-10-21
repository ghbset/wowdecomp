#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "Engine/Source/Gxu/GxuFontMsdf.h"
#include "Engine/Source/Gxu/GxuFontMiscClasses.h"
#include "Engine/Source/Storm/H/storm.h"

// msdfgen headers
#include <msdfgen/msdfgen.h>
#include <msdfgen/msdfgen-ext.h>

// STATIC
uint8_t g_msdfEnabled = 1;

void MsdfInitialize() {
}

void MsdfShutdown() {
}

float MsdfComputeScreenPxRange(uint32_t renderedPixelSize) {
    // screenPxRange = (display_size / atlas_glyph_size) * sdf_range
    // This ensures the anti-aliasing width is consistent regardless of zoom level
    float scale = static_cast<float>(renderedPixelSize) / static_cast<float>(MSDF_GLYPH_SIZE);
    return scale * static_cast<float>(MSDF_PIXEL_RANGE);
}

static void CalculateYOffset(uint32_t pixelSize, int32_t baseline, FT_Face face, uint32_t glyphHeight, int32_t* yOffset, uint32_t* yStart) {
    if (glyphHeight > pixelSize) {
        glyphHeight = pixelSize;
    }
    int32_t offset = 0;
    int32_t start = 0;

    int32_t top = face->glyph->bitmap_top;
    if (top <= baseline) {
        start = baseline - top;
    } else {
        start = 0;
        offset = top - baseline;
    }

    start = start <= 0 ? 0 : start;
    int32_t available = static_cast<int32_t>(pixelSize) - static_cast<int32_t>(glyphHeight);

    if (available >= start) {
        *yOffset = offset;
        *yStart = static_cast<uint32_t>(start);
    } else {
        int32_t s = available <= 0 ? 0 : available;
        *yOffset = static_cast<int32_t>(pixelSize) - start - static_cast<int32_t>(glyphHeight);
        *yStart = static_cast<uint32_t>(s);
    }
}

bool MsdfGenerateGlyph(FT_Face face, uint32_t pixelSize, uint32_t code, uint32_t baseline, GLYPHBITMAPDATA* dataPtr, uint32_t padding) {
    if (!face || !pixelSize || !dataPtr) {
        return false;
    }

    FT_UInt index = FT_Get_Char_Index(face, code);
    if (!index) {
        return false;
    }

    if (FT_Load_Glyph(face, index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING)) {
        return false;
    }

    // Read the vector outline geometry before FT_Render_Glyph decomposes or replaces it
    msdfgen::Shape shape;
    FT_Error err = msdfgen::readFreetypeOutline(shape, &face->glyph->outline, 1.0 / 64.0);

    // Call FT_Render_Glyph to get exact FreeType bitmap metrics and dimensions
    if (FT_Render_Glyph(face->glyph, static_cast<FT_Render_Mode>(0)) != 0) {
        return false;
    }

    uint32_t width = face->glyph->bitmap.width;
    uint32_t height = pixelSize >= static_cast<uint32_t>(face->glyph->bitmap.rows) ? face->glyph->bitmap.rows : pixelSize;

    // Handle whitespace / empty glyphs (spaces, control characters, or empty outlines)
    if (err != 0 || shape.contours.empty() || shape.edgeCount() == 0 || !width || !height) {
        uint32_t emptyWidth = width ? (width + padding) : ((pixelSize + 3) >> 2);
        if (!emptyWidth) emptyWidth = pixelSize;
        uint32_t emptyHeight = height ? (height + padding) : pixelSize;
        if (!emptyHeight) emptyHeight = 1;

        uint32_t emptyPitch = emptyWidth * 3;
        uint32_t emptySize = emptyHeight * emptyPitch;

        void* data = SMemAlloc(emptySize, __FILE__, __LINE__, 0);
        if (data) {
            memset(data, 0, emptySize);
        }

        dataPtr->m_data = data;
        dataPtr->m_dataSize = emptySize;
        dataPtr->m_glyphWidth = emptyWidth;
        dataPtr->m_glyphHeight = emptyHeight;
        dataPtr->m_glyphCellWidth = emptyWidth;
        dataPtr->m_glyphPitch = emptyPitch;
        dataPtr->m_glyphAdvance = (face->glyph->metrics.horiAdvance / 64.0f) + 1.0f;
        dataPtr->m_glyphBearing = face->glyph->metrics.horiBearingX * (1.0f / 64.0f);
        dataPtr->m_yOffset = 0;
        dataPtr->m_yStart = 0;
        dataPtr->m_isMsdf = 1;
        return true;
    }

    shape.normalize();
    msdfgen::edgeColoringInkTrap(shape, 3.0, 0);

    uint32_t pad = padding / 2;
    uint32_t msdfWidth = width + padding;
    uint32_t msdfHeight = height + padding;

    if (msdfWidth > MSDF_ATLAS_SIZE || msdfHeight > MSDF_ATLAS_SIZE || !msdfWidth || !msdfHeight) {
        return false;
    }

    // Projection mapping:
    // Outline coordinates in `shape` are in pixels (1/64 FreeType units).
    // The bottom-left of FreeType's bitmap is at:
    //   x = face->glyph->bitmap_left
    //   y = face->glyph->bitmap_top - face->glyph->bitmap.rows
    double translateX = -static_cast<double>(face->glyph->bitmap_left) + static_cast<double>(pad);
    double translateY = -static_cast<double>(face->glyph->bitmap_top - static_cast<int32_t>(face->glyph->bitmap.rows)) + static_cast<double>(pad);

    // Ensure vector outline bounds are safely within the MSDF bitmap bounds
    msdfgen::Shape::Bounds bounds = shape.getBounds();
    if (bounds.t + translateY > static_cast<double>(msdfHeight)) {
        translateY = static_cast<double>(msdfHeight) - bounds.t;
    }
    if (bounds.b + translateY < 0.0) {
        translateY = -bounds.b;
    }
    if (bounds.r + translateX > static_cast<double>(msdfWidth)) {
        translateX = static_cast<double>(msdfWidth) - bounds.r;
    }
    if (bounds.l + translateX < 0.0) {
        translateX = -bounds.l;
    }

    msdfgen::Bitmap<float, 3> msdf(msdfWidth, msdfHeight);
    msdfgen::Projection projection(msdfgen::Vector2(1.0, 1.0), msdfgen::Vector2(translateX, translateY));
    msdfgen::MSDFGeneratorConfig config(true);
    config.overlapSupport = true;
    config.errorCorrection.distanceCheckMode = msdfgen::ErrorCorrectionConfig::DO_NOT_CHECK_DISTANCE;
    msdfgen::generateMSDF(msdf, shape, projection, msdfgen::Range(MSDF_PIXEL_RANGE), config);

    uint32_t pitch = msdfWidth * 3;
    uint32_t size = pitch * msdfHeight;

    uint8_t* pixelData = static_cast<uint8_t*>(SMemAlloc(size, __FILE__, __LINE__, 0));
    if (!pixelData) {
        return false;
    }

    for (uint32_t y = 0; y < msdfHeight; y++) {
        for (uint32_t x = 0; x < msdfWidth; x++) {
            // msdfgen bitmaps are bottom-up, WoW font textures are top-down
            const float* pixel = msdf(x, msdfHeight - 1 - y);
            uint32_t idx = y * pitch + x * 3;
            pixelData[idx + 0] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, pixel[0] * 255.0f + 0.5f)));
            pixelData[idx + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, pixel[1] * 255.0f + 0.5f)));
            pixelData[idx + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, pixel[2] * 255.0f + 0.5f)));
        }
    }

    dataPtr->m_data = pixelData;
    dataPtr->m_dataSize = size;
    dataPtr->m_glyphWidth = msdfWidth;
    dataPtr->m_glyphHeight = msdfHeight;
    dataPtr->m_glyphCellWidth = msdfWidth;
    dataPtr->m_glyphPitch = pitch;
    dataPtr->m_glyphAdvance = (face->glyph->metrics.horiAdvance / 64.0f) + 1.0f;
    dataPtr->m_glyphBearing = face->glyph->metrics.horiBearingX * (1.0f / 64.0f);
    dataPtr->m_isMsdf = 1;

    CalculateYOffset(pixelSize, baseline, face, height, &dataPtr->m_yOffset, &dataPtr->m_yStart);

    return true;
}

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gxu/GxuFont.h"

// The D3D9 shader's source is Shaders/FontMsdf_ps2a.hlsl; the D3D11 one's FontMsdf_ps40.hlsl.
#include "Engine/Source/Gxu/GxuFontMsdfShaders.inc"

// Precompiled ps_2_a fallback bytecode for the above adaptive RGSS HLSL shader
static const uint8_t s_msdfPs2aBytecode[] = {
      1,   2, 255, 255, 254, 255, 
     43,   0,  67,  84,  65,  66, 
     28,   0,   0,   0, 127,   0, 
      0,   0,   1,   2, 255, 255, 
      2,   0,   0,   0,  28,   0, 
      0,   0,   0,   1,   0,   0, 
    120,   0,   0,   0,  68,   0, 
      0,   0,   3,   0,   0,   0, 
      1,   0,   2,   0,  72,   0, 
      0,   0,   0,   0,   0,   0, 
     88,   0,   0,   0,   2,   0, 
      0,   0,   1,   0,   2,   0, 
    104,   0,   0,   0,   0,   0, 
      0,   0, 115,  48,   0, 171, 
      4,   0,  12,   0,   1,   0, 
      1,   0,   1,   0,   0,   0, 
      0,   0,   0,   0, 117,  95, 
    111, 117, 116, 108, 105, 110, 
    101,  80,  97, 114,  97, 109, 
    115,   0,   1,   0,   3,   0, 
      1,   0,   4,   0,   1,   0, 
      0,   0,   0,   0,   0,   0, 
    112, 115,  95,  50,  95,  97, 
      0,  77, 105,  99, 114, 111, 
    115, 111, 102, 116,  32,  40, 
     82,  41,  32,  72,  76,  83, 
     76,  32,  83, 104,  97, 100, 
    101, 114,  32,  67, 111, 109, 
    112, 105, 108, 101, 114,  32, 
     49,  48,  46,  49,   0, 171, 
     81,   0,   0,   5,   1,   0, 
     15, 160, 189,  55, 134,  53, 
      0,   0,   0,  60,   0,   0, 
      0,   0,   0,   0,   0,  63, 
     81,   0,   0,   5,   2,   0, 
     15, 160,   0,   0, 128,  63, 
      0,   0, 192,  63,   0,   0, 
     64,  65,  10, 215, 163,  59, 
     81,   0,   0,   5,   3,   0, 
     15, 160,   0,   0, 128,  62, 
      0,   0,  96, 193,   0,   0, 
     96,  65, 205, 204,  76, 189, 
     81,   0,   0,   5,   4,   0, 
     15, 160,   0,   0,   0,   0, 
     41,  92,  15,  61,   0,   0, 
      0,  68,  51,  51, 147, 191, 
     81,   0,   0,   5,   5,   0, 
     15, 160, 205, 204,  76,  63, 
      0,   0,   0,  62,   0,   0, 
    192,  62,   0,   0, 192, 190, 
     81,   0,   0,   5,   6,   0, 
     15, 160,  10, 215, 163, 188, 
      0,   0, 160,  65,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     81,   0,   0,   5,   7,   0, 
     15, 160,   0,   0,   0,  63, 
    111,  18, 131, 186,  73, 146, 
    100,  64, 228,  56,  14,  64, 
     31,   0,   0,   2,   0,   0, 
      0, 128,   0,   0,  15, 144, 
     31,   0,   0,   2,   0,   0, 
      0, 128,   0,   0,   3, 176, 
     31,   0,   0,   2,   0,   0, 
      0, 144,   0,   8,  15, 160, 
     92,   0,   0,   2,   0,   0, 
     15, 128,   0,   0,  68, 176, 
     90,   0,   0,   4,   1,   0, 
      8, 128,   0,   0, 238, 128, 
      0,   0, 238, 128,   1,   0, 
    170, 160,   7,   0,   0,   2, 
      1,   0,   1, 128,   1,   0, 
    255, 128,   6,   0,   0,   2, 
      1,   0,   1, 128,   1,   0, 
      0, 128,  91,   0,   0,   2, 
      2,   0,  15, 128,   0,   0, 
     68, 176,  90,   0,   0,   4, 
      1,   0,   2, 128,   2,   0, 
    238, 128,   2,   0, 238, 128, 
      1,   0, 170, 160,   7,   0, 
      0,   2,   1,   0,   2, 128, 
      1,   0,  85, 128,   6,   0, 
      0,   2,   1,   0,   2, 128, 
      1,   0,  85, 128,  11,   0, 
      0,   3,   3,   0,   8, 128, 
      1,   0,  85, 128,   1,   0, 
      0, 128,   4,   0,   0,   4, 
      1,   0,   1, 128,   3,   0, 
    255, 128,   4,   0, 170, 160, 
      4,   0, 255, 160,   5,   0, 
      0,   3,   1,   0,  17, 128, 
      1,   0,   0, 128,   5,   0, 
      0, 160,   8,   0,   0,   3, 
      1,   0,   2, 128,   0,   0, 
    228, 144,   2,   0,   0, 160, 
      2,   0,   0,   3,   1,   0, 
      2, 128,   1,   0,  85, 128, 
      3,   0, 255, 160,  88,   0, 
      0,   4,   1,   0,   1, 128, 
      1,   0,  85, 128,   1,   0, 
      0, 128,   1,   0, 170, 160, 
     88,   0,   0,   4,   1,   0, 
      2, 128,   1,   0,  85, 128, 
      4,   0,   0, 160,   4,   0, 
     85, 160,   5,   0,   0,   3, 
      3,   0,  15, 128,   0,   0, 
    228, 128,   5,   0, 165, 160, 
     35,   0,   0,   2,   0,   0, 
      3, 128,   0,   0, 238, 128, 
      4,   0,   0,   4,   4,   0, 
     15, 128,   2,   0, 238, 128, 
      5,   0,  95, 160,   3,   0, 
    228, 129,   4,   0,   0,   4, 
      3,   0,  15, 128,   2,   0, 
    228, 128,   5,   0,  95, 161, 
      3,   0, 228, 128,  35,   0, 
      0,   2,   0,   0,  12, 128, 
      2,   0, 228, 128,   2,   0, 
      0,   3,   0,   0,   3, 128, 
      0,   0, 228, 128,   0,   0, 
    238, 128,  11,   0,   0,   3, 
      1,   0,  12, 128,   0,   0, 
     68, 128,   1,   0,   0, 160, 
      4,   0,   0,   4,   0,   0, 
      3, 128,   4,   0, 238, 128, 
      1,   0,   0, 128,   0,   0, 
    228, 176,   4,   0,   0,   4, 
      2,   0,   3, 128,   4,   0, 
    228, 128,   1,   0,   0, 128, 
      0,   0, 228, 176,  66,   0, 
      0,   3,   0,   0,  15, 128, 
      0,   0, 228, 128,   0,   8, 
    228, 160,  66,   0,   0,   3, 
      2,   0,  15, 128,   2,   0, 
    228, 128,   0,   8, 228, 160, 
      2,   0,   0,   3,   0,   0, 
      8, 128,   0,   0,  85, 129, 
      0,   0,   0, 128,  88,   0, 
      0,   4,   0,   0,   3, 128, 
      0,   0, 255, 128,   0,   0, 
    225, 128,   0,   0, 228, 128, 
     10,   0,   0,   3,   2,   0, 
      8, 128,   0,   0, 170, 128, 
      0,   0,  85, 128,  11,   0, 
      0,   3,   4,   0,   1, 128, 
      0,   0,   0, 128,   2,   0, 
    255, 128,   6,   0,   0,   2, 
      0,   0,   1, 128,   1,   0, 
    170, 128,   6,   0,   0,   2, 
      0,   0,   2, 128,   1,   0, 
    255, 128,  90,   0,   0,   4, 
      2,   0,   8, 128,   1,   0, 
     85, 160,   0,   0, 228, 128, 
      1,   0, 170, 160,   5,   0, 
      0,   3,   2,   0,   8, 128, 
      2,   0, 255, 128,   1,   0, 
    255, 160,  11,   0,   0,   3, 
      0,   0,   1, 128,   2,   0, 
    255, 128,   2,   0,   0, 160, 
      1,   0,   0,   2,   5,   0, 
      3, 128,   0,   0, 228, 160, 
      2,   0,   0,   3,   0,   0, 
      6, 128,   5,   0, 196, 129, 
      2,   0, 208, 160,  88,   0, 
      0,   4,   2,   0,   8, 128, 
      0,   0,  85, 128,   2,   0, 
    170, 160,   5,   0,  85, 128, 
     88,   0,   0,   4,   0,   0, 
      2, 128,   0,   0, 170, 128, 
      7,   0, 170, 160,   7,   0, 
    255, 160,   5,   0,   0,   3, 
      2,   0,   8, 128,   0,   0, 
      0, 128,   2,   0, 255, 128, 
      4,   0,   0,   4,   0,   0, 
      4, 128,   2,   0, 255, 128, 
      3,   0,   0, 161,   3,   0, 
    170, 160,   4,   0,   0,   4, 
      0,   0,   8, 128,   2,   0, 
    255, 128,   3,   0,   0, 160, 
      3,   0,  85, 160,   5,   0, 
      0,   3,   2,   0,   8, 128, 
      0,   0, 170, 128,   2,   0, 
    255, 160,  88,   0,   0,   4, 
      2,   0,   8, 128,   0,   0, 
    255, 128,   1,   0, 170, 160, 
      2,   0, 255, 128,   2,   0, 
      0,   3,   2,   0,   8, 128, 
      1,   0,  85, 128,   2,   0, 
    255, 128,   2,   0,   0,   3, 
      0,   0,   4, 128,   2,   0, 
    255, 128,   4,   0,   0, 128, 
      2,   0,   0,   3,   0,   0, 
      8, 128,   4,   0,   0, 128, 
      6,   0,   0, 160,   5,   0, 
      0,   3,   0,   0,  24, 128, 
      0,   0, 255, 128,   6,   0, 
     85, 160,   2,   0,   0,   3, 
      0,   0,   4, 128,   0,   0, 
    170, 128,   1,   0, 255, 161, 
      4,   0,   0,   4,   1,   0, 
     18, 128,   0,   0, 170, 128, 
      0,   0,   0, 128,   1,   0, 
    255, 160,   5,   0,   0,   3, 
      1,   0,   2, 128,   0,   0, 
    255, 128,   1,   0,  85, 128, 
      2,   0,   0,   3,   1,   0, 
      4, 128,   2,   0,  85, 129, 
      2,   0,   0, 128,  88,   0, 
      0,   4,   1,   0,  12, 128, 
      1,   0, 170, 128,   2,   0, 
     20, 128,   2,   0,  68, 128, 
     10,   0,   0,   3,   4,   0, 
      1, 128,   2,   0, 170, 128, 
      1,   0, 255, 128,  11,   0, 
      0,   3,   2,   0,   1, 128, 
      1,   0, 170, 128,   4,   0, 
      0, 128,   2,   0,   0,   3, 
      1,   0,   4, 128,   2,   0, 
    255, 128,   2,   0,   0, 128, 
      2,   0,   0,   3,   1,   0, 
      8, 128,   2,   0,   0, 128, 
      6,   0,   0, 160,   5,   0, 
      0,   3,   1,   0,  24, 128, 
      1,   0, 255, 128,   6,   0, 
     85, 160,   2,   0,   0,   3, 
      1,   0,   4, 128,   1,   0, 
    170, 128,   1,   0, 255, 161, 
      4,   0,   0,   4,   2,   0, 
     17, 128,   1,   0, 170, 128, 
      0,   0,   0, 128,   1,   0, 
    255, 160,   4,   0,   0,   4, 
      1,   0,   2, 128,   2,   0, 
      0, 128,   1,   0, 255, 128, 
      1,   0,  85, 128,   4,   0, 
      0,   4,   2,   0,   3, 128, 
      3,   0, 228, 128,   1,   0, 
      0, 128,   0,   0, 228, 176, 
      4,   0,   0,   4,   3,   0, 
      3, 128,   3,   0, 238, 128, 
      1,   0,   0, 128,   0,   0, 
    228, 176,  66,   0,   0,   3, 
      4,   0,  15, 128,   2,   0, 
    228, 128,   0,   8, 228, 160, 
     66,   0,   0,   3,   3,   0, 
     15, 128,   3,   0, 228, 128, 
      0,   8, 228, 160,   2,   0, 
      0,   3,   3,   0,   8, 128, 
      4,   0,  85, 129,   4,   0, 
      0, 128,  88,   0,   0,   4, 
      2,   0,   3, 128,   3,   0, 
    255, 128,   4,   0, 225, 128, 
      4,   0, 228, 128,  10,   0, 
      0,   3,   3,   0,   8, 128, 
      4,   0, 170, 128,   2,   0, 
     85, 128,  11,   0,   0,   3, 
      1,   0,   1, 128,   2,   0, 
      0, 128,   3,   0, 255, 128, 
      2,   0,   0,   3,   3,   0, 
      8, 128,   2,   0, 255, 128, 
      1,   0,   0, 128,   2,   0, 
      0,   3,   1,   0,   1, 128, 
      1,   0,   0, 128,   6,   0, 
      0, 160,   5,   0,   0,   3, 
      1,   0,  17, 128,   1,   0, 
      0, 128,   6,   0,  85, 160, 
      2,   0,   0,   3,   3,   0, 
      8, 128,   3,   0, 255, 128, 
      1,   0, 255, 161,   4,   0, 
      0,   4,   2,   0,  17, 128, 
      3,   0, 255, 128,   0,   0, 
      0, 128,   1,   0, 255, 160, 
      4,   0,   0,   4,   1,   0, 
      2, 128,   2,   0,   0, 128, 
      1,   0,   0, 128,   1,   0, 
     85, 128,   2,   0,   0,   3, 
      2,   0,   1, 128,   3,   0, 
     85, 129,   3,   0,   0, 128, 
     88,   0,   0,   4,   2,   0, 
      3, 128,   2,   0,   0, 128, 
      3,   0, 225, 128,   3,   0, 
    228, 128,  10,   0,   0,   3, 
      4,   0,   1, 128,   3,   0, 
    170, 128,   2,   0,  85, 128, 
     11,   0,   0,   3,   3,   0, 
      1, 128,   2,   0,   0, 128, 
      4,   0,   0, 128,   2,   0, 
      0,   3,   2,   0,   1, 128, 
      2,   0, 255, 128,   3,   0, 
      0, 128,   2,   0,   0,   3, 
      2,   0,   2, 128,   3,   0, 
      0, 128,   6,   0,   0, 160, 
      5,   0,   0,   3,   2,   0, 
     18, 128,   2,   0,  85, 128, 
      6,   0,  85, 160,   2,   0, 
      0,   3,   2,   0,   1, 128, 
      2,   0,   0, 128,   1,   0, 
    255, 161,   4,   0,   0,   4, 
      2,   0,  20, 128,   2,   0, 
      0, 128,   0,   0,   0, 128, 
      1,   0, 255, 160,   4,   0, 
      0,   4,   1,   0,   2, 128, 
      2,   0, 170, 128,   2,   0, 
     85, 128,   1,   0,  85, 128, 
      5,   0,   0,   3,   1,   0, 
      2, 128,   1,   0,  85, 128, 
      3,   0,   0, 160,   2,   0, 
      0,   3,   2,   0,  12, 128, 
      5,   0,   0, 128,   7,   0, 
     68, 160,   5,   0,   0,   3, 
      0,   0,   2, 128,   0,   0, 
     85, 128,   2,   0, 170, 128, 
     11,   0,   0,   3,   2,   0, 
      4, 128,   0,   0,   0, 128, 
      0,   0,  85, 128,   4,   0, 
      0,   4,   0,   0,   1, 128, 
      0,   0, 170, 128,   2,   0, 
    170, 128,   0,   0,   0, 160, 
      2,   0,   0,   3,   0,   0, 
     17, 128,   0,   0,   0, 128, 
      1,   0, 255, 160,   5,   0, 
      0,   3,   0,   0,   1, 128, 
      0,   0, 255, 128,   0,   0, 
      0, 128,   4,   0,   0,   4, 
      0,   0,   2, 128,   1,   0, 
    170, 128,   2,   0, 170, 128, 
      0,   0,   0, 160,   2,   0, 
      0,   3,   0,   0,  18, 128, 
      0,   0,  85, 128,   1,   0, 
    255, 160,   4,   0,   0,   4, 
      0,   0,   1, 128,   0,   0, 
     85, 128,   1,   0, 255, 128, 
      0,   0,   0, 128,   4,   0, 
      0,   4,   0,   0,   2, 128, 
      3,   0, 255, 128,   2,   0, 
    170, 128,   0,   0,   0, 160, 
      4,   0,   0,   4,   0,   0, 
      4, 128,   2,   0,   0, 128, 
      2,   0, 170, 128,   0,   0, 
      0, 160,   2,   0,   0,   3, 
      0,   0,  22, 128,   0,   0, 
    228, 128,   1,   0, 255, 160, 
      4,   0,   0,   4,   0,   0, 
      1, 128,   0,   0,  85, 128, 
      1,   0,   0, 128,   0,   0, 
      0, 128,   4,   0,   0,   4, 
      0,   0,   1, 128,   0,   0, 
    170, 128,   2,   0,  85, 128, 
      0,   0,   0, 128,   5,   0, 
      0,   3,   0,   0,   1, 128, 
      0,   0,   0, 128,   3,   0, 
      0, 160,  88,   0,   0,   4, 
      0,   0,   1, 128,   2,   0, 
    255, 128,   0,   0,   0, 128, 
      1,   0,  85, 128,   5,   0, 
      0,   3,   0,   0,  14, 128, 
      1,   0,  85, 128,   0,   0, 
    144, 144,  88,   0,   0,   4, 
      1,   0,   7, 128,   2,   0, 
    255, 128,   0,   0, 249, 128, 
      0,   0, 228, 144,   5,   0, 
      0,   3,   1,   0,   8, 128, 
      0,   0,   0, 128,   0,   0, 
    255, 144,   1,   0,   0,   2, 
      0,   8,  15, 128,   1,   0, 
    228, 128, 255, 255,   0,   0
};

// Which bytecode the device takes: D3D9 (and 9Ex) tester's ps_2_a, D3D11 ps_4_0 DXBC, Vulkan none
// (CGxDeviceVulkan recognises the name "UI_MSDF" and binds its own Msdf.frag). Any other device
// gets no shader, MsdfActive() stays false and the fonts keep the stock glyphs.
CGxShader* GxuFontCreateMsdfPixelShader() {
    if (!g_theGxDevicePtr) {
        return nullptr;
    }

    const void* code = nullptr;
    uint32_t codeSize = 0;

    switch (g_theGxDevicePtr->m_api) {
        case GxApi_D3d9:
        case GxApi_D3d9Ex:
            code = s_msdfPs2aBytecode;
            codeSize = sizeof(s_msdfPs2aBytecode);
            break;

        case GxApi_D3d11:
            code = s_msdfPs40Dxbc;
            codeSize = sizeof(s_msdfPs40Dxbc);
            break;

        case GxApi_Vulkan:
            break;

        default:
            return nullptr;
    }

    CGxShader* existing = g_theGxDevicePtr->m_shaderList[GxSh_Pixel].Ptr("UI_MSDF");
    if (existing) {
        existing->m_refCount++;
        return existing;
    }

    CGxShader* shader = g_theGxDevicePtr->m_shaderList[GxSh_Pixel].New("UI_MSDF", 0, 0);
    if (!shader) {
        return nullptr;
    }

    shader->m_refCount = 1;
    shader->m_target = GxSh_Pixel;
    shader->m_code.SetCount(codeSize);

    if (codeSize) {
        memcpy(shader->m_code.m_data, code, codeSize);
    }

    // Created on the device at the first Valid() (MsdfActive).
    shader->m_loaded = 0;
    shader->m_valid = 0;

    return shader;
}

bool MsdfActive() {
    return g_msdfEnabled
        && g_fontMsdfPixelShader && g_fontMsdfPixelShader->Valid()
        && g_fontVertexShader[0] && g_fontVertexShader[0]->Valid()
        && g_fontPixelShader && g_fontPixelShader->Valid();
}
