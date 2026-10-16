// The Vulkan backend's shaders: the engine's CGxShader objects stay the base's (the hash tables, the
// .bls code loaded from the MPQs, the reference counts); a shader becomes valid when this backend
// can draw its bytes -- with a program of its own for the interface's "UI" vertex shader and "UI" /
// "Desaturate" pixel shaders, or with the translated module the manifest lists for the MD5 of its
// code (Tools/VulkanShaders). Every other shader stays invalid, so the engine takes its
// fixed-function paths for it, which the backend's fixed-function programs render; an unknown
// shader is named once in the log and never retried.
//
// The engine asks CGxShader::Valid() before it sets a shader (IShaderCreate on first ask), and the
// interface and the shader effects set shaders without asking; a shader not yet created is created
// at the draw, as CGxDeviceD3d::IBindPixelShader does.
#include <cstdint>
#include <cstring>
#include <new>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Manifest.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Md5.hpp"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {
    namespace manifest  = gxvk::vk::manifest;
    namespace pipelines = gxvk::vk::pipelines;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Info;
    using gxvk::log::Warn;

    // The backend's program for a shader; CGxShader::m_apiSpecific points at one of these for every
    // valid shader: a static one for the shaders the backend owns, one of a table indexed by manifest
    // record for the translated ones.
    struct Program {
        pipelines::VertexProgram   vertex;
        pipelines::FragmentProgram fragment;
        const manifest::Record*    record;
    };

    Program g_uiVertex   = { pipelines::VertexProgram::Ui, pipelines::FragmentProgram::FixedFunction, nullptr };
    Program g_uiPixel    = { pipelines::VertexProgram::FixedFunction, pipelines::FragmentProgram::Ui, nullptr };
    Program g_desaturate = { pipelines::VertexProgram::FixedFunction, pipelines::FragmentProgram::Desaturate, nullptr };

    Program* g_translated      = nullptr;   ///< one per manifest record, made at first need
    uint32_t g_translatedCount = 0;

    struct Stats {
        uint64_t creates;
        uint64_t own;          ///< valid through a program of the backend's own
        uint64_t translated;   ///< valid through a translated module
        uint64_t unknown;      ///< invalid: bytes the manifest does not list
        uint64_t noCode;       ///< invalid: the engine loaded no code
        uint64_t flushes;      ///< constant flushes with a dirty range
        uint64_t pairsCompanion;      ///< draws of a translated vertex shader with no pixel shader, through the record's companion
        uint64_t pairsFixedPixel;     ///< ... through the fixed-function pixel stage of the translated interface
        uint64_t pairsFixedVertex;    ///< draws of a translated pixel shader with no vertex shader, through the fixed-function vertex stage
        uint64_t pairsOwnReplaced;    ///< draws mixing an own program with a translated one, the own one replaced by the interface's fixed stage
    };
    Stats g_stats;

    // The hash key is the name ShaderCreate was given, with ":n" for the n-th of several
    // permutations: the "UI" vertex shader is created with two ("UI:0", "UI:1" -- the second is
    // the stereo one, which this device never enables).
    Program* OwnProgramOf(const CGxShader* shader) {
        const char* name = shader->m_key.m_str;

        if (!name) {
            return nullptr;
        }

        if (shader->m_target == GxSh_Vertex) {
            if (!SStrCmpI(name, "UI", STORM_MAX_STR) || !SStrCmpI(name, "UI:0", STORM_MAX_STR) || !SStrCmpI(name, "UI:1", STORM_MAX_STR)) {
                return &g_uiVertex;
            }
        } else if (shader->m_target == GxSh_Pixel) {
            if (!SStrCmpI(name, "UI", STORM_MAX_STR)) {
                return &g_uiPixel;
            }

            if (!SStrCmpI(name, "Desaturate", STORM_MAX_STR)) {
                return &g_desaturate;
            }
        }

        return nullptr;
    }

    // The translated program of the manifest record with the md5 of the shader's code, of the
    // shader's kind.
    Program* TranslatedProgramOf(CGxShader* shader) {
        if (!manifest::Loaded() || shader->m_code.Count() == 0) {
            return nullptr;
        }

        char md5[33];
        gxvk::md5::Hex(shader->m_code.m_data, shader->m_code.Count(), md5);
        const manifest::Record* record = manifest::ByMd5(md5);

        if (!record) {
            return nullptr;
        }

        const bool vertex = shader->m_target == GxSh_Vertex;

        if ((record->kind == manifest::Kind::Vertex) != vertex) {
            return nullptr;
        }

        if (!g_translated) {
            g_translatedCount = manifest::Count();
            g_translated = new (std::nothrow) Program[g_translatedCount];

            if (!g_translated) {
                g_translatedCount = 0;
                return nullptr;
            }

            for (uint32_t i = 0; i < g_translatedCount; i++) {
                const manifest::Record* r = manifest::At(i);
                g_translated[i].record = r;
                g_translated[i].vertex = r && r->kind == manifest::Kind::Vertex ? pipelines::VertexProgram::Translated : pipelines::VertexProgram::FixedFunction;
                g_translated[i].fragment = r && r->kind == manifest::Kind::Pixel ? pipelines::FragmentProgram::Translated : pipelines::FragmentProgram::FixedFunction;
            }
        }

        const uint16_t index = manifest::IndexOf(record);
        return index < g_translatedCount ? &g_translated[index] : nullptr;
    }

    // The shader the engine set for a stage, created if it never was, or null for the fixed function.
    Program* Bound(CGxDeviceVulkan* device, void* value) {
        CGxShader* shader = static_cast<CGxShader*>(value);

        if (!shader) {
            return nullptr;
        }

        if (!shader->m_loaded) {
            device->IShaderCreate(shader);
        }

        return shader->m_valid ? static_cast<Program*>(shader->m_apiSpecific) : nullptr;
    }

}

// Mirrors CGxDeviceD3d::ShaderCreate (0x006AA130): the base's object, created on the device at once
// for a single permutation; several permutations are created when the engine first asks Valid().
void CGxDeviceVulkan::ShaderCreate(CGxShader** shaders, EGxShTarget target, const char* path, const char* name, uint32_t permutations) {
    CGxDevice::ShaderCreate(shaders, target, path, name, permutations);

    if (permutations == 1 && !shaders[0]->m_loaded) {
        this->IShaderCreate(shaders[0]);
    }
}

// Mirrors CGxDeviceD3d::IShaderCreateVertex / IShaderCreatePixel (0x006AA0D0 / 0x006AA070), with
// "CreateVertexShader succeeded" replaced by "this backend has a program for it": one of its own
// by name, else the translated module of the code's md5. Loaded either way, so the engine does not
// ask again.
void CGxDeviceVulkan::IShaderCreate(CGxShader* shader) {
    shader->m_valid = 0;

    if (!this->m_context) {
        return;
    }

    if (shader->m_target != GxSh_Vertex && shader->m_target != GxSh_Pixel) {
        return;
    }

    shader->m_loaded = 1;
    g_stats.creates++;

    Program* program = OwnProgramOf(shader);

    if (program) {
        g_stats.own++;
    } else {
        program = TranslatedProgramOf(shader);

        if (program) {
            g_stats.translated++;
        } else if (shader->m_code.Count() == 0) {
            g_stats.noCode++;
        } else {
            g_stats.unknown++;
            char md5[33];
            gxvk::md5::Hex(shader->m_code.m_data, shader->m_code.Count(), md5);
            Write(Warn, "vulkan: shaders: no translated module for %s shader \"%s\" (md5 %s, %u bytes); the engine's fixed-function path draws it",
                  shader->m_target == GxSh_Vertex ? "vertex" : "pixel", shader->m_key.m_str ? shader->m_key.m_str : "?", md5, shader->m_code.Count());
        }
    }

    shader->m_apiSpecific = program;

    if (program) {
        shader->m_valid = 1;
    }
}

// Mirrors CGxDeviceD3d::ShaderDestroy (0x006AA190): the API object goes with the last reference;
// here it is a table entry, so only the fields are reset.
void CGxDeviceVulkan::ShaderDestroy(CGxShader*& shader) {
    if (shader->m_refCount == 1) {
        shader->m_apiSpecific = nullptr;
        shader->m_loaded = 0;
    }

    CGxDevice::ShaderDestroy(shader);
}

// Mirrors CGxDeviceD3d::IShaderReload: the code reloaded from the MPQs, the device object made again.
void CGxDeviceVulkan::IShaderReload(CGxShader* shader, const char* path, const char* name) {
    if (shader->m_target != GxSh_Vertex && shader->m_target != GxSh_Pixel) {
        return;
    }

    if (shader->m_refCount == 1) {
        shader->m_apiSpecific = nullptr;
        shader->m_loaded = 0;
    }

    this->IShaderLoad(&shader, static_cast<EGxShTarget>(shader->m_target), path, name, 1);
    this->IShaderCreate(shader);
}

// The base keeps the shadow constants; IShaderConstantsFlush moves their dirty ranges into the
// device's register files at each draw.
void CGxDeviceVulkan::ShaderConstantsSet(EGxShTarget target, uint32_t index, const float* constants, uint32_t count) {
    CGxDevice::ShaderConstantsSet(target, index, constants, count);
}

// The folder the translated shaders are read from: GX_VULKAN_SHADERS in the environment, else
// VulkanShaders beside the executable.
void CGxDeviceVulkan::IShaderFolder(char* folder, size_t size) {
    DWORD length = GetEnvironmentVariableA("GX_VULKAN_SHADERS", folder, static_cast<DWORD>(size));

    if (length > 0 && length < size) {
        return;
    }

    char exe[MAX_PATH];
    length = GetModuleFileNameA(nullptr, exe, sizeof(exe));

    if (length == 0 || length >= sizeof(exe)) {
        SStrCopy(folder, "VulkanShaders", static_cast<int32_t>(size));
        return;
    }

    char* slash = strrchr(exe, '\\');

    if (slash) {
        *slash = '\0';
    }

    SStrPrintf(folder, static_cast<int32_t>(size), "%s\\VulkanShaders", exe);
}

// Mirrors CGxDeviceD3d::IShaderConstantsFlush (0x006A9FE0): the shadow constants' dirty ranges
// become the device's registers (index 1 of s_shadowConstants is the vertex file, 0 the pixel one,
// as ShaderConstantsSet files them), and the ranges are reset.
void CGxDeviceVulkan::IShaderConstantsFlush() {
    ShadowConstants& vs = CGxDevice::s_shadowConstants[1];

    if (vs.dirtyMin <= vs.dirtyMax && vs.dirtyMax < 256) {
        memcpy(&this->m_vertexRegs[vs.dirtyMin * 4], &vs.constants[vs.dirtyMin * 4], (vs.dirtyMax - vs.dirtyMin + 1) * 4 * sizeof(float));
        g_stats.flushes++;
    }

    vs.dirtyMin = 255;
    vs.dirtyMax = 0;

    ShadowConstants& ps = CGxDevice::s_shadowConstants[0];

    if (ps.dirtyMin <= ps.dirtyMax && ps.dirtyMax < 256) {
        memcpy(&this->m_pixelRegs[ps.dirtyMin * 4], &ps.constants[ps.dirtyMin * 4], (ps.dirtyMax - ps.dirtyMin + 1) * 4 * sizeof(float));
        g_stats.flushes++;
    }

    ps.dirtyMin = 255;
    ps.dirtyMax = 0;
}

// The programs of a draw from GxRs_VertexShader and GxRs_PixelShader, and the pairing D3D would
// make of them:
//
//   both translated           the two records (the pipeline checks the pixel module's reads against
//                             the vertex module's outputs)
//   translated vertex, no     the record's generated companion when it has one (the weather), else
//   pixel shader              the fixed-function pixel stage of the translated interface -- D3D runs
//                             its fixed-function stage 0 after any vertex shader
//   translated pixel, no      the fixed-function vertex stage of the translated interface, with the
//   vertex shader             device's matrices
//   an own program beside a   the own one is replaced by the interface's fixed stage (the UI vertex
//   translated one            shader's registers transposed into the matrix the stage reads)
//   neither translated        the backend's own programs (the fixed function, the UI)
//
// The UI vertex program reads its registers c0..c3 from the vertex register file; a fixed-function
// vertex program the device's World * View * ProjNative.
//
// @return uint32 samplerMask : the texture stages the draw must resolve (bit s = GxRs_Texture0 + s)
uint32_t CGxDeviceVulkan::IShaderPrograms(pipelines::Draw& draw) {
    const Program* vs = Bound(this, this->m_appRenderStates[GxRs_VertexShader].m_value.m_data.p);
    const Program* ps = Bound(this, this->m_appRenderStates[GxRs_PixelShader].m_value.m_data.p);

    const bool vsTranslated = vs && vs->record;
    const bool psTranslated = ps && ps->record;

    draw.vertex = pipelines::VertexProgram::FixedFunction;
    draw.fragment = pipelines::FragmentProgram::FixedFunction;
    draw.vertexRecord = nullptr;
    draw.fragmentRecord = nullptr;
    draw.vertexRows = pipelines::kVertexRowsSmall;

    if (!vsTranslated && !psTranslated) {
        if (vs) {
            draw.vertex = vs->vertex;
        }

        if (ps) {
            draw.fragment = ps->fragment;
        }

        if (draw.vertex == pipelines::VertexProgram::Ui) {
            memcpy(draw.xform, &this->m_vertexRegs[0], 16 * sizeof(float));
        } else {
            this->IModelViewProj(draw.xform);
        }

        // The fixed-function pixel stages read texture stages 0 and 1; the UI and Desaturate
        // shaders stage 0 alone.
        return draw.fragment == pipelines::FragmentProgram::FixedFunction ? 3u : 1u;
    }

    if (vsTranslated) {
        draw.vertex = pipelines::VertexProgram::Translated;
        draw.vertexRecord = vs->record;
        draw.vertexRows = vs->record->vconst == pipelines::kVertexRowsLarge ? pipelines::kVertexRowsLarge : pipelines::kVertexRowsSmall;
    } else {
        draw.vertex = pipelines::VertexProgram::FixedFunctionSemantic;

        if (vs && vs->vertex == pipelines::VertexProgram::Ui) {
            // The UI's c0..c3 are the rows of the transposed view-projection: transposed back, they
            // are the matrix the fixed stage multiplies by.
            const float* c = &this->m_vertexRegs[0];

            for (uint32_t row = 0; row < 4; row++) {
                for (uint32_t col = 0; col < 4; col++) {
                    draw.xform[row * 4 + col] = c[col * 4 + row];
                }
            }

            g_stats.pairsOwnReplaced++;
        } else {
            this->IModelViewProj(draw.xform);
        }

        g_stats.pairsFixedVertex++;
    }

    if (psTranslated) {
        draw.fragment = pipelines::FragmentProgram::Translated;
        draw.fragmentRecord = ps->record;
    } else {
        const manifest::Record* companion = vsTranslated ? manifest::Companion(*vs->record) : nullptr;

        if (companion) {
            draw.fragment = pipelines::FragmentProgram::Translated;
            draw.fragmentRecord = companion;
            g_stats.pairsCompanion++;
        } else {
            draw.fragment = pipelines::FragmentProgram::FixedFunctionTranslated;
            g_stats.pairsFixedPixel++;

            if (ps) {
                g_stats.pairsOwnReplaced++;
            }
        }
    }

    return draw.fragmentRecord ? draw.fragmentRecord->samplerMask : 3u;
}
