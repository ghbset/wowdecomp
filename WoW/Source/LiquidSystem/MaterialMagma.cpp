#include "Liquid.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Storm/H/storm.h"
#include <cmath>
#include <vector>
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Base/Status.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Source/DB/WowClientDB.h"
#include "WoW/Source/DB/Rec/LiquidTypeRec.h"

// BACKLOG #10 (not in 12340): Cata 4.3.4 ProcMagma for lava, slime and green lava, CVar liquidProcMagma.
// wowdecomp15595 CMaterialMagma::SetupRender (waterDetail >= 1) and wxl-water (Water.cpp kLavaRows,
// Shaders.hpp kLavaPixel). The stock vsLiquidMagma stays; the pixel shader LiquidProcMagma (custom.MPQ)
// does Cata's VS texture matrices per pixel. Anything missing (shader, noise, a layer) = the stock draw.
int32_t Liquid::s_procMagma = 1;

namespace {

// Cata 4.3.4 LiquidType Float[2..11] and Int[2] per row (lava-research-L1 10.2): the 3.3.5a rows carry none of
// them. P2 noise scale, P3 noise angle, P4..P6 layer scales, P7..P9 layer angles (degrees), P11 scroll
// (uv per second); P10 is dead in Cata. kind: texture set 0 magma, 1 slime, 2 green lava.
struct ProcMagmaRow {
    int32_t id;
    float p2, p3, p4, p5, p6, p7, p8, p9, p11;
    uint32_t base;
    int32_t kind;
};

constexpr ProcMagmaRow kProcMagmaRows[] = {
    { 3, 0.025f, 0.0f, 0.55f, 0.45f, 0.35f, 0.0f, 0.0f, 0.0f, 0.05f, 0xFF6600, 0 },           // Magma
    { 4, 0.025f, 0.0f, 0.55f, 0.45f, 0.35f, 0.0f, 0.0f, 0.0f, 0.05f, 0xFF6600, 1 },           // Slime
    { 7, 0.025f, 0.0f, 0.55f, 0.45f, 0.35f, 0.0f, 0.0f, 0.0f, 0.02f, 0xFF6600, 0 },           // Slow Magma
    { 8, 0.025f, 0.0f, 0.55f, 0.45f, 0.35f, 0.0f, 0.0f, 0.0f, 0.05f, 0xFF6600, 1 },           // Slow Slime
    { 11, 0.025f, 0.0f, 0.55f, 0.45f, 0.35f, 0.0f, 0.0f, 0.0f, 0.05f, 0xFF6600, 0 },          // Fast Magma
    { 12, 0.025f, 0.0f, 0.55f, 0.45f, 0.35f, 0.0f, 0.0f, 0.0f, 0.05f, 0xFF6600, 1 },          // Fast Slime
    { 15, 0.15f, 0.0f, 2.5f, 1.5f, 1.3f, 0.0f, 0.0f, 0.0f, 0.01f, 0x68CC00, 2 },              // Green Lava
    { 19, 0.1f, 270.0f, 2.2f, 1.8f, 1.4f, 270.0f, 270.0f, 270.0f, 0.02f, 0xFF6600, 0 },       // WMO Magma
    { 20, 0.1f, 270.0f, 2.2f, 1.8f, 1.4f, 270.0f, 270.0f, 270.0f, 0.03f, 0x000000, 1 },       // WMO Slime
    { 21, 0.025f, 0.0f, 0.55f, 0.45f, 0.35f, 0.0f, 0.0f, 0.0f, 0.05f, 0xFF6600, 1 },          // Naxxramas - Slime
    { 121, 0.025f, 0.0f, 0.55f, 0.45f, 0.35f, 0.0f, 0.0f, 0.0f, 0.05f, 0xFF6600, 0 },         // CoA Black - Magma
    { 141, 0.025f, 0.0f, 0.55f, 0.45f, 0.35f, 0.0f, 0.0f, 0.0f, 0.05f, 0xFF6600, 0 },         // Chamber Magma
};

constexpr uint32_t kNoiseSize = 128;     // XTextures\FX\noise.blob: 128 x 128 x 16 A8R8G8B8
constexpr uint32_t kNoiseDepth = 16;
constexpr uint32_t kNoiseTile = kNoiseSize + 2;
constexpr uint32_t kNoiseAtlas = 4 * kNoiseTile;   // 4 x 4 slices, 520 x 520

CGxShader* s_procPixelShader;
CGxTex* s_procNoise;
CTexture* s_procLayers[3][3];
bool s_procTried;
std::vector<uint32_t> s_procNoisePixels;

void NoiseCallback(EGxTexCommand command, uint32_t, uint32_t, uint32_t, uint32_t, void*, uint32_t& stride, const void*& texels) {
    if (command == GxTex_Latch) {
        texels = s_procNoisePixels.data();
        stride = kNoiseAtlas * 4;
    }
}

// The Gx API has no volume textures: each slice goes into a tile with a 1-texel wrapped border (the
// VolumetricFog "noise atlas" layout), and the shader does the slice lerp.
void LoadProcMagma() {
    s_procTried = true;
    SFileImpl* file = nullptr;
    SFileOpenFile("Textures\\ProcMagma\\noise.blob", &file);
    if (!file) {
        return;
    }
    std::vector<uint32_t> blob(kNoiseSize * kNoiseSize * kNoiseDepth);
    const bool read = SFileGetFileSize(file, nullptr) == blob.size() * 4
        && SFileReadFile(file, blob.data(), static_cast<uint32_t>(blob.size() * 4), nullptr, nullptr, 0);
    SFileCloseFile(file);
    if (!read) {
        return;
    }
    s_procNoisePixels.resize(kNoiseAtlas * kNoiseAtlas);
    for (uint32_t z = 0; z < kNoiseDepth; z++) {
        for (uint32_t j = 0; j < kNoiseTile; j++) {
            const uint32_t y = (j + kNoiseSize - 1) % kNoiseSize;
            for (uint32_t i = 0; i < kNoiseTile; i++) {
                const uint32_t x = (i + kNoiseSize - 1) % kNoiseSize;
                s_procNoisePixels[((z / 4) * kNoiseTile + j) * kNoiseAtlas + (z % 4) * kNoiseTile + i] =
                    blob[(z * kNoiseSize + y) * kNoiseSize + x];
            }
        }
    }
    CGxTexFlags noiseFlags(GxTex_Linear, 1, 1, 0, 0, 0, 1, 0, 0, 0);
    g_theGxDevicePtr->TexCreate(GxTex_2d, kNoiseAtlas, kNoiseAtlas, 0, GxTex_Argb8888, GxTex_Argb8888, noiseFlags, nullptr, NoiseCallback, "ProcMagmaNoise", s_procNoise);

    static const char* const kSets[3] = { "Magma", "Slime", "LavaGreen" };
    CGxTexFlags flags(GxTex_LinearMipLinear, 1, 1, 0, 0, 0, 1, 0, 0, 0);
    for (uint32_t k = 0; k < 3; k++) {
        for (uint32_t i = 0; i < 3; i++) {
            char name[64];
            SStrPrintf(name, sizeof(name), "Textures\\ProcMagma\\%s%u.blp", kSets[k], i);
            CStatus status;
            s_procLayers[k][i] = TextureCreate(name, flags, &status, 0);
        }
    }
}

void DestroyProcMagma() {
    if (s_procPixelShader) {
        g_theGxDevicePtr->ShaderDestroy(s_procPixelShader);
        s_procPixelShader = nullptr;
    }
    if (s_procNoise) {
        GxTexDestroy(s_procNoise);
        s_procNoise = nullptr;
    }
    for (auto& set : s_procLayers) {
        for (CTexture*& layer : set) {
            if (layer) {
                HandleClose(reinterpret_cast<HOBJECT>(layer));
                layer = nullptr;
            }
        }
    }
    s_procNoisePixels.clear();
    s_procTried = false;
}

// The settings carry no LiquidType id: the settings bank is indexed by it.
const ProcMagmaRow* FindProcMagmaRow(const Liquid::CMaterialSettings* settings, ProcMagmaRow& fallback) {
    auto& bank = Liquid::CSettingsBank::s_settings;
    for (uint32_t id = 0; id < bank.Count(); id++) {
        if (bank.m_data[id] != settings) {
            continue;
        }
        for (const ProcMagmaRow& row : kProcMagmaRows) {
            if (row.id == static_cast<int32_t>(id)) {
                return &row;
            }
        }
        // a row the table does not know: the ground magma values, slime set for SoundBank 3
        const LiquidTypeRec* rec = Liquid::s_liquidTypeDB ? Liquid::s_liquidTypeDB->GetRecord(id) : nullptr;
        fallback = kProcMagmaRows[0];
        fallback.kind = rec && rec->m_soundBank == 3 ? 1 : 0;
        return &fallback;
    }
    return nullptr;
}

// Cata SetupRender: Mi = rotZ(angle) . scale(f) . T(scroll(speed), 0), row vectors; kept as (row0.xy, row1.xy)
// in c6 + i and row3.xy in c12..c14.
void SetProcMatrix(float (*c)[4], uint32_t i, float degrees, float scale, float speed) {
    const float a = degrees * 0.017453292f;
    const float cs = std::cos(a) * scale;
    const float sn = std::sin(a) * scale;
    c[i][0] = cs;
    c[i][1] = sn;
    c[i][2] = -sn;
    c[i][3] = cs;
    c[6 + i / 2][(i % 2) * 2] = Liquid::GetTexScrollMtx(C2Vector(speed, 0.0f))->d0;
    c[6 + i / 2][(i % 2) * 2 + 1] = 0.0f;
}

// Binds ProcMagma over the stock state Render has set; false = draw stock.
bool BindProcMagma(Liquid::CMaterialSettings* settings) {
    if (!Liquid::s_procMagma) {
        return false;
    }
    if (!s_procTried) {
        g_theGxDevicePtr->ShaderCreate(&s_procPixelShader, GxSh_Pixel, "Shaders\\Pixel", "LiquidProcMagma", 1);
        LoadProcMagma();
    }
    ProcMagmaRow fallback;
    const ProcMagmaRow* row = FindProcMagmaRow(settings, fallback);
    if (!row || !s_procPixelShader || !s_procPixelShader->Valid() || !s_procNoise) {
        return false;
    }
    CGxTex* layers[3];
    for (uint32_t i = 0; i < 3; i++) {
        CTexture* layer = s_procLayers[row->kind][i];
        layers[i] = layer ? TextureGetGxTex(layer, 0, nullptr) : nullptr;
        if (!layers[i]) {
            return false;
        }
    }

    const float k = Liquid::WorldCoordScale();
    float c[10][4] = {};
    SetProcMatrix(c, 0, 0.0f, 0.05f, row->p11);                         // the noise A/B lerp field
    SetProcMatrix(c, 1, row->p3, k * row->p2 * 0.617f, row->p11 * 0.5f);  // noise uv A
    SetProcMatrix(c, 2, row->p3, k * row->p2 * 0.474f, row->p11 * 0.5f);  // noise uv B
    SetProcMatrix(c, 3, row->p7, row->p4, row->p11);                     // layers 0..2
    SetProcMatrix(c, 4, row->p8, row->p5, row->p11);
    SetProcMatrix(c, 5, row->p9, row->p6, row->p11);
    c[9][0] = static_cast<float>((row->base >> 16) & 0xFF) / 255.0f;
    c[9][1] = static_cast<float>((row->base >> 8) & 0xFF) / 255.0f;
    c[9][2] = static_cast<float>(row->base & 0xFF) / 255.0f;
    c[9][3] = static_cast<float>(OsGetAsyncTimeMs() % 600000) / 600000.0f;

    // the stock VS then hands the raw vertex uv to the PS (Cata's ProcMagma VS reads tc0 unscaled)
    Liquid::s_vsConstants.m_texTransforms[0] = C44Matrix();
    g_theGxDevicePtr->RsSet(GxRs_Texture0, s_procNoise);
    for (uint32_t i = 0; i < 3; i++) {
        g_theGxDevicePtr->RsSet(static_cast<EGxRenderState>(GxRs_Texture1 + i), layers[i]);
    }
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 6, &c[0][0], 10);
    return true;
}

}  // namespace

// STATIC: 0x00D450C0
CDataAllocator Liquid::s_magmaFFPAllocator(sizeof(Liquid::CMaterialMagmaFFP), 8);

// STATIC: 0x00D450AC
CDataAllocator Liquid::s_magmaAllocator(sizeof(Liquid::CMaterialMagma), 8);

// STATIC: 0x00D44C40
int32_t Liquid::CMaterialMagma::s_refCount;

// STATIC: 0x00D44BF4
CGxShader* Liquid::CMaterialMagma::s_vertexShader;

// STATIC: 0x00D44BF0
CGxShader* Liquid::CMaterialMagma::s_pixelShader;

// VERIFY: c1ce3d88 auto
// OFFSET: 0x008A4170
void Liquid::CMaterialMagma::Release() {
    this->~CMaterialMagma();
    Liquid::s_magmaAllocator.PutData(this, nullptr, 0);
}

// VERIFY: a8322b3f auto
// OFFSET: 0x008A4190
Liquid::CMaterialMagma::CMaterialMagma() {
    if (Liquid::CMaterialMagma::s_refCount++ == 0) {
        g_theGxDevicePtr->ShaderCreate(&Liquid::CMaterialMagma::s_vertexShader, GxSh_Vertex, "Shaders\\Vertex", "vsLiquidMagma", 1);
        g_theGxDevicePtr->ShaderCreate(&Liquid::CMaterialMagma::s_pixelShader, GxSh_Pixel, "Shaders\\Pixel", "psLiquidMagma", 1);
    }
}

// VERIFY: 90d48f71 auto
// OFFSET: [0x008A4200, 0x008A48A0]
Liquid::CMaterialMagma::~CMaterialMagma() {
    if (Liquid::CMaterialMagma::s_refCount-- == 1) {
        if (Liquid::CMaterialMagma::s_vertexShader) {
            g_theGxDevicePtr->ShaderDestroy(Liquid::CMaterialMagma::s_vertexShader);
        }

        if (Liquid::CMaterialMagma::s_pixelShader) {
            g_theGxDevicePtr->ShaderDestroy(Liquid::CMaterialMagma::s_pixelShader);
        }

        DestroyProcMagma();   // BACKLOG #10
    }
}

// VERIFY: 98a0bedc auto
// OFFSET: 0x008A4260
void Liquid::CMaterialMagmaFFP::Release() {
    this->~CMaterialMagmaFFP();
    Liquid::s_magmaFFPAllocator.PutData(this, nullptr, 0);
}

// VERIFY: 7e8b3bec auto
// OFFSET: 0x008A4870
Liquid::CMaterialMagma* Liquid::CMaterialMagma::Create() {
    void* mem = Liquid::s_magmaAllocator.GetData(0, ".?AVCMaterialMagma@Liquid@@", SLOG_OBJECT);
    return mem ? new (mem) Liquid::CMaterialMagma() : nullptr;
}

// VERIFY: db3efd66 auto
// OFFSET: 0x008A48D0
Liquid::CMaterialMagmaFFP* Liquid::CMaterialMagmaFFP::Create() {
    void* mem = Liquid::s_magmaFFPAllocator.GetData(0, ".?AVCMaterialMagmaFFP@Liquid@@", SLOG_OBJECT);
    return mem ? new (mem) Liquid::CMaterialMagmaFFP() : nullptr;
}

// VERIFY: b5c1ce43 manual
// OFFSET: 0x008A6090
void Liquid::CMaterialMagma::Render(Liquid::IEnvironment* environment, Liquid::IGeomFactory* geomFactory, Liquid::IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, Liquid::CMaterialSettings* settings) {
    CGxTex* texture0 = settings->GetTexture(0, 1250);

    if (!texture0) {
        return;
    }

    g_theGxDevicePtr->RsPush();
    g_theGxDevicePtr->RsSet(GxRs_Culling, 0);
    g_theGxDevicePtr->RsSet(GxRs_Fog, 1);

    Liquid::s_vsConstants.m_projection = g_theGxDevicePtr->m_projNative;

    if (g_theGxDevicePtr->DeviceApi() == GxApi_OpenGl) {
        Liquid::s_vsConstants.m_projection.c0 *= -1.0f;
        Liquid::s_vsConstants.m_projection.c1 *= -1.0f;
        Liquid::s_vsConstants.m_projection.c2 *= -1.0f;
        Liquid::s_vsConstants.m_projection.c3 *= -1.0f;
    }

    C44Matrix matrix;
    Liquid::SetShaderTransforms(cameraPos, geomFactory->SetTransform(const_cast<C44Matrix*>(transform)) ? matrix : *transform);

    CM2Lighting lighting(*bounds);
    environment->SelectLights(&lighting);
    Liquid::SetShaderLighting(lighting);

    C2Vector speed;
    speed.x = settings->GetFloat(0);
    speed.y = settings->GetFloat(1);
    C44Matrix texMatrix = *Liquid::GetTexScrollMtx(speed);
    texMatrix.Scale(Liquid::WorldCoordScale());
    Liquid::s_vsConstants.m_texTransforms[0] = texMatrix;

    g_theGxDevicePtr->RsSet(GxRs_Texture0, texture0);
    const bool proc = BindProcMagma(settings);   // BACKLOG #10: Cata ProcMagma, or stock
    Liquid::SetShaderConstants();
    g_theGxDevicePtr->RsSet(GxRs_VertexShader, Liquid::CMaterialMagma::s_vertexShader);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, proc ? s_procPixelShader : Liquid::CMaterialMagma::s_pixelShader);

    CGxBuf* vertexBuf = nullptr;
    CGxBuf* indexBuf = nullptr;
    CGxBatch batch;
    batch.m_primType = GxPrims_Last;
    batch.m_start = 0;
    batch.m_count = 0;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 0;
    geomFactory->GetBuffers(GxVBF_PCT, &vertexBuf, &indexBuf, &batch);
    GxPrimVertexPtr(vertexBuf, GxVBF_PCT);
    g_theGxDevicePtr->PrimIndexPtr(indexBuf);
    g_theGxDevicePtr->Draw(&batch, 1);
    g_theGxDevicePtr->RsPop();
}

// VERIFY: 35b378c5 manual
// OFFSET: 0x008A6350
void Liquid::CMaterialMagmaFFP::Render(Liquid::IEnvironment* environment, Liquid::IGeomFactory* geomFactory, Liquid::IWaveManager* waveManager, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, Liquid::CMaterialSettings* settings) {
    CGxTex* texture0 = settings->GetTexture(0, 1250);

    if (!texture0) {
        return;
    }

    g_theGxDevicePtr->RsPush();
    g_theGxDevicePtr->XformPush(GxXform_World);
    g_theGxDevicePtr->XformPush(GxXform_Tex0);
    g_theGxDevicePtr->RsSet(GxRs_Culling, 0);

    C44Matrix world = *transform;

    if (Liquid::UseCameraRelativeCoords()) {
        world.d0 -= cameraPos.x;
        world.d1 -= cameraPos.y;
        world.d2 -= cameraPos.z;
    }

    GxXformSet(GxXform_World, world);

    CM2Lighting lighting(*bounds);
    environment->SelectLights(&lighting);
    lighting.SetupGxLights(&cameraPos);
    g_theGxDevicePtr->RsSet(GxRs_Lighting, 0);
    lighting.SetupGxFog();

    g_theGxDevicePtr->RsSet(GxRs_Texture0, texture0);

    C2Vector speed;
    speed.x = settings->GetFloat(0);
    speed.y = settings->GetFloat(1);
    GxXformSet(GxXform_Tex0, *Liquid::GetTexScrollMtx(speed));
    g_theGxDevicePtr->RsSet(GxRs_TextureShader0, 1);

    CGxBuf* vertexBuf = nullptr;
    CGxBuf* indexBuf = nullptr;
    CGxBatch batch;
    batch.m_primType = GxPrims_Last;
    batch.m_start = 0;
    batch.m_count = 0;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 0;
    geomFactory->GetBuffers(GxVBF_PCT, &vertexBuf, &indexBuf, &batch);
    GxPrimVertexPtr(vertexBuf, GxVBF_PCT);
    g_theGxDevicePtr->PrimIndexPtr(indexBuf);
    g_theGxDevicePtr->Draw(&batch, 1);

    GxXformPop(GxXform_Tex0);
    GxXformPop(GxXform_World);
    g_theGxDevicePtr->RsPop();
}
