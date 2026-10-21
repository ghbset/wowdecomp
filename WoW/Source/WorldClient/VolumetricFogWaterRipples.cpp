// VFOG water ripples and wakes: native port of coa-vfog src/water_ripples.cpp, water_contacts.cpp, engine_actors.cpp
// and client_ripple_sprites.cpp (@ 4e31ddf, GPLv3). docs/VFOG_PORT.md "Water", ripples.
//
// The units in water around the camera target (players, creatures, pets, mounts: the visible object list, coa
// engine_actors) feed a tracker (coa water_contacts: held footprints along each unit's path, a splash on entering) that
// disturbs a 30 Hz height field: VFogRippleStep ping-pongs two Gr1616F maps (height, previous height) of 0.125 yd
// texels around the camera target, 512^2 (256^2 at vfogWaterQuality 1). VFogWater samples it at s12 with c65-c67.
// Nothing runs without water in view (called from the water pass) or when nothing moved for 15 s (coa quiet steps).
#include "WoW/Source/WorldClient/VolumetricFogWater.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/MapEntity.h"

namespace VolumetricFog {
namespace {

// ---- coa engine_actors.h / water_contacts.h / water_ripples.h / water_renderer.cpp constants -----------------------
constexpr uint32_t kMaxWaterContacts = 32;
constexpr float kWaterContactRange = 48.0f;
constexpr float kMaxWorldCoordinate = 100000.0f;
constexpr float kMaxCollisionRadius = 50.0f;
constexpr float kMaxCollisionHeight = 100.0f;
constexpr float kMinClientRippleDepthLimit = 1.0f;
constexpr float kClientRippleDepthLimitPerHeight = 2.0f;
constexpr uint32_t kSwimmingMovementFlag = 0x200000;
constexpr uint32_t kEntityLiquidFound = 0x20;
constexpr uint32_t kEntitySurfaceCrossesBody = 0x100;

constexpr size_t kMaxDepartedWaterEntries = 256;
constexpr float kWaterSplashRearmDepthPerHeight = 0.15f;
constexpr double kMinWaterSplashIntervalSeconds = 2.0;
constexpr double kStaleWaterTrackSeconds = 0.5;
constexpr double kWaterFootprintFadeSeconds = 1.0;
constexpr double kWaterWakeRampSeconds = 0.3;
constexpr float kMaxWaterWakeSpeed = 40.0f;
constexpr float kWaterWakeStepSlack = 0.5f;
constexpr float kClientSplashDepthPerHeight = 0.4f;
constexpr float kClientFullStrength = 1.0f;
constexpr float kClientDeepestStrength = 0.5f;
constexpr float kFootprintRadiusPerCollisionRadius = 1.25f;
constexpr float kMinFootprintRadius = 0.3f;
constexpr float kMaxFootprintRadius = 6.0f;
constexpr float kFullImmersionDepthPerHeight = 0.25f;
constexpr float kFullyImmersedFootprintLevel = 3.0f;
constexpr float kStandingFootprintShare = 0.2f;
constexpr float kFullWakeSpeed = 4.5f;
constexpr float kSplashRadiusPerCollisionRadius = 3.0f;
constexpr float kMinSplashRadius = 0.75f;
constexpr float kMaxSplashRadius = 9.0f;
constexpr float kEntryImpulsePerStrength = -1.5f;
constexpr double kDepartedEntrySeconds = 30.0;

constexpr float kWaterRippleTexelYards = 0.125f;
constexpr double kWaterRippleStepsPerSecond = 30.0;
constexpr int kMaxWaterRippleStepsPerFrame = 4;
constexpr uint32_t kMaxWaterRippleDisturbances = 48;
constexpr int kWaterRippleTexelsLow = 256;
constexpr int kWaterRippleTexels = 512;
constexpr uint32_t kWaterRippleQuietSteps = 450;
constexpr float kMaxWindowCentreYards = 100000.0f;
constexpr float kHeldFootprint = 1.0f;
constexpr float kAddedImpulse = 0.0f;
constexpr uint32_t kStepRegister = 0;
constexpr uint32_t kFirstSegmentRegister = 1;
constexpr uint32_t kFirstShapeRegister = kFirstSegmentRegister + kMaxWaterRippleDisturbances;
constexpr uint32_t kKeepRegister = 97;      // new: 0 = clear (VFogRippleStep.hlsl)

constexpr double kRippleGapSeconds = 1.0;
constexpr float kRippleEdgeRampPerExtent = 1.0f / 16.0f;
constexpr float kRippleFadeYards = 4.0f;
constexpr double kClientSpriteGraceSeconds = 1.0;

// ---- contacts (coa engine_actors.cpp) ----------------------------------------------------------------------------
struct WaterContact {
    uint64_t guid = 0;
    float position[3] = {};
    float surface = 0.0f;
    float radius = 0.0f;
    float height = 0.0f;
    bool swimming = false;
};

struct WaterContactFrame {
    WaterContact contacts[kMaxWaterContacts];
    uint32_t count = 0;
};

float WaterContactDepth(const WaterContact& contact) {
    return contact.surface - contact.position[2];
}

float ClientRippleDepthLimit(float height) {
    return std::max(kMinClientRippleDepthLimit, kClientRippleDepthLimitPerHeight * height);
}

bool FiniteWithin(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool PlausibleContact(const WaterContact& c) {
    for (float axis : c.position) {
        if (!FiniteWithin(axis, -kMaxWorldCoordinate, kMaxWorldCoordinate)) {
            return false;
        }
    }
    return FiniteWithin(c.surface, -kMaxWorldCoordinate, kMaxWorldCoordinate) && c.radius > 0.0f && FiniteWithin(c.radius, 0.0f, kMaxCollisionRadius)
        && c.height > 0.0f && FiniteWithin(c.height, 0.0f, kMaxCollisionHeight) && WaterContactDepth(c) < ClientRippleDepthLimit(c.height);
}

double SquaredGroundDistance(const WaterContact& c, const float centre[3]) {
    const double dx = static_cast<double>(c.position[0]) - centre[0];
    const double dy = static_cast<double>(c.position[1]) - centre[1];
    return dx * dx + dy * dy;
}

bool Precedes(const WaterContact& a, const WaterContact& b, const float centre[3]) {
    const double aDistance = SquaredGroundDistance(a, centre);
    const double bDistance = SquaredGroundDistance(b, centre);
    return aDistance != bDistance ? aDistance < bDistance : a.guid < b.guid;
}

// coa SelectWaterContact: the 32 nearest plausible contacts within 48 yd, nearest first.
void SelectWaterContact(WaterContactFrame& frame, const WaterContact& contact, const float centre[3]) {
    if (!PlausibleContact(contact) || SquaredGroundDistance(contact, centre) > static_cast<double>(kWaterContactRange) * kWaterContactRange) {
        return;
    }
    uint32_t index = 0;
    while (index < frame.count && !Precedes(contact, frame.contacts[index], centre)) {
        ++index;
    }
    if (index == kMaxWaterContacts) {
        return;
    }
    const uint32_t count = std::min(frame.count + 1, kMaxWaterContacts);
    for (uint32_t i = count - 1; i > index; --i) {
        frame.contacts[i] = frame.contacts[i - 1];
    }
    frame.contacts[index] = contact;
    frame.count = count;
}

// coa WalkVisibleUnits / ReadWaterContact, through the decomp's types: a unit whose map entity found liquid whose surface
// crosses its body (CMapStaticEntity m_staticFlags 0x20 | 0x100, the flags the client's own ripple emitter tests).
void CaptureWaterContacts(const float centre[3], WaterContactFrame& out) {
    out.count = 0;
    ObjectMgr* manager = ClntObjMgrGetCurrent();
    if (!manager) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (!FiniteWithin(centre[i], -kMaxWorldCoordinate, kMaxWorldCoordinate)) {
            return;
        }
    }
    for (CGObject_C* object = manager->m_visibleObjects.Head(); object; object = manager->m_visibleObjects.Next(object)) {
        if (!object->m_obj || !(object->m_obj->m_type & TYPE_UNIT)) {
            continue;
        }
        const CGUnit_C* unit = static_cast<const CGUnit_C*>(object);
        const CMapEntity* entity = unit->m_worldObject;
        if (!entity || !(entity->m_staticFlags & kEntityLiquidFound) || !(entity->m_staticFlags & kEntitySurfaceCrossesBody)) {
            continue;
        }
        const CMovement_C& movement = unit->m_movementData;
        WaterContact contact;
        contact.guid = object->m_obj->m_guid.m_guid;
        const C3Vector& position = movement.m_transportGuid ? entity->m_position : movement.m_position;
        contact.position[0] = position.x;
        contact.position[1] = position.y;
        contact.position[2] = position.z;
        contact.surface = entity->m_liquidHeight;
        contact.radius = movement.m_collisionRadius;
        contact.height = movement.m_collisionHeight;
        contact.swimming = (movement.m_flags & kSwimmingMovementFlag) != 0;
        SelectWaterContact(out, contact, centre);
    }
}

// ---- tracker (coa water_contacts.cpp) ------------------------------------------------------------------------------
struct WaterRippleDisturbance {
    float from[2] = {};
    float to[2] = {};
    float radius = 0.0f;
    float amplitude = 0.0f;
    bool held = false;
};

struct WaterEntryState {
    bool armed = false;
    double splashedAt = -std::numeric_limits<double>::infinity();
};

struct WaterFootprint {
    float from[2] = {};
    double fromSeconds = 0.0;
    float stamped[2] = {};
    double placedAt = 0.0;
    double steppedAt = -std::numeric_limits<double>::infinity();
    float radius = 0.0f;
    float level = 0.0f;
    float motion = 0.0f;
};

struct WaterContactTrack {
    uint64_t guid = 0;
    float position[3] = {};
    double seenAt = 0.0;
    WaterEntryState entry;
    WaterFootprint footprint;
    float splashRadius = 0.0f;
    float pendingSplash = 0.0f;
};

struct DepartedWaterEntry {
    uint64_t guid = 0;
    WaterEntryState entry;
    double seenAt = 0.0;
};

float ClientRippleStrength(float depth, float height) {
    const float limit = ClientRippleDepthLimit(height);
    if (depth <= 0.5f * limit) {
        return kClientFullStrength;
    }
    return std::max(kClientDeepestStrength, kClientDeepestStrength + (limit - depth) / limit);
}

float WaterFootprintRadius(float collisionRadius) {
    return std::clamp(kFootprintRadiusPerCollisionRadius * collisionRadius, kMinFootprintRadius, kMaxFootprintRadius);
}

float WaterFootprintLevel(float depth, float height) {
    const float immersion = std::clamp(depth / (kFullImmersionDepthPerHeight * height), 0.0f, 1.0f);
    return kFullyImmersedFootprintLevel * immersion * ClientRippleStrength(depth, height);
}

float WaterWakeMotion(float speed) {
    return std::clamp(speed / kFullWakeSpeed, 0.0f, 1.0f);
}

float WaterFootprintDepthShare(float motion) {
    return kStandingFootprintShare + (1.0f - kStandingFootprintShare) * motion;
}

double SmoothUnitStep(double x) {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float WaterFootprintFade(double sincePlaced, double sinceSeen) {
    return static_cast<float>(SmoothUnitStep(sincePlaced / kWaterFootprintFadeSeconds) * SmoothUnitStep(1.0 - sinceSeen / kStaleWaterTrackSeconds));
}

bool PlausibleWaterWakeStep(float distance, double seconds) {
    return distance <= kMaxWaterWakeSpeed * std::max(seconds, 0.0) + kWaterWakeStepSlack;
}

float WaterSplashRadius(float collisionRadius) {
    return std::clamp(kSplashRadiusPerCollisionRadius * collisionRadius, kMinSplashRadius, kMaxSplashRadius);
}

float WaterEntryImpulse(float depth, float height) {
    return kEntryImpulsePerStrength * ClientRippleStrength(depth, height);
}

bool TakeEntrySplash(WaterEntryState& entry, const WaterContact& contact, double seconds) {
    if (contact.swimming) {
        entry.armed = false;
        return false;
    }
    const float depth = WaterContactDepth(contact);
    if (depth <= kWaterSplashRearmDepthPerHeight * contact.height) {
        entry.armed = true;
    }
    if (!entry.armed || depth <= kClientSplashDepthPerHeight * contact.height) {
        return false;
    }
    entry.armed = false;
    if (seconds - entry.splashedAt < kMinWaterSplashIntervalSeconds) {
        return false;
    }
    entry.splashedAt = seconds;
    return true;
}

float GroundDistance(const float a[3], const float b[3]) {
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}

void PlaceFootprint(WaterContactTrack& track, const float position[3], double seconds) {
    WaterFootprint& footprint = track.footprint;
    for (int axis = 0; axis < 2; ++axis) {
        footprint.from[axis] = footprint.stamped[axis] = position[axis];
    }
    footprint.fromSeconds = seconds;
    footprint.placedAt = seconds;
    footprint.motion = 0.0f;
    footprint.steppedAt = -std::numeric_limits<double>::infinity();
}

void FollowMotion(WaterFootprint& footprint, float travelled, double stepSeconds) {
    const double elapsed = stepSeconds - footprint.steppedAt;
    footprint.steppedAt = stepSeconds;
    if (!(elapsed > 0.0) || !std::isfinite(elapsed)) {
        return;
    }
    const float ramp = static_cast<float>(elapsed / kWaterWakeRampSeconds);
    const float target = WaterWakeMotion(static_cast<float>(travelled / elapsed));
    footprint.motion += std::clamp(target - footprint.motion, -ramp, ramp);
}

float PathFraction(const WaterContactTrack& track, double stepSeconds) {
    const double span = track.seenAt - track.footprint.fromSeconds;
    if (!(span > 0.0)) {
        return 1.0f;
    }
    return static_cast<float>(std::clamp((stepSeconds - track.footprint.fromSeconds) / span, 0.0, 1.0));
}

WaterRippleDisturbance FootprintStep(WaterContactTrack& track, double stepSeconds) {
    WaterFootprint& footprint = track.footprint;
    const float along = PathFraction(track, stepSeconds);
    WaterRippleDisturbance step;
    for (int axis = 0; axis < 2; ++axis) {
        step.from[axis] = footprint.stamped[axis];
        step.to[axis] = footprint.from[axis] + (track.position[axis] - footprint.from[axis]) * along;
        footprint.stamped[axis] = step.to[axis];
    }
    FollowMotion(footprint, std::hypot(step.to[0] - step.from[0], step.to[1] - step.from[1]), stepSeconds);
    step.radius = footprint.radius;
    step.amplitude = footprint.level * WaterFootprintFade(stepSeconds - footprint.placedAt, stepSeconds - track.seenAt) * WaterFootprintDepthShare(footprint.motion);
    step.held = true;
    return step;
}

class WaterContactTracker {
public:
    void Reset() {
        m_tracks.clear();
        m_departed.clear();
        m_seconds = -1.0;
    }

    void Update(const WaterContactFrame& frame, double seconds) {
        if (m_seconds >= 0.0 && seconds < m_seconds) {
            Reset();
        }
        for (uint32_t i = 0; i < frame.count; ++i) {
            const WaterContact& contact = frame.contacts[i];
            bool created = false;
            WaterContactTrack& track = TrackOf(contact.guid, created);
            if (!created && track.seenAt == seconds) {
                continue;
            }
            Follow(track, contact, seconds, created);
        }
        m_seconds = seconds;
        DropStaleTracks(seconds);
    }

    void PlaceFootprints(double seconds) {
        for (WaterContactTrack& track : m_tracks) {
            PlaceFootprint(track, track.position, seconds);
        }
    }

    uint32_t TakeDisturbances(double stepSeconds, WaterRippleDisturbance* out, uint32_t capacity) {
        uint32_t count = TakeFootprints(stepSeconds, true, out, capacity);
        count += TakeFootprints(stepSeconds, false, out + count, capacity - count);
        return count + TakeSplashes(out + count, capacity - count);
    }

    bool Emitting() const { return !m_tracks.empty(); }

private:
    WaterContactTrack& TrackOf(uint64_t guid, bool& created) {
        for (WaterContactTrack& track : m_tracks) {
            if (track.guid == guid) {
                created = false;
                return track;
            }
        }
        created = true;
        m_tracks.emplace_back();
        m_tracks.back().guid = guid;
        return m_tracks.back();
    }

    WaterEntryState FirstEntryState(uint64_t guid) {
        auto departed = std::find_if(m_departed.begin(), m_departed.end(), [guid](const DepartedWaterEntry& e) { return e.guid == guid; });
        if (departed == m_departed.end()) {
            return {};
        }
        const WaterEntryState entry = departed->entry;
        m_departed.erase(departed);
        return entry;
    }

    void Follow(WaterContactTrack& track, const WaterContact& contact, double seconds, bool created) {
        if (created) {
            track.entry = FirstEntryState(contact.guid);
        }
        if (created || !PlausibleWaterWakeStep(GroundDistance(track.position, contact.position), seconds - track.seenAt)) {
            PlaceFootprint(track, contact.position, seconds);
        } else {
            for (int axis = 0; axis < 2; ++axis) {
                track.footprint.from[axis] = track.position[axis];      // coa MoveFootprint
            }
            track.footprint.fromSeconds = track.seenAt;
        }
        std::copy(contact.position, contact.position + 3, track.position);
        track.seenAt = seconds;
        const float depth = WaterContactDepth(contact);
        track.footprint.radius = WaterFootprintRadius(contact.radius);
        track.footprint.level = WaterFootprintLevel(depth, contact.height);
        track.splashRadius = WaterSplashRadius(contact.radius);
        if (TakeEntrySplash(track.entry, contact, seconds)) {
            track.pendingSplash += WaterEntryImpulse(depth, contact.height);
        }
    }

    void RememberEntry(const WaterContactTrack& track) {
        const DepartedWaterEntry memory = { track.guid, track.entry, track.seenAt };
        auto same = std::find_if(m_departed.begin(), m_departed.end(), [&track](const DepartedWaterEntry& d) { return d.guid == track.guid; });
        if (same != m_departed.end()) {
            *same = memory;
        } else if (m_departed.size() < kMaxDepartedWaterEntries) {
            m_departed.push_back(memory);
        } else {
            *std::min_element(m_departed.begin(), m_departed.end(), [](const DepartedWaterEntry& a, const DepartedWaterEntry& b) { return a.seenAt < b.seenAt; }) = memory;
        }
    }

    void DropStaleTracks(double seconds) {
        const auto stale = [seconds](const WaterContactTrack& track) { return seconds - track.seenAt > kStaleWaterTrackSeconds; };
        for (const WaterContactTrack& track : m_tracks) {
            if (stale(track)) {
                RememberEntry(track);
            }
        }
        m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(), stale), m_tracks.end());
        m_departed.erase(std::remove_if(m_departed.begin(), m_departed.end(), [seconds](const DepartedWaterEntry& d) { return seconds - d.seenAt > kDepartedEntrySeconds; }), m_departed.end());
    }

    uint32_t TakeFootprints(double stepSeconds, bool fresh, WaterRippleDisturbance* out, uint32_t capacity) {
        uint32_t count = 0;
        for (WaterContactTrack& track : m_tracks) {
            if ((track.seenAt == m_seconds) == fresh && count < capacity) {
                out[count++] = FootprintStep(track, stepSeconds);
            }
        }
        return count;
    }

    uint32_t TakeSplashes(WaterRippleDisturbance* out, uint32_t capacity) {
        uint32_t count = 0;
        for (WaterContactTrack& track : m_tracks) {
            if (track.pendingSplash == 0.0f || count == capacity) {
                continue;
            }
            WaterRippleDisturbance& splash = out[count++];
            splash = {};
            for (int axis = 0; axis < 2; ++axis) {
                splash.from[axis] = splash.to[axis] = track.position[axis];
            }
            splash.radius = track.splashRadius;
            splash.amplitude = track.pendingSplash;
            track.pendingSplash = 0.0f;
        }
        return count;
    }

    std::vector<WaterContactTrack> m_tracks;
    std::vector<DepartedWaterEntry> m_departed;
    double m_seconds = -1.0;
};

// ---- simulation (coa water_ripples.cpp) ----------------------------------------------------------------------------
struct Ripples {
    CGxShader* step = nullptr;
    CGxTex* maps[2] = {};
    int texels = 0;
    int current = 0;
    bool mapsCleared = false;
    int origin[2] = {};
    bool running = false;
    double epoch = 0.0;
    double lastScheduled = 0.0;
    int64_t stepsDone = 0;
    uint32_t quietSteps = 0;
    bool unsupported = false;

    WaterContactTracker contacts;
    double lastSeconds = -1.0;          // coa m_lastRippleSeconds
    int mapId = 0;
    double lastShaded = -1.0;           // client sprites (coa ClientRippleSprites m_lastShaded)
    bool mapsFailed = false;            // the last Prepare could not create the maps: keep the client sprites
};

Ripples s_ripples;

void Restart() {
    s_ripples.running = false;
    s_ripples.mapsCleared = false;
    s_ripples.quietSteps = 0;
    s_ripples.current = 0;
}

void DestroyMaps() {
    for (CGxTex*& map : s_ripples.maps) {
        if (map) {
            GxTexDestroy(map);
            map = nullptr;
        }
    }
    s_ripples.texels = 0;
    Restart();
}

bool Prepare(int texels) {
    CGxDevice* device = g_theGxDevicePtr;
    if (s_ripples.unsupported) {
        return false;
    }
    if (!device->Caps().m_rttFormat[GxTex_Gr1616F]) {
        s_ripples.unsupported = true;      // coa: "no filterable 16-bit floating-point render targets"
        return false;
    }
    if (!Shader(s_ripples.step, "VFogRippleStep")) {
        s_ripples.unsupported = true;
        return false;
    }
    if (s_ripples.maps[0] && s_ripples.maps[1] && s_ripples.texels == texels) {
        s_ripples.mapsFailed = false;
        return true;
    }
    DestroyMaps();
    s_ripples.mapsFailed = true;
    for (CGxTex*& map : s_ripples.maps) {
        // Clamped and bilinear (coa: point for the step, which reads texel centres only; linear for the shading).
        CGxTexFlags flags(GxTex_Linear, false, false, 0, 0, true, 1, 0, 0, 0);
        device->TexCreate(GxTex_2d, texels, texels, 0, GxTex_Gr1616F, GxTex_Gr1616F, flags, nullptr, GxTexNullUserFunc, "VFogRipples", map);
        if (!map) {
            DestroyMaps();
            return false;
        }
    }
    s_ripples.texels = texels;
    s_ripples.mapsFailed = false;
    return true;
}

struct Schedule {
    int steps = 0;
    double stepSeconds[kMaxWaterRippleStepsPerFrame] = {};
};

Schedule ScheduleSteps(double seconds) {
    Schedule schedule;
    if (!s_ripples.running || seconds < s_ripples.lastScheduled) {
        Restart();
        s_ripples.running = true;
        s_ripples.epoch = seconds;
        s_ripples.stepsDone = 0;
        s_ripples.lastScheduled = seconds;
        return schedule;
    }
    s_ripples.lastScheduled = seconds;
    const int64_t due = static_cast<int64_t>(std::floor((seconds - s_ripples.epoch) * kWaterRippleStepsPerSecond)) - s_ripples.stepsDone;
    if (due <= 0) {
        return schedule;
    }
    const int64_t dropped = std::max<int64_t>(0, due - kMaxWaterRippleStepsPerFrame);
    s_ripples.stepsDone += dropped;
    schedule.steps = static_cast<int>(due - dropped);
    for (int i = 0; i < schedule.steps; ++i) {
        schedule.stepSeconds[i] = s_ripples.epoch + static_cast<double>(s_ripples.stepsDone + 1 + i) / kWaterRippleStepsPerSecond;
    }
    s_ripples.stepsDone += schedule.steps;
    return schedule;
}

int WindowOrigin(float centre, int texels) {
    return static_cast<int>(std::floor(static_cast<double>(centre) / kWaterRippleTexelYards)) - texels / 2;
}

bool OutsideWindow(const float from[2], const float to[2], float radius, int texels) {
    for (int axis = 0; axis < 2; ++axis) {
        if (std::max(from[axis], to[axis]) < -radius || std::min(from[axis], to[axis]) > texels + radius) {
            return true;
        }
    }
    return false;
}

void DrawStep(CGxTex* target, CGxTex* source, float keep) {
    const float keepRow[4] = { keep, 0.0f, 0.0f, 0.0f };
    SetPs(kKeepRegister, keepRow, 1);
    Bind(0, source);
    GxRenderTargetSet(GxBuffers_Color, target, 0);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, s_ripples.step);
    DrawQuad(target->m_width, target->m_height);
    Bind(0, nullptr);
}

// coa WaterRipples::Step: recentre on the camera target (clearing both maps on a jump), upload the disturbances in
// window texels, one propagation into the other map.
void Step(const float centre[2], const WaterRippleDisturbance* disturbances, uint32_t count) {
    Ripples& r = s_ripples;
    const bool usable = std::fabs(centre[0]) <= kMaxWindowCentreYards && std::fabs(centre[1]) <= kMaxWindowCentreYards;
    const int desired[2] = { usable ? WindowOrigin(centre[0], r.texels) : r.origin[0], usable ? WindowOrigin(centre[1], r.texels) : r.origin[1] };
    int shift[2] = { desired[0] - r.origin[0], desired[1] - r.origin[1] };
    if (!r.mapsCleared || std::abs(shift[0]) >= r.texels || std::abs(shift[1]) >= r.texels) {
        const float clearRow[4] = { 0.0f, 0.0f, 1.0f / r.texels, 0.0f };     // no disturbances
        SetPs(kStepRegister, clearRow, 1);
        DrawStep(r.maps[0], r.maps[1], 0.0f);
        DrawStep(r.maps[1], r.maps[0], 0.0f);
        r.mapsCleared = true;
        shift[0] = shift[1] = 0;
    }
    r.origin[0] = desired[0];
    r.origin[1] = desired[1];

    static float s_rows[1 + 2 * kMaxWaterRippleDisturbances][4];
    float (*segments)[4] = &s_rows[kFirstSegmentRegister];
    float (*shapes)[4] = &s_rows[kFirstShapeRegister];
    uint32_t used = 0;
    for (uint32_t i = 0; i < count && used < kMaxWaterRippleDisturbances; ++i) {
        const WaterRippleDisturbance& d = disturbances[i];
        const float radius = d.radius / kWaterRippleTexelYards;
        if (!(radius > 0.0f) || !std::isfinite(d.amplitude) || (d.amplitude == 0.0f && !d.held)) {
            continue;
        }
        float from[2];
        float to[2];
        for (int axis = 0; axis < 2; ++axis) {
            from[axis] = static_cast<float>(d.from[axis] / kWaterRippleTexelYards - r.origin[axis]);
            to[axis] = static_cast<float>(d.to[axis] / kWaterRippleTexelYards - r.origin[axis]);
        }
        if (OutsideWindow(from, to, radius, r.texels)) {
            continue;
        }
        segments[used][0] = from[0];
        segments[used][1] = from[1];
        segments[used][2] = to[0];
        segments[used][3] = to[1];
        shapes[used][0] = 1.0f / radius;
        shapes[used][1] = d.amplitude;
        shapes[used][2] = d.held ? kHeldFootprint : kAddedImpulse;
        shapes[used][3] = 0.0f;
        ++used;
    }
    s_rows[kStepRegister][0] = static_cast<float>(shift[0]);
    s_rows[kStepRegister][1] = static_cast<float>(shift[1]);
    s_rows[kStepRegister][2] = 1.0f / r.texels;
    s_rows[kStepRegister][3] = static_cast<float>(used);
    SetPs(kStepRegister, s_rows[kStepRegister], 1);
    if (used) {
        SetPs(kFirstSegmentRegister, segments[0], used);
        SetPs(kFirstShapeRegister, shapes[0], used);
    }
    DrawStep(r.maps[1 - r.current], r.maps[r.current], 1.0f);
    r.current = 1 - r.current;
    r.quietSteps = used ? 0 : r.quietSteps + 1;
    if (r.quietSteps >= kWaterRippleQuietSteps) {
        Restart();
    }
}

void Release() {
    DestroyMaps();
    s_ripples.contacts.Reset();
    s_ripples.lastSeconds = -1.0;
}

}  // namespace

// coa WaterRenderer::SimulateRipples + FillRippleConstants.
void RippleFrame(const FrameInputs& in, float strength, int quality, double seconds, const WorldViewport& vp, RippleShading& out) {
    out = {};
    Ripples& r = s_ripples;
    r.lastShaded = seconds;     // the modern water draws this frame (coa g_waterShadedThisFrame)
    bool shaded = false;
    if (strength <= 0.0f) {
        Release();
    } else {
        if (r.lastSeconds >= 0.0 && (seconds < r.lastSeconds || seconds - r.lastSeconds > kRippleGapSeconds || in.mapId != r.mapId)) {
            Restart();
            r.contacts.Reset();
        }
        r.lastSeconds = seconds;
        r.mapId = in.mapId;
        static WaterContactFrame s_frame;
        CaptureWaterContacts(in.camTarget, s_frame);
        r.contacts.Update(s_frame, seconds);
        if ((r.running || r.contacts.Emitting()) && Prepare(quality == 1 ? kWaterRippleTexelsLow : kWaterRippleTexels)) {
            if (!r.running) {
                r.contacts.PlaceFootprints(seconds);
            }
            const Schedule schedule = ScheduleSteps(seconds);
            if (schedule.steps) {
                static SavedPassState s_saved;
                BeginPasses(s_saved);
                const float centre[2] = { in.camTarget[0], in.camTarget[1] };
                WaterRippleDisturbance disturbances[kMaxWaterRippleDisturbances];
                for (int step = 0; step < schedule.steps; ++step) {
                    const uint32_t count = r.contacts.TakeDisturbances(schedule.stepSeconds[step], disturbances, kMaxWaterRippleDisturbances);
                    Step(centre, disturbances, count);
                }
                GxRenderTargetSet(GxBuffers_Color, vp.target, 0);
                EndPasses(s_saved, vp);
            }
            shaded = r.running && r.mapsCleared;
        }
    }
    // coa WaterRipples::Window / FillRippleConstants
    const float extent = r.texels > 0 ? r.texels * kWaterRippleTexelYards : 1.0f;
    const double progress = (seconds - r.epoch) * kWaterRippleStepsPerSecond - static_cast<double>(r.stepsDone);
    out.map = shaded ? r.maps[r.current] : nullptr;
    out.window[0] = static_cast<float>(r.origin[0] * static_cast<double>(kWaterRippleTexelYards));
    out.window[1] = static_cast<float>(r.origin[1] * static_cast<double>(kWaterRippleTexelYards));
    out.window[2] = 1.0f / extent;
    out.window[3] = r.texels > 0 ? 1.0f / r.texels : 0.0f;
    out.shape[0] = static_cast<float>(std::clamp(progress, 0.0, 1.0));
    out.shape[1] = shaded ? strength : 0.0f;
    out.shape[2] = extent;
    out.fade[0] = extent * (0.5f - kRippleEdgeRampPerExtent);
    out.fade[1] = 1.0f / kRippleFadeYards;
    out.fade[2] = kWaterRippleTexelYards;
}

// coa RingsReplaceClientSprites + ClientRippleSprites::Update: while the modern water and its ripples run, plus 1 s.
bool RipplesHideClientSprites(bool allowed) {
    const double now = FrameTime::s_curTimeMs * 0.001;
    return allowed && !s_ripples.unsupported && !s_ripples.mapsFailed && s_ripples.lastShaded >= 0.0 && now >= s_ripples.lastShaded
        && now - s_ripples.lastShaded <= kClientSpriteGraceSeconds;
}

void RippleRelease() {
    Release();
}

void RippleShutdown() {
    Release();
    if (s_ripples.step && g_theGxDevicePtr) {
        g_theGxDevicePtr->ShaderDestroy(s_ripples.step);
    }
    s_ripples = Ripples{};
}

}  // namespace VolumetricFog
