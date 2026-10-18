#include "WoW/Source/WorldClient/TerrainSunShadow.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapLowDetail.h"

namespace TerrainSunShadow {

namespace {

constexpr int kSize = 256;                  // texels per side, one per WDL outer vertex
constexpr float kCell = 33.333332f;         // WDL outer vertex spacing (MapLowDetail.cpp)
constexpr float kGridOrigin = 512.0f;       // 17066.666 / kCell: world x/y 0 in WDL vertices
constexpr float kNone = -30000.0f;          // no land (no WDL tile), and "nothing blocks the light"
constexpr float kSoftYards = 16.0f;         // fully dark this far under the lit height
constexpr int kRecentre = 16;               // texels the camera may drift before a rebuild
constexpr float kTurnCos = 0.99999f;        // ~0.26 degrees of light turn before a rebuild
constexpr float kMarchYards = 8000.0f;      // the CPU test's reach
constexpr float kTopOfWorld = 4000.0f;      // no land above this

CVar* s_cvar;
CVar* s_maskCvar;       // shadowMask (Legion survey 2.7)
CVar* s_softCvar;       // shadowSoft
CGxTex* s_tex;
float s_lit[kSize * kSize];
uint32_t s_texels[kSize * kSize];
float s_light[3];
int s_r0 = 0, s_c0 = 0;
const void* s_wdl;
bool s_built;

float Vertex(int r, int c) {
    if (r < 0 || c < 0 || r > 1024 || c > 1024) {
        return kNone;
    }
    const int tr = std::min(r >> 4, 63), tc = std::min(c >> 4, 63);
    const CMapAreaLow* area = CMap::s_lowDetail.m_areas[tr * 64 + tc];
    if (!area || !area->m_heights) {
        return kNone;
    }
    return area->m_heights[(r - tr * 16) * 17 + (c - tc * 16)];
}

// Bilinear over the WDL outer vertices; r runs down world x, c down world y (CMap::BuildLowDetailGridVertices).
float HeightAt(float r, float c) {
    const float fr = std::floor(r), fc = std::floor(c);
    const int ir = static_cast<int>(fr), ic = static_cast<int>(fc);
    const float h00 = Vertex(ir, ic), h01 = Vertex(ir, ic + 1), h10 = Vertex(ir + 1, ic), h11 = Vertex(ir + 1, ic + 1);
    if (h00 == kNone || h01 == kNone || h10 == kNone || h11 == kNone) {
        return std::max(std::max(h00, h01), std::max(h10, h11));   // a coast of missing tiles: the highest known
    }
    const float tr = r - fr, tc = c - fc;
    return (h00 * (1 - tc) + h01 * tc) * (1 - tr) + (h10 * (1 - tc) + h11 * tc) * tr;
}

uint16_t Half(float f) {    // normal range only (|f| <= 30000, rounded to nearest)
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 112;
    if (exponent <= 0) {
        return static_cast<uint16_t>(sign);
    }
    const uint32_t bits = (static_cast<uint32_t>(exponent) << 10) + (((x & 0x7FFFFFu) + 0x1000u) >> 13);
    return static_cast<uint16_t>(sign | std::min(bits, 0x7BFFu));
}

void Callback(EGxTexCommand command, uint32_t width, uint32_t, uint32_t, uint32_t, void*, uint32_t& stride, const void*& texels) {
    if (command == GxTex_Latch) {
        texels = s_texels;
        stride = width * 4;
    }
}

// lit(p) = max over the land toward the light of (height - rise along the way): the lowest z the light reaches.
// Swept one texel line at a time from the light's side: lit(p) = max(H(q), lit(q)) - drop, q = p one step toward the
// light along the major axis (the minor offset |off| <= 1 keeps q in the previous line, already done).
void Build(const float L[3]) {
    const float h = std::sqrt(L[0] * L[0] + L[1] * L[1]);
    const float rise = std::max(L[2], 0.005f) / std::max(h, 1e-4f);    // tan(elevation)
    const float dR = h > 1e-4f ? -L[0] / h : 0.0f;    // grid r/c grow as world x/y fall
    const float dC = h > 1e-4f ? -L[1] / h : 1.0f;
    const bool rowsMajor = std::fabs(dR) >= std::fabs(dC);
    const float m = std::max(std::fabs(dR), std::fabs(dC));
    const int step = (rowsMajor ? dR : dC) > 0 ? 1 : -1;
    const float off = (rowsMajor ? dC : dR) / m;
    const float drop = kCell / m * rise;
    auto at = [&](int a, int b) -> float& { return rowsMajor ? s_lit[a * kSize + b] : s_lit[b * kSize + a]; };

    for (int n = 0; n < kSize; n++) {
        const int a = step > 0 ? kSize - 1 - n : n;
        const int na = a + step;
        for (int b = 0; b < kSize; b++) {
            const float nb = b + off;
            const float land = rowsMajor ? HeightAt(static_cast<float>(s_r0 + na), s_c0 + nb) : HeightAt(s_r0 + nb, static_cast<float>(s_c0 + na));
            float past = kNone;
            const int b0 = static_cast<int>(std::floor(nb));
            if (na >= 0 && na < kSize && b0 >= 0 && b0 + 1 < kSize) {
                const float t = nb - b0;
                past = at(na, b0) * (1 - t) + at(na, b0 + 1) * t;
            } else if (na >= 0 && na < kSize && b0 >= 0 && b0 < kSize) {
                past = at(na, b0);
            }
            at(a, b) = std::max(std::max(land, past) - drop, kNone);
        }
    }
    for (int i = 0; i < kSize * kSize; i++) {
        const int r = i / kSize, c = i % kSize;
        const bool border = r == 0 || c == 0 || r == kSize - 1 || c == kSize - 1;     // the clamp past the grid: lit
        s_texels[i] = Half(border ? kNone : s_lit[i]);   // R = lit height, G unused
    }
}

}  // namespace

void RegisterCVars() {
    s_cvar = CVar::Register("gfxTerrainShadow", "The land (near terrain and the far WDL horizon) blocks the sun for the volumetric fog's sun shafts and glow and for the sun glare: 1 = on (default), 0 = off", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_maskCvar = CVar::Register("shadowMask", "Mountains (near terrain and the far WDL horizon) cast the sun's shadow onto the world: terrain, buildings and models (needs gfxTerrainShadow): 1 = on (default), 0 = off", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    s_softCvar = CVar::Register("shadowSoft", "Softness of the mountains' shadow edge (shadowMask), 0 = hard .. 4 = very soft, default 1", 1, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

bool MaskWanted() {
    return (!s_maskCvar || s_maskCvar->m_intValue != 0) && Enabled();
}

float MaskSoftYards() {
    // shadowSoft 0..4: the land's penumbra, yards under the lit height to full dark (kSoftYards each); 0 = 2 yd.
    const float soft = s_softCvar ? s_softCvar->m_floatValue : 1.0f;
    return std::max(kSoftYards * std::clamp(soft, 0.0f, 4.0f), 2.0f);
}

bool Enabled() {
    return (!s_cvar || s_cvar->m_intValue != 0) && CMap::s_lowDetail.m_data != nullptr;
}

bool Update(const float toLight[3], const float camPos[3], CGxTex*& tex, float row[4]) {
    tex = nullptr;
    if (!Enabled() || toLight[2] <= 0.0f || !g_theGxDevicePtr) {
        return false;
    }
    const int r = static_cast<int>(std::floor(kGridOrigin - camPos[0] / kCell)) - kSize / 2;
    const int c = static_cast<int>(std::floor(kGridOrigin - camPos[1] / kCell)) - kSize / 2;
    const float turn = toLight[0] * s_light[0] + toLight[1] * s_light[1] + toLight[2] * s_light[2];
    if (!s_built || !s_tex || s_wdl != CMap::s_lowDetail.m_data || turn < kTurnCos || std::abs(r - s_r0) > kRecentre || std::abs(c - s_c0) > kRecentre) {
        s_r0 = r;
        s_c0 = c;
        std::memcpy(s_light, toLight, sizeof(s_light));
        s_wdl = CMap::s_lowDetail.m_data;
        Build(toLight);
        if (!s_tex) {
            CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 0, 1, 0, 0, 0);
            g_theGxDevicePtr->TexCreate(GxTex_2d, kSize, kSize, 0, GxTex_Gr1616F, GxTex_Gr1616F, flags, nullptr, Callback, "TerrainSunShadow", s_tex);
        }
        if (s_tex) {
            GxTexUpdate(s_tex, 0, 0, kSize, kSize, 1);
        }
        s_built = s_tex != nullptr;
    }
    if (!s_tex) {
        return false;
    }
    tex = s_tex;
    row[0] = (kGridOrigin - s_c0 + 0.5f) / kSize;
    row[1] = (kGridOrigin - s_r0 + 0.5f) / kSize;
    row[2] = 1.0f / (kCell * kSize);
    row[3] = 1.0f / kSoftYards;
    return true;
}

float Visibility(const float pos[3], const float toward[3]) {
    if (!Enabled() || toward[2] <= 0.0f) {
        return 1.0f;
    }
    const float h = std::sqrt(toward[0] * toward[0] + toward[1] * toward[1]);
    if (h < 1e-4f) {
        return 1.0f;
    }
    const float rise = toward[2] / h;
    for (float s = kCell * 0.5f; s < kMarchYards; s += kCell * 0.5f) {
        const float z = pos[2] + s * rise;
        if (z > kTopOfWorld) {
            break;
        }
        const float x = pos[0] + toward[0] / h * s, y = pos[1] + toward[1] / h * s;
        if (HeightAt(kGridOrigin - x / kCell, kGridOrigin - y / kCell) > z + 2.0f) {
            return 0.0f;
        }
    }
    return 1.0f;
}

void Release() {
    if (s_tex) {
        GxTexDestroy(s_tex);
        s_tex = nullptr;
    }
    s_built = false;
}

}  // namespace TerrainSunShadow
