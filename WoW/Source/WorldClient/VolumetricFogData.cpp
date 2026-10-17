// VFOG WP1: Classic/Forever fog data, VFog\fogdata.bin (port of coa-vfog src/fog_data.cpp @ 4e31ddf).
// VFD1 format 4: header, then lights, params, keys, layers, zone lights, zone points, grading curves.
#include "WoW/Source/WorldClient/VolumetricFogModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/IConsole.h"

namespace VolumetricFog {

namespace {

constexpr char kFileMagic[4] = { 'V', 'F', 'D', '1' };
constexpr uint32_t kFormatVersion = 4;
constexpr float kHalfMinutesPerDay = 2880.0f;
constexpr uint32_t kMinimumOutlinePoints = 3;
constexpr uint32_t kClientSelectedLayerFlag = 0x8;
constexpr uint32_t kClientNoiseLayerFlag = 0x4;
constexpr float kNoiseScaleUnitYards = 100.0f;
constexpr uint32_t kNoGradingCurve = 0;
constexpr float kGradingCurveCodeMax = 255.0f;
constexpr int kClearSlot = 0;
constexpr int kStormSlot = 2;

struct Header {
    char magic[4];
    uint32_t version;
    uint32_t lightCount;
    uint32_t paramsCount;
    uint32_t keyCount;
    uint32_t layerCount;
    uint32_t zoneLightCount;
    uint32_t zonePointCount;
    uint32_t gradingCurveCount;
};
static_assert(sizeof(Header) == 36, "fogdata header");

struct Light {
    uint32_t id;
    int32_t mapId;
    float position[3];
    float falloffStart;
    float falloffEnd;
    uint32_t paramsBySlot[kLightParamsSlots];
};
struct Params {
    uint32_t id;
    uint32_t firstKey;
    uint32_t keyCount;
    float glow;
};
struct Key {
    uint16_t halfMinuteOfDay;
    uint16_t layerCount;
    uint32_t firstLayer;
    uint32_t directRgb;
    uint32_t gradingCurve;
};
struct Layer {
    uint32_t diffuseRgb;
    uint32_t emissiveRgb;
    uint32_t shadowEmissiveRgb;
    uint32_t flags;
    float start;
    float density;
    float shadowMultiplier;
    float upperDensity;
    float upperHeight;
    float lowerDensity;
    float lowerHeight;
    float intensity;
    float g;
    float strength;
    float exponent;
    uint32_t noiseFadeRgb;
    float noiseDirections[kAuthoredNoiseOctaves][3];
    float noiseColumn27[kAuthoredNoiseOctaves];
    float noiseColumn28[kAuthoredNoiseOctaves];
    float unmappedToggle;
};
struct GradingCurve {
    uint32_t fileDataId;
    uint8_t entries[kGradingCurveEntries];
};
struct ZoneLight {
    uint32_t id;
    int32_t mapId;
    uint32_t lightId;
    float zMin;
    float zMax;
    uint32_t firstPoint;
    uint32_t pointCount;
};
struct ZonePoint {
    float x;
    float y;
};
static_assert(sizeof(Light) == 60 && sizeof(Params) == 16 && sizeof(Key) == 16 && sizeof(Layer) == 108 &&
                  sizeof(ZoneLight) == 28 && sizeof(ZonePoint) == 8 && sizeof(GradingCurve) == 36,
              "records match the struct formats of coa-vfog tools/convert_classic_fog.py");

struct ZoneOutline {
    const ZoneLight* zone;
    const Light* light;
    float area;
};

struct Contribution {
    const Light* light;
    float weight;
};

struct LightBlend {
    Contribution lights[kMaxBlendedLights];
    int count;

    void Add(const Light* light, float weight) {
        if (weight <= 0.0f) {
            return;
        }
        for (int i = 0; i < this->count; ++i) {
            if (this->lights[i].light == light) {
                this->lights[i].weight += weight;
                return;
            }
        }
        if (this->count < kMaxBlendedLights) {
            this->lights[this->count++] = { light, weight };
        }
    }

    void Scale(float factor) {
        for (int i = 0; i < this->count; ++i) {
            this->lights[i].weight *= factor;
        }
    }
};

// Parsed and blended as coa does; the native port does not grade (docs/VFOG_PORT.md section 1).
struct GradingBlend {
    float curve[kGradingCurveEntries];
    float weight;

    void Add(const GradingBlend& other, float factor) {
        for (int i = 0; i < kGradingCurveEntries; ++i) {
            this->curve[i] += other.curve[i] * factor;
        }
        this->weight += other.weight * factor;
    }

    void AddCurve(const GradingCurve& graded, float factor) {
        for (int i = 0; i < kGradingCurveEntries; ++i) {
            this->curve[i] += static_cast<float>(graded.entries[i]) / kGradingCurveCodeMax * factor;
        }
        this->weight += factor;
    }

    void Result(float* out) const {
        const float identityWeight = std::max(1.0f - this->weight, 0.0f);
        for (int i = 0; i < kGradingCurveEntries; ++i) {
            out[i] = this->curve[i] + identityWeight * static_cast<float>(i) / (kGradingCurveEntries - 1);
        }
    }
};

struct ConditionFog {
    AuthoredLayer layers[kMaxAuthoredLayers];
    int layerCount;
    float directLight[3];
    float directLightPresence;
    float glow;
    float glowPresence;
    GradingBlend grading;
};

bool ContainsRange(size_t size, uint32_t first, uint32_t count) {
    return first <= size && count <= size - first;
}

void UnpackRgb(uint32_t rgb, float* out) {
    out[0] = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(rgb & 0xFF) / 255.0f;
}

void AddScaled(AuthoredLayer& acc, const AuthoredLayer& l, float w) {
    for (int i = 0; i < 3; ++i) {
        acc.diffuse[i] += l.diffuse[i] * w;
        acc.emissive[i] += l.emissive[i] * w;
        acc.shadowEmissive[i] += l.shadowEmissive[i] * w;
    }
    acc.start += l.start * w;
    acc.density += l.density * w;
    acc.shadowMultiplier += l.shadowMultiplier * w;
    acc.upperDensity += l.upperDensity * w;
    acc.upperHeight += l.upperHeight * w;
    acc.lowerDensity += l.lowerDensity * w;
    acc.lowerHeight += l.lowerHeight * w;
    acc.intensity += l.intensity * w;
    acc.g += l.g * w;
    acc.strength += l.strength * w;
    acc.exponent += l.exponent * w;
}

class NoiseBlend {
public:
    void Add(const AuthoredNoise& noise, float weight) {
        const float noiseWeight = noise.presence * weight;
        if (noiseWeight <= 0.0f) {
            return;
        }
        this->m_weight += noiseWeight;
        for (int c = 0; c < 3; ++c) {
            this->m_sum.fade[c] += noise.fade[c] * noiseWeight;
        }
        this->m_sum.unmappedToggle += noise.unmappedToggle * noiseWeight;
        for (int octave = 0; octave < kAuthoredNoiseOctaves; ++octave) {
            const float octaveWeight = noise.octaveShare[octave] * noiseWeight;
            this->m_sum.octaveShare[octave] += octaveWeight;
            this->m_sum.tileYards[octave] += noise.tileYards[octave] * octaveWeight;
            for (int axis = 0; axis < 3; ++axis) {
                this->m_sum.velocity[octave][axis] += noise.velocity[octave][axis] * octaveWeight;
            }
        }
    }

    AuthoredNoise Result(float layerWeight) const {
        AuthoredNoise out = {};
        if (this->m_weight <= 0.0f || layerWeight <= 0.0f) {
            return out;
        }
        out.presence = std::min(this->m_weight / layerWeight, 1.0f);
        for (int c = 0; c < 3; ++c) {
            out.fade[c] = this->m_sum.fade[c] / this->m_weight;
        }
        out.unmappedToggle = this->m_sum.unmappedToggle / this->m_weight;
        for (int octave = 0; octave < kAuthoredNoiseOctaves; ++octave) {
            const float octaveWeight = this->m_sum.octaveShare[octave];
            if (octaveWeight <= 0.0f) {
                continue;
            }
            out.octaveShare[octave] = octaveWeight / this->m_weight;
            out.tileYards[octave] = this->m_sum.tileYards[octave] / octaveWeight;
            for (int axis = 0; axis < 3; ++axis) {
                out.velocity[octave][axis] = this->m_sum.velocity[octave][axis] / octaveWeight;
            }
        }
        return out;
    }

private:
    AuthoredNoise m_sum = {};
    float m_weight = 0.0f;
};

class LayerBlend {
public:
    void Add(const AuthoredLayer& layer, float weight) {
        if (!(layer.flags & kClientSelectedLayerFlag) || layer.density <= 0.0f || weight <= 0.0f) {
            return;
        }
        AddScaled(this->m_sum, layer, weight);
        this->m_noise.Add(layer.noise, weight);
        this->m_presence += weight;
        if (weight > this->m_dominantWeight) {
            this->m_dominantWeight = weight;
            this->m_dominantFlags = layer.flags;
        }
    }

    AuthoredLayer Result() const {
        AuthoredLayer out = {};
        if (this->m_presence <= 0.0f) {
            return out;
        }
        AddScaled(out, this->m_sum, 1.0f / this->m_presence);
        out.density = this->m_sum.density;
        out.flags = this->m_dominantFlags;
        out.noise = this->m_noise.Result(this->m_presence);
        return out;
    }

private:
    AuthoredLayer m_sum = {};
    NoiseBlend m_noise;
    float m_presence = 0.0f;
    float m_dominantWeight = 0.0f;
    uint32_t m_dominantFlags = 0;
};

class PresenceBlend {   // coa GlowBlend / DirectLightBlend: presence-weighted mean of N floats
public:
    void Add(const float* v, int n, float presence, float weight) {
        const float w = presence * weight;
        if (w <= 0.0f) {
            return;
        }
        for (int i = 0; i < n; ++i) {
            this->m_sum[i] += v[i] * w;
        }
        this->m_presence += w;
    }

    float Presence() const {
        return this->m_presence;
    }

    void Result(float* out, int n) const {
        for (int i = 0; i < n; ++i) {
            out[i] = this->m_presence > 0.0f ? this->m_sum[i] / this->m_presence : 0.0f;
        }
    }

private:
    float m_sum[3] = {};
    float m_presence = 0.0f;
};

float SquaredDistanceToSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float ex = bx - ax;
    const float ey = by - ay;
    const float lengthSq = ex * ex + ey * ey;
    const float t = lengthSq > 0.0f ? std::clamp(((px - ax) * ex + (py - ay) * ey) / lengthSq, 0.0f, 1.0f) : 0.0f;
    const float dx = px - (ax + ex * t);
    const float dy = py - (ay + ey * t);
    return dx * dx + dy * dy;
}

bool CrossesRayToPositiveX(float px, float py, float ax, float ay, float bx, float by) {
    return (ay > py) != (by > py) && px < (bx - ax) * (py - ay) / (by - ay) + ax;
}

template <typename T>
void ReadArray(const uint8_t*& cursor, std::vector<T>& out, uint32_t count) {
    out.resize(count);
    if (count != 0) {
        std::memcpy(out.data(), cursor, sizeof(T) * count);
        cursor += sizeof(T) * count;
    }
}

class FogData {
public:
    bool Load(const void* data, size_t size);
    bool Loaded() const {
        return !this->m_lights.empty();
    }
    bool Resolve(int mapId, const float* position, float dayFraction, const LightParamsSelection& selection,
                 AuthoredFog& out) const;

private:
    void Clear();
    static bool IsMapWide(const Light& light);
    static float SphereWeight(const Light& light, const float* position);
    const Light* FindLight(uint32_t id) const;
    const Params* FindParams(uint32_t id) const;
    float EnclosedArea(const ZoneLight& zone) const;
    float ZoneWeight(const ZoneLight& zone, const float* position) const;
    bool ValidateRecords() const;
    bool BuildZoneOutlines();
    bool HasFogInAnySlot(const Light& light) const;
    void CollectMapsWithFog();
    LightBlend BlendLights(int mapId, const float* position) const;
    static AuthoredLayer Unpack(const Layer& layer);
    static AuthoredNoise UnpackNoise(const Layer& layer);
    static ConditionFog BlendConditions(const ConditionFog& a, const ConditionFog& b, float bWeight);
    ConditionFog InterpolateKeys(const Params& params, float halfMinuteOfDay) const;
    GradingBlend InterpolateGrading(const Params& params, float halfMinuteOfDay) const;
    ConditionFog ParamsCondition(const Params* params, float halfMinuteOfDay) const;
    ConditionFog LightConditionFog(const Light& light, float halfMinuteOfDay, const LightParamsSelection& selection) const;

    std::vector<Light> m_lights;
    std::vector<Params> m_params;
    std::vector<Key> m_keys;
    std::vector<Layer> m_layers;
    std::vector<ZoneLight> m_zoneLights;
    std::vector<ZonePoint> m_zonePoints;
    std::vector<GradingCurve> m_gradingCurves;
    std::vector<ZoneOutline> m_zonesLargestFirst;
    std::vector<int32_t> m_mapsWithFog;
};

void FogData::Clear() {
    this->m_lights.clear();
    this->m_params.clear();
    this->m_keys.clear();
    this->m_layers.clear();
    this->m_zoneLights.clear();
    this->m_zonePoints.clear();
    this->m_gradingCurves.clear();
    this->m_zonesLargestFirst.clear();
    this->m_mapsWithFog.clear();
}

// fog_data.cpp:277-328, reading from memory instead of a FILE.
bool FogData::Load(const void* data, size_t size) {
    this->Clear();
    Header h = {};
    if (!data || size < sizeof(Header)) {
        ConsolePrintf("vfog: VFog\\fogdata.bin is too short; derived layers only");
        return false;
    }
    std::memcpy(&h, data, sizeof(h));
    if (std::memcmp(h.magic, kFileMagic, sizeof(kFileMagic)) != 0) {
        ConsolePrintf("vfog: VFog\\fogdata.bin is not VFD1 data; derived layers only");
        return false;
    }
    if (h.version != kFormatVersion) {
        ConsolePrintf("vfog: VFog\\fogdata.bin is format %u, this build reads format %u; derived layers only",
                      h.version, kFormatVersion);
        return false;
    }
    const uint64_t expectedSize = sizeof(Header) + static_cast<uint64_t>(h.lightCount) * sizeof(Light) +
                                  static_cast<uint64_t>(h.paramsCount) * sizeof(Params) +
                                  static_cast<uint64_t>(h.keyCount) * sizeof(Key) +
                                  static_cast<uint64_t>(h.layerCount) * sizeof(Layer) +
                                  static_cast<uint64_t>(h.zoneLightCount) * sizeof(ZoneLight) +
                                  static_cast<uint64_t>(h.zonePointCount) * sizeof(ZonePoint) +
                                  static_cast<uint64_t>(h.gradingCurveCount) * sizeof(GradingCurve);
    bool ok = static_cast<uint64_t>(size) == expectedSize;
    if (ok) {
        const uint8_t* cursor = static_cast<const uint8_t*>(data) + sizeof(Header);
        ReadArray(cursor, this->m_lights, h.lightCount);
        ReadArray(cursor, this->m_params, h.paramsCount);
        ReadArray(cursor, this->m_keys, h.keyCount);
        ReadArray(cursor, this->m_layers, h.layerCount);
        ReadArray(cursor, this->m_zoneLights, h.zoneLightCount);
        ReadArray(cursor, this->m_zonePoints, h.zonePointCount);
        ReadArray(cursor, this->m_gradingCurves, h.gradingCurveCount);
    }
    ok = ok && this->ValidateRecords() && this->BuildZoneOutlines();
    if (!ok) {
        ConsolePrintf("vfog: VFog\\fogdata.bin is invalid; derived layers only");
        this->Clear();
        return false;
    }
    this->CollectMapsWithFog();
    ConsolePrintf("vfog: Classic fog data: %u lights, %u light params, %u keys, %u layers, %u zone lights",
                  h.lightCount, h.paramsCount, h.keyCount, h.layerCount, h.zoneLightCount);
    return true;
}

bool FogData::ValidateRecords() const {
    for (const Params& params : this->m_params) {
        if (!ContainsRange(this->m_keys.size(), params.firstKey, params.keyCount)) {
            return false;
        }
    }
    for (const Key& key : this->m_keys) {
        if (!ContainsRange(this->m_layers.size(), key.firstLayer, key.layerCount) ||
            key.halfMinuteOfDay >= kHalfMinutesPerDay || key.gradingCurve > this->m_gradingCurves.size()) {
            return false;
        }
    }
    return true;
}

bool FogData::BuildZoneOutlines() {
    this->m_zonesLargestFirst.clear();
    for (const ZoneLight& zone : this->m_zoneLights) {
        const Light* light = this->FindLight(zone.lightId);
        if (!light || zone.pointCount < kMinimumOutlinePoints ||
            !ContainsRange(this->m_zonePoints.size(), zone.firstPoint, zone.pointCount)) {
            return false;
        }
        this->m_zonesLargestFirst.push_back({ &zone, light, this->EnclosedArea(zone) });
    }
    std::stable_sort(this->m_zonesLargestFirst.begin(), this->m_zonesLargestFirst.end(),
                     [](const ZoneOutline& a, const ZoneOutline& b) { return a.area > b.area; });
    return true;
}

float FogData::EnclosedArea(const ZoneLight& zone) const {
    const ZonePoint* points = &this->m_zonePoints[zone.firstPoint];
    double twiceArea = 0.0;
    for (uint32_t i = 0, j = zone.pointCount - 1; i < zone.pointCount; j = i++) {
        twiceArea += static_cast<double>(points[j].x) * points[i].y - static_cast<double>(points[i].x) * points[j].y;
    }
    return static_cast<float>(std::fabs(twiceArea) * 0.5);
}

// fog_data.cpp:367-383
float FogData::ZoneWeight(const ZoneLight& zone, const float* position) const {
    if (position[2] < zone.zMin || position[2] > zone.zMax) {
        return 0.0f;
    }
    const ZonePoint* points = &this->m_zonePoints[zone.firstPoint];
    bool inside = false;
    float nearestEdgeSq = std::numeric_limits<float>::max();
    for (uint32_t i = 0, j = zone.pointCount - 1; i < zone.pointCount; j = i++) {
        const ZonePoint& a = points[j];
        const ZonePoint& b = points[i];
        if (CrossesRayToPositiveX(position[0], position[1], a.x, a.y, b.x, b.y)) {
            inside = !inside;
        }
        nearestEdgeSq = std::min(nearestEdgeSq, SquaredDistanceToSegment(position[0], position[1], a.x, a.y, b.x, b.y));
    }
    return inside ? std::min(std::sqrt(nearestEdgeSq) / kZoneLightEdgeFade, 1.0f) : 0.0f;
}

bool FogData::IsMapWide(const Light& light) {
    return light.falloffEnd <= 0.0f && light.position[0] == 0.0f && light.position[1] == 0.0f;
}

// fog_data.cpp:391-402
float FogData::SphereWeight(const Light& light, const float* position) {
    const float dx = position[0] - light.position[0];
    const float dy = position[1] - light.position[1];
    const float dz = position[2] - light.position[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance <= light.falloffStart) {
        return 1.0f;
    }
    if (distance < light.falloffEnd) {
        return (light.falloffEnd - distance) / (light.falloffEnd - light.falloffStart);
    }
    return 0.0f;
}

const Light* FogData::FindLight(uint32_t id) const {
    for (const Light& light : this->m_lights) {
        if (light.id == id) {
            return &light;
        }
    }
    return nullptr;
}

const Params* FogData::FindParams(uint32_t id) const {
    auto it = std::lower_bound(this->m_params.begin(), this->m_params.end(), id,
                               [](const Params& p, uint32_t v) { return p.id < v; });
    return it != this->m_params.end() && it->id == id ? &*it : nullptr;
}

// fog_data.cpp:419-460, time keys
ConditionFog FogData::InterpolateKeys(const Params& params, float halfMinuteOfDay) const {
    ConditionFog fog = {};
    const uint32_t count = params.keyCount;
    if (count == 0) {
        return fog;
    }
    const Key* keys = &this->m_keys[params.firstKey];

    uint32_t next = 0;
    while (next < count && keys[next].halfMinuteOfDay <= halfMinuteOfDay) {
        ++next;
    }
    const uint32_t prev = next == 0 ? count - 1 : next - 1;
    next = next == count ? 0 : next;
    float prevTime = keys[prev].halfMinuteOfDay;
    float nextTime = keys[next].halfMinuteOfDay;
    if (prevTime > halfMinuteOfDay) {
        prevTime -= kHalfMinutesPerDay;
    }
    if (nextTime < halfMinuteOfDay || (nextTime == prevTime && count > 1)) {
        nextTime += kHalfMinutesPerDay;
    }
    const float fraction =
        nextTime > prevTime ? std::clamp((halfMinuteOfDay - prevTime) / (nextTime - prevTime), 0.0f, 1.0f) : 0.0f;

    const Key& a = keys[prev];
    const Key& b = keys[next];
    fog.layerCount = std::min<int>(std::max(a.layerCount, b.layerCount), kMaxAuthoredLayers);
    for (int i = 0; i < fog.layerCount; ++i) {
        LayerBlend blend;
        if (i < a.layerCount) {
            blend.Add(Unpack(this->m_layers[a.firstLayer + i]), 1.0f - fraction);
        }
        if (i < b.layerCount) {
            blend.Add(Unpack(this->m_layers[b.firstLayer + i]), fraction);
        }
        fog.layers[i] = blend.Result();
    }
    float directA[3];
    float directB[3];
    UnpackRgb(a.directRgb, directA);
    UnpackRgb(b.directRgb, directB);
    for (int i = 0; i < 3; ++i) {
        fog.directLight[i] = directA[i] + (directB[i] - directA[i]) * fraction;
    }
    fog.directLightPresence = 1.0f;
    return fog;
}

GradingBlend FogData::InterpolateGrading(const Params& params, float halfMinuteOfDay) const {
    GradingBlend grading = {};
    const Key* first = nullptr;
    const Key* last = nullptr;
    const Key* previous = nullptr;
    const Key* next = nullptr;
    for (uint32_t i = 0; i < params.keyCount; ++i) {
        const Key& key = this->m_keys[params.firstKey + i];
        if (key.gradingCurve == kNoGradingCurve) {
            continue;
        }
        first = first ? first : &key;
        last = &key;
        if (key.halfMinuteOfDay <= halfMinuteOfDay) {
            previous = &key;
        } else if (!next) {
            next = &key;
        }
    }
    if (!first) {
        return grading;
    }
    const float previousTime = previous ? previous->halfMinuteOfDay : last->halfMinuteOfDay - kHalfMinutesPerDay;
    const float nextTime = next ? next->halfMinuteOfDay : first->halfMinuteOfDay + kHalfMinutesPerDay;
    previous = previous ? previous : last;
    next = next ? next : first;
    const float fraction =
        nextTime > previousTime ? std::clamp((halfMinuteOfDay - previousTime) / (nextTime - previousTime), 0.0f, 1.0f)
                                : 0.0f;
    grading.AddCurve(this->m_gradingCurves[previous->gradingCurve - 1], 1.0f - fraction);
    grading.AddCurve(this->m_gradingCurves[next->gradingCurve - 1], fraction);
    return grading;
}

ConditionFog FogData::ParamsCondition(const Params* params, float halfMinuteOfDay) const {
    if (!params) {
        return ConditionFog{};
    }
    ConditionFog condition = this->InterpolateKeys(*params, halfMinuteOfDay);
    condition.glow = params->glow;
    condition.glowPresence = 1.0f;
    condition.grading = this->InterpolateGrading(*params, halfMinuteOfDay);
    return condition;
}

ConditionFog FogData::BlendConditions(const ConditionFog& a, const ConditionFog& b, float bWeight) {
    ConditionFog blended = {};
    blended.layerCount = std::max(a.layerCount, b.layerCount);
    for (int i = 0; i < blended.layerCount; ++i) {
        LayerBlend blend;
        blend.Add(a.layers[i], 1.0f - bWeight);
        blend.Add(b.layers[i], bWeight);
        blended.layers[i] = blend.Result();
    }
    PresenceBlend direct;
    direct.Add(a.directLight, 3, a.directLightPresence, 1.0f - bWeight);
    direct.Add(b.directLight, 3, b.directLightPresence, bWeight);
    direct.Result(blended.directLight, 3);
    blended.directLightPresence = direct.Presence();
    PresenceBlend glow;
    glow.Add(&a.glow, 1, a.glowPresence, 1.0f - bWeight);
    glow.Add(&b.glow, 1, b.glowPresence, bWeight);
    glow.Result(&blended.glow, 1);
    blended.glowPresence = glow.Presence();
    blended.grading.Add(a.grading, 1.0f - bWeight);
    blended.grading.Add(b.grading, bWeight);
    return blended;
}

AuthoredLayer FogData::Unpack(const Layer& layer) {
    AuthoredLayer out = {};
    UnpackRgb(layer.diffuseRgb, out.diffuse);
    UnpackRgb(layer.emissiveRgb, out.emissive);
    UnpackRgb(layer.shadowEmissiveRgb, out.shadowEmissive);
    out.start = layer.start;
    out.density = layer.density;
    out.shadowMultiplier = layer.shadowMultiplier;
    out.upperDensity = layer.upperDensity;
    out.upperHeight = layer.upperHeight;
    out.lowerDensity = layer.lowerDensity;
    out.lowerHeight = layer.lowerHeight;
    out.intensity = layer.intensity;
    out.g = layer.g;
    out.strength = layer.strength;
    out.exponent = layer.exponent;
    out.flags = layer.flags;
    out.noise = UnpackNoise(layer);
    return out;
}

AuthoredNoise FogData::UnpackNoise(const Layer& layer) {
    AuthoredNoise noise = {};
    bool anyOctave = false;
    for (int octave = 0; octave < kAuthoredNoiseOctaves; ++octave) {
        const float scale = layer.noiseColumn27[octave];
        const float speed = layer.noiseColumn28[octave];
        if (scale <= 0.0f) {
            continue;
        }
        anyOctave = true;
        noise.octaveShare[octave] = 1.0f;
        noise.tileYards[octave] = scale * kNoiseScaleUnitYards;
        for (int axis = 0; axis < 3; ++axis) {
            noise.velocity[octave][axis] = layer.noiseDirections[octave][axis] * speed;
        }
    }
    noise.presence = (layer.flags & kClientNoiseLayerFlag) && anyOctave ? 1.0f : 0.0f;
    UnpackRgb(layer.noiseFadeRgb, noise.fade);
    noise.unmappedToggle = layer.unmappedToggle;
    return noise;
}

// fog_data.cpp:496-531, LightParams slots: a forced screen-effect slot wins, else clear blended to storm.
ConditionFog FogData::LightConditionFog(const Light& light, float halfMinuteOfDay,
                                        const LightParamsSelection& selection) const {
    const int effectSlot = selection.screenEffectSlot;
    if (effectSlot >= 0 && effectSlot < kLightParamsSlots && light.paramsBySlot[effectSlot] != 0) {
        return this->ParamsCondition(this->FindParams(light.paramsBySlot[effectSlot]), halfMinuteOfDay);
    }
    const ConditionFog clearFog = this->ParamsCondition(this->FindParams(light.paramsBySlot[kClearSlot]), halfMinuteOfDay);
    const float storm = std::clamp(selection.stormBlend, 0.0f, 1.0f);
    if (storm <= 0.0f || light.paramsBySlot[kStormSlot] == 0) {
        return clearFog;
    }
    const ConditionFog stormFog = this->ParamsCondition(this->FindParams(light.paramsBySlot[kStormSlot]), halfMinuteOfDay);
    return BlendConditions(clearFog, stormFog, storm);
}

// fog_data.cpp:591-647: up to three spheres over a background of map-wide light under nested zone outlines.
LightBlend FogData::BlendLights(int mapId, const float* position) const {
    const auto byWeight = [](const Contribution& a, const Contribution& b) { return a.weight < b.weight; };
    Contribution spheres[kMaxSphereLights] = {};
    int sphereCount = 0;
    const Light* mapWide = nullptr;
    for (const Light& light : this->m_lights) {
        if (light.mapId != mapId) {
            continue;
        }
        if (IsMapWide(light)) {
            if (!mapWide) {
                mapWide = &light;
            }
            continue;
        }
        const float weight = SphereWeight(light, position);
        if (weight <= 0.0f) {
            continue;
        }
        if (sphereCount < kMaxSphereLights) {
            spheres[sphereCount++] = { &light, weight };
        } else {
            auto weakest = std::min_element(spheres, spheres + sphereCount, byWeight);
            if (weakest->weight < weight) {
                *weakest = { &light, weight };
            }
        }
    }
    float sphereWeight = 0.0f;
    for (int i = 0; i < sphereCount; ++i) {
        sphereWeight += spheres[i].weight;
    }
    const float sphereNormalization = sphereWeight > 1.0f ? 1.0f / sphereWeight : 1.0f;

    LightBlend background = {};
    if (mapWide) {
        background.Add(mapWide, 1.0f);
    }
    int nesting = 0;
    for (const ZoneOutline& outline : this->m_zonesLargestFirst) {
        if (outline.zone->mapId != mapId || nesting == kMaxZoneLightNesting) {
            continue;
        }
        const float weight = this->ZoneWeight(*outline.zone, position);
        if (weight <= 0.0f) {
            continue;
        }
        background.Scale(1.0f - weight);
        background.Add(outline.light, weight);
        ++nesting;
    }
    background.Scale(std::max(1.0f - sphereWeight * sphereNormalization, 0.0f));

    LightBlend blend = {};
    for (int i = 0; i < sphereCount; ++i) {
        blend.Add(spheres[i].light, spheres[i].weight * sphereNormalization);
    }
    for (int i = 0; i < background.count; ++i) {
        blend.Add(background.lights[i].light, background.lights[i].weight);
    }
    return blend;
}

// fog_data.cpp:649-700
bool FogData::Resolve(int mapId, const float* position, float dayFraction, const LightParamsSelection& selection,
                      AuthoredFog& out) const {
    std::memset(&out, 0, sizeof(out));
    GradingBlend{}.Result(out.gradingCurve);
    if (this->m_lights.empty() || mapId < 0) {
        return false;
    }

    const LightBlend blend = this->BlendLights(mapId, position);
    for (int i = 0; i < blend.count; ++i) {
        out.coverage += blend.lights[i].weight;
    }
    if (out.coverage <= 0.0f) {
        return false;
    }
    const bool classicFog = std::binary_search(this->m_mapsWithFog.begin(), this->m_mapsWithFog.end(), mapId) &&
                            out.coverage >= kMinimumClassicCoverage;

    const float halfMinuteOfDay = std::fmod(std::max(dayFraction, 0.0f), 1.0f) * kHalfMinutesPerDay;
    LayerBlend layerBlends[kMaxAuthoredLayers];
    PresenceBlend directLight;
    PresenceBlend glow;
    GradingBlend grading = {};
    for (int i = 0; i < blend.count; ++i) {
        const Light& light = *blend.lights[i].light;
        const float weight = blend.lights[i].weight / out.coverage;
        const ConditionFog condition = this->LightConditionFog(light, halfMinuteOfDay, selection);
        glow.Add(&condition.glow, 1, condition.glowPresence, weight);
        grading.Add(condition.grading, weight);
        out.lightIds[out.lightCount] = light.id;
        out.lightWeights[out.lightCount] = weight;
        ++out.lightCount;
        if (!classicFog) {
            continue;
        }
        for (int j = 0; j < condition.layerCount; ++j) {
            layerBlends[j].Add(condition.layers[j], weight);
        }
        directLight.Add(condition.directLight, 3, condition.directLightPresence, weight);
        out.layerCount = std::max(out.layerCount, condition.layerCount);
    }
    for (int j = 0; j < out.layerCount; ++j) {
        out.layers[j] = layerBlends[j].Result();
    }
    out.hasClassicDirectLight = directLight.Presence() > 0.0f;
    directLight.Result(out.classicDirectLight, 3);
    out.hasGlow = glow.Presence() > 0.0f;
    glow.Result(&out.glow, 1);
    out.hasGradingCurve = grading.weight > 0.0f;
    grading.Result(out.gradingCurve);
    return classicFog;
}

bool FogData::HasFogInAnySlot(const Light& light) const {
    for (uint32_t id : light.paramsBySlot) {
        const Params* params = id != 0 ? this->FindParams(id) : nullptr;
        if (params && params->keyCount > 0) {
            return true;
        }
    }
    return false;
}

void FogData::CollectMapsWithFog() {
    this->m_mapsWithFog.clear();
    for (const Light& light : this->m_lights) {
        if (this->HasFogInAnySlot(light)) {
            this->m_mapsWithFog.push_back(light.mapId);
        }
    }
    std::sort(this->m_mapsWithFog.begin(), this->m_mapsWithFog.end());
    this->m_mapsWithFog.erase(std::unique(this->m_mapsWithFog.begin(), this->m_mapsWithFog.end()),
                              this->m_mapsWithFog.end());
}

FogData s_fogData;
bool s_loadTried = false;

}  // namespace

bool LoadFogDataFromMemory(const void* data, size_t size) {
    s_loadTried = true;
    return s_fogData.Load(data, size);
}

bool FogDataLoaded() {
    return s_fogData.Loaded();
}

bool LoadFogData() {
    s_loadTried = true;
    SFileImpl* file = nullptr;
    SFileOpenFile("VFog\\fogdata.bin", &file);
    if (!file) {
        ConsolePrintf("vfog: no VFog\\fogdata.bin; derived layers only");
        return false;
    }
    std::vector<uint8_t> data(SFileGetFileSize(file, nullptr));
    const bool read = data.empty() || SFileReadFile(file, data.data(), static_cast<uint32_t>(data.size()), nullptr, nullptr, 0);
    SFileCloseFile(file);
    if (!read) {
        ConsolePrintf("vfog: VFog\\fogdata.bin could not be read; derived layers only");
        return false;
    }
    return LoadFogDataFromMemory(data.data(), data.size());
}

bool ResolveFogData(int mapId, const float position[3], float dayFraction, const LightParamsSelection& selection,
                    AuthoredFog& out) {
    if (!s_loadTried) {
        LoadFogData();  // first frame that wants the data, if the driver did not load it already
    }
    return s_fogData.Resolve(mapId, position, dayFraction, selection, out);
}

}  // namespace VolumetricFog
