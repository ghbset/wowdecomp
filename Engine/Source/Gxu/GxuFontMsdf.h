#ifndef ENGINE_SOURCE_GXU_GXUFONTMSDF_H
#define ENGINE_SOURCE_GXU_GXUFONTMSDF_H

#include <cstddef>
#include <cstdint>
#include <freetype/freetype.h>

struct GLYPHBITMAPDATA;

// MSDF configuration constants
static constexpr uint32_t MSDF_ATLAS_SIZE = 512;          // Atlas texture dimensions (512x512)
static constexpr uint32_t MSDF_GLYPH_SIZE = 48;           // Glyph cell size in atlas pixels
static constexpr double   MSDF_PIXEL_RANGE = 3.0;         // SDF range in pixels
static constexpr float    MSDF_INV_ATLAS_SIZE = 1.0f / 512.0f;

// Global MSDF enable flag (toggled by CVar)
extern uint8_t g_msdfEnabled;

// Initialize / shutdown the MSDF subsystem
void MsdfInitialize();
void MsdfShutdown();

// Generate an MSDF glyph for the given codepoint using the given FreeType face.
// Returns true on success. The output GLYPHBITMAPDATA will contain:
//   - m_data: allocated RGB pixel data (3 bytes per pixel, row-major)
//   - m_glyphWidth/Height: the MSDF bitmap dimensions
//   - m_glyphPitch: row stride in bytes
//   - m_glyphAdvance, m_glyphBearing: standard glyph metrics
//   - m_isMsdf: set to 1
// The caller owns m_data and must free it with SMemFree.
bool MsdfGenerateGlyph(FT_Face face, uint32_t pixelSize, uint32_t code, uint32_t baseline, GLYPHBITMAPDATA* dataPtr, uint32_t padding);

// Compute the screenPxRange shader constant for a given font.
// This is: (rendered_pixel_size / msdf_glyph_size) * msdf_pixel_range
float MsdfComputeScreenPxRange(uint32_t renderedPixelSize);

class CGxShader;
// Create the MSDF pixel shader from embedded HLSL (compiling via D3DCompile if available, with static bytecode fallback)
CGxShader* GxuFontCreateMsdfPixelShader();

#endif
