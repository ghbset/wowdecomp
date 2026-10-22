// Not stock: Legion 7.3.5's ActionCam (over-shoulder, dynamic pitch, head movement, target focus) and Legion's zoom
// smoothing, ported in-tree from wxl-camera (/root/wxl-camera src/Camera.cpp, a WarcraftXL plugin that detoured the
// stock functions). Here the decomp calls in directly; one frame:
//
//   CalcTargetCamera 0x00606F90   FrameBegin: frame context (unit, dt); with everything off nothing else runs
//     after UpdateTargetSmoothing AfterSmoothing: Legion zoom spring on the drawn distance (wxl_cameraLegionZoom)
//     CollideCameraWithWorld 0x00605D60
//       before                    CollideBegin: dynamic-pitch state, shoulder offset, head movement -> offset
//       boom ray                  BoomShift / BoomHit: ray end shifted by the shoulder offset, fraction recorded
//       near-plane sweep          SweepOffset: the shoulder offset cut by the boom hit
//       after                     CollideEnd: Legion's scaling by hit fraction and dist/dist0, lateral trace
//     end                         FrameEnd: look-only rotations of m_facing (dynamic pitch, then target focus)
//   CanSmoothTargetFacing 0x005FFDE0 RefusePivot: smart pivot refused below the cutoff while dynamic pitch is on
//
// Addresses and fields: /root/RE/camera/port-335-reference.md. Algorithms: /root/RE/camera/legion/re-*.md (maths in
// ActionCamMath.h, checked by Tools/ActionCamCheck). Places that knowingly differ from Legion are marked DEVIATION.
#include "ActionCam.h"
#include "ActionCamMath.h"
#include "Camera.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Model2/M2Model.h"
#include "WoW/Source/Object/Movement.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/World.h"

using namespace ActionCamMath;

namespace {

// m_state bits (Camera.h / wiki world-camera)
constexpr uint32_t kFlagLocked    = 0x8;             // first person / telescope: no collision
constexpr uint32_t kFlagMoving    = 0x200;           // movement input held
constexpr uint32_t kFlagBoomHit   = 0x10000;         // returned by CollideCameraWithWorld
constexpr uint32_t kFlagHeightHit = 0x20000;
constexpr uint32_t kFlagDistBlend = 0x4000000;       // stock timed distance blend running

constexpr uint32_t kUnitFlagTaxi  = 0x100000;
constexpr uint32_t kMoveFalling   = 0x1000;
constexpr uint32_t kMoveSwimming  = 0x200000;
constexpr uint32_t kMoveFlying    = 0x2000000;
constexpr uint32_t kMoveSlowFall  = 0x20000000;

Vec3 V(const C3Vector& c) { return { c.x, c.y, c.z }; }
C3Vector C(Vec3 v) { return C3Vector(v.x, v.y, v.z); }

// ---- settings: the retail CVars -----------------------------------------------------------------------------
enum Cv {
    kOverShoulder, kDynPitch, kPad, kPadFlying, kPadDownScale, kSmartPivotCutoff,
    kHeadStrength, kHeadRangeScale, kHeadMoving, kHeadStanding, kHeadMovingDamp, kHeadStandingDamp,
    kHeadFirstPersonDamp, kHeadDeadZone,
    kFocusEnemy, kFocusEnemyPitch, kFocusEnemyYaw, kFocusInteract, kFocusInteractPitch, kFocusInteractYaw,
    kLegionZoom, kSizeFactor, kCvCount
};
struct CVarDef { const char* name; const char* def; const char* help; };
constexpr CVarDef kCVars[kCvCount] = {
    { "test_cameraOverShoulder", "0", "Camera sideways offset in yards, + = camera to the right" },
    { "test_cameraDynamicPitch", "0", "Tilt the view to keep the feet a set distance above the screen edge" },
    { "test_cameraDynamicPitchBaseFovPad", "0.4", "Feet height above the bottom edge, fraction of the half vertical FoV" },
    { "test_cameraDynamicPitchBaseFovPadFlying", "0.75", "The same while flying, swimming or on a flight path" },
    { "test_cameraDynamicPitchBaseFovPadDownScale", "0.25", "Share of the tilt kept when looking straight down" },
    { "test_cameraDynamicPitchSmartPivotCutoffDist", "10", "Zoom (yards) below which dynamic pitch replaces the smart pivot" },
    { "test_cameraHeadMovementStrength", "0", "Camera follows the animated head, 0 = off" },
    { "test_cameraHeadMovementRangeScale", "5", "Zoom (yards) up to which head movement is at full strength" },
    { "test_cameraHeadMovementMovingStrength", "0.5", "Head movement strength while moving" },
    { "test_cameraHeadMovementStandingStrength", "0.3", "Head movement strength while standing" },
    { "test_cameraHeadMovementMovingDampRate", "10", "Head movement damping while moving, 1/s" },
    { "test_cameraHeadMovementStandingDampRate", "10", "Head movement damping while standing, 1/s" },
    { "test_cameraHeadMovementFirstPersonDampRate", "20", "Head movement damping in first person, 1/s" },
    { "test_cameraHeadMovementDeadZone", "0.015", "Head movement ignored below this, yards per axis" },
    { "test_cameraTargetFocusEnemyEnable", "0", "Turn the view toward an attackable target" },
    { "test_cameraTargetFocusEnemyStrengthPitch", "0.4", "Share of the pitch error removed, 0..1" },
    { "test_cameraTargetFocusEnemyStrengthYaw", "0.5", "Share of the yaw error removed, 0..1" },
    { "test_cameraTargetFocusInteractEnable", "0", "Turn the view toward the NPC or object being interacted with" },
    { "test_cameraTargetFocusInteractStrengthPitch", "0.75", "Share of the pitch error removed, 0..1" },
    { "test_cameraTargetFocusInteractStrengthYaw", "1.0", "Share of the yaw error removed, 0..1" },
    { "wxl_cameraLegionZoom", "0", "Legion zoom smoothing: the drawn distance springs to the zoom" },
    { "wxl_cameraSizeFactor", "1", "Stand-in for Legion's per-model size factor (shoulder offset, head range)" },
};
CVar* g_cvars[kCvCount];

float CvF(Cv i) {
    const float v = g_cvars[i] ? g_cvars[i]->m_floatValue : std::strtof(kCVars[i].def, nullptr);
    return std::isfinite(v) ? v : 0.0f;
}
int CvI(Cv i) { return g_cvars[i] ? g_cvars[i]->m_intValue : std::atoi(kCVars[i].def); }

// UNKNOWN: Legion multiplies the shoulder offset and the head range by a per-object float (obj+0x50, model size x
// scale). No 3.3.5a equivalent is known, so it is a CVar, default 1.
float SizeFactor() { return std::clamp(CvF(kSizeFactor), 0.0f, 10.0f); }

// ---- state --------------------------------------------------------------------------------------------------
struct Frame {                  // one CalcTargetCamera call; `active` only while inside it
    bool        active = false;
    CGCamera*   cam = nullptr;
    CGUnit_C*   unit = nullptr; // followed object, if it is a unit
    float       dt = 0.0f;
    bool        player = false; // following the active player, no cinematic camera
    bool        gate = false;   // + third-person view 1..5 and not first person (Legion's accessors)
    bool        h1Ran = false;  // collision ran, so the fields below are this frame's
    Vec3        base{};         // followed unit's position
    Vec3        pivot{};        // base + shoulder offset + pivot height (Legion's focus pivot)
    float       pitch0 = 0.0f;  // camera pitch before any look-only rotation
    float       padTarget = 1.0f;
    float       outHeight = 0.0f, curDist = 0.0f, desired = 0.0f;
    bool        collided = false; // boom, height or lateral hit, or the zoom is still easing
};
struct Collide {                // one CollideCameraWithWorld call
    bool  ours = false, locked = false, armed = false, hit = false;
    float dist0 = 0.0f, t = 1.0f;
    Vec3  pivotOff{}, head{}, added{};
};
struct DynPitch { Spring strength; Spring pad; bool padInit = false; bool zoomedIn = false; };
struct Head     { Vec3 accum{}, offset{}; CM2Model* model = nullptr; };
enum class FocusKind { None, Enemy, Interact };
struct Focus {
    float     off[2] = { 0.0f, 0.0f }, vel[2] = { 0.0f, 0.0f };   // pitch, yaw
    uint64_t  guid = 0;
    FocusKind kind = FocusKind::None;
    bool      active = false, blending = false;
};
struct Zoom { ZoomState st; CGCamera* cam = nullptr; bool valid = false; uint32_t lastNow = 0; float written = 0.0f; };

Frame    g_frame;
Collide  g_col;
Spring   g_shoulder;            // signed yards along the camera's Right() row (which points left)
DynPitch g_dyn;
Head     g_head;
Focus    g_focus;
Zoom     g_zoom;
uint32_t g_lastNow = 0;

// ---- engine access ------------------------------------------------------------------------------------------
float& DesiredDist(CGCamera* cam) { return cam->m_smoothDistance.m_target; }

CGObject_C* ObjectPtr(uint64_t guid, uint32_t typeMask) {
    if (!guid) return nullptr;
    return ClntObjMgrObjectPtr(WOWGUID{ guid }, static_cast<OBJECT_TYPE>(typeMask), __FILE__, __LINE__);
}

bool IsUnit(CGObject_C* obj) { return obj && obj->m_obj && (obj->m_obj->m_type & TYPE_UNIT); }
uint32_t MoveFlags(CGUnit_C* unit) { return unit->m_movement ? unit->m_movement->m_flags : 0; }
bool OnTaxi(CGUnit_C* unit) { return unit->m_unit && (unit->m_unit->m_unitFieldFlags & kUnitFlagTaxi); }
bool Mounted(CGUnit_C* unit) { return unit->m_mountDisplayId > 0; }
uint64_t ActivePlayer() { return ClntObjMgrGetActivePlayer().m_guid; }

Mat33 ReadMatrix(CGCamera* cam) { Mat33 m; std::memcpy(&m, &cam->m_facing, sizeof m); return m; }
void  WriteMatrix(CGCamera* cam, const Mat33& m) { if (Finite(m)) std::memcpy(&cam->m_facing, &m, sizeof m); }
static_assert(sizeof(Mat33) == sizeof(C33Matrix), "m_facing is three C3Vector rows");

/// Seconds since the last camera frame. The world tick's own delta is used when it agrees with the camera's
/// millisecond clock; the clock alone otherwise (it is coarser, never wrong).
float FrameDt(uint32_t now) {
    const float tick  = FrameTime::s_tickTimeSec;
    const float clock = g_lastNow ? static_cast<float>(now - g_lastNow) * 0.001f : -1.0f;
    g_lastNow = now;
    const bool tickOk = tick > 0.0f && tick < 0.25f, clockOk = clock > 0.0f && clock < 0.25f;
    if (tickOk && (!clockOk || std::fabs(tick - clock) <= 0.02f + 0.5f * clock)) return std::min(tick, 0.1f);
    return clockOk ? std::min(clock, 0.1f) : 0.0f;
}

bool OverShoulderActive(const Frame& f) { return f.gate && CvF(kOverShoulder) != 0.0f; }   // Legion 0x9EB769

/// Legion's accessor gate (0x9EB62E and siblings) plus "following the active player".
bool ThirdPersonGate(CGCamera* cam) {
    const uint32_t view = cam->m_viewIndex;
    const uint64_t guid = cam->m_targetGUID.m_guid;
    return !cam->m_model && view >= 1 && view <= 5 && std::fabs(DesiredDist(cam)) >= kEps
        && guid && guid == ActivePlayer();
}

// ---- dynamic pitch: state (Legion 0xA35983, frame step 14) --------------------------------------------------
float TargetPad(CGUnit_C* unit) {
    PadInputs in{};
    in.basePad   = Clamp01(CvF(kPad));
    in.flyingPad = Clamp01(CvF(kPadFlying));
    CMovement_C* mv = unit->m_movement;
    if (!mv) return in.basePad;
    const uint32_t flags = mv->m_flags;
    // Legion tests a "flying spline"; that spline bit is not identified on 3.3.5a, the taxi flag stands in.
    in.flying    = OnTaxi(unit) || (flags & (kMoveSlowFall | kMoveFlying | kMoveSwimming)) != 0;
    in.falling   = (flags & kMoveFalling) != 0;
    in.fallSpeed = FallSpeed(mv->m_fallVelocity, mv->m_fallTimeMs, (flags & kMoveSlowFall) != 0);
    // DEVIATION: Legion holds its "not initialised" sentinel (2.0) here when the first active frame is mid-fall,
    // which reads as "pad >= 1" and keeps the feature off until landing. The base pad is held instead.
    in.heldPad   = g_dyn.padInit ? g_dyn.pad.value : in.basePad;
    return TargetFovPad(in);
}

void UpdateDynamicPitchState(CGCamera* cam, Frame& f) {
    DynPitch& s = g_dyn;
    bool active = f.gate && CvI(kDynPitch) != 0;
    if ((!active && std::fabs(s.strength.value) < kEps) || !f.unit) { s.pad = {}; s.padInit = false; return; }

    f.padTarget = TargetPad(f.unit);
    if (f.padTarget < 0.0f || f.padTarget >= 1.0f) active = false;      // pad 1 = feet at centre = off
    SmoothDamp(s.strength, active ? 1.0f : 0.0f, 20.0f, f.dt, 0.0f);
    if (std::fabs(s.strength.value) < kEps) { s.pad = {}; s.padInit = false; return; }

    // Smart-pivot replacement: with the shoulder offset on and the zoom below the cutoff, looking up slides the
    // boom in to 1 yd instead of pivoting.
    const float pivotPitch = cam->m_targetOffset, desired = DesiredDist(cam);
    const float p = f.pitch0 + pivotPitch;
    float boom = -1.0f;
    if (std::fabs(pivotPitch) < 0.001f && desired < CvF(kSmartPivotCutoff) && OverShoulderActive(f) && p < 0.0f
        && !Mounted(f.unit)) {
        boom = SmartPivotDistance(p, desired);
        s.zoomedIn = true;
    } else if (s.zoomedIn && std::fabs(pivotPitch) < 0.001f) {
        boom = desired;                                                 // snap back, as Legion
        s.zoomedIn = false;
    }
    if (boom >= 0.0f && std::isfinite(boom)) {
        cam->m_distance = boom;
        if (g_zoom.valid) { g_zoom.st.s = { boom, 0.0f }; g_zoom.written = boom; }
    }
}

// ---- over-shoulder: the scalar (Legion 0xA35BCF, frame step 15) ---------------------------------------------
Vec3 UpdateShoulder(CGCamera* cam, const Frame& f) {
    Spring& s = g_shoulder;
    const bool active = OverShoulderActive(f);
    if ((!active && std::fabs(s.value) < kEps) || !f.unit) return {};

    float off = active ? CvF(kOverShoulder) : 0.0f;
    if (off > 0.0f) {                                                   // positive values only, as Legion
        if (f.player && CGGameUI::m_lockedTarget.m_guid == cam->m_targetGUID.m_guid)
            off = 0.0f;                                                 // the player has himself targeted
        else if (Mounted(f.unit))
            off *= 0.1f;
    }
    if (!(std::fabs(off) < kEps && std::fabs(s.value) < kEps))
        SmoothDamp(s, SizeFactor() * -1.0f * off, 30.0f, f.dt, 0.0f);
    return V(cam->Right()) * s.value;                                   // facing row b is the LEFT vector
}

// ---- head movement (Legion frame steps 6 and 19) ------------------------------------------------------------
/// The animated head relative to the camera's normal pivot, through the per-axis dead zone.
void SampleHead(CGCamera* cam, CGUnit_C* unit, Head& h) {
    CM2Model* model = unit->m_worldModel;
    if (!model || !model->IsLoaded(0, 0)) return;
    if (model != h.model) { h.model = model; h.accum = {}; }           // Legion zeroes it on model init

    const uint32_t topId = model->HasAttachment(55) ? 55 : 11;          // HeadTop, else Helm (3.3.5a has no 55)
    const bool hasTop = model->HasAttachment(topId) != 0, hasBreath = model->HasAttachment(17) != 0;
    if (!hasTop && !hasBreath) return;
    Vec3 top{}, breath{};
    if (hasTop) top = V(model->GetAttachmentPosition(topId));
    if (hasBreath) breath = V(model->GetAttachmentPosition(17));
    const Vec3 head = hasTop && hasBreath ? breath + (top - breath) * 0.5f : (hasTop ? top : breath);

    CM2Model* mount = unit->m_mountModel;
    if (mount && !mount->IsLoaded(0, 0)) return;
    const Vec3 origin = V((mount ? mount : model)->GetWorldPosition());

    Vec3 delta = head - origin;
    delta.z -= cam->m_height;
    if (!Finite(delta) || LenSq(delta) > 20.0f * 20.0f) return;        // not a head: a wrong assumption above
    // Legion caches the dead zone at model init (CVar changes need a reload there); read live here.
    const float deadZone = std::clamp(CvF(kHeadDeadZone), 0.0f, 50.0f);
    h.accum.x = DeadZoneStep(h.accum.x, delta.x, deadZone);
    h.accum.y = DeadZoneStep(h.accum.y, delta.y, deadZone);
    h.accum.z = DeadZoneStep(h.accum.z, delta.z, deadZone);
}

Vec3 UpdateHead(CGCamera* cam, const Frame& f) {
    Head& h = g_head;
    const float master = CvF(kHeadStrength);
    if ((!(master > 0.0f) && LenSq(h.offset) == 0.0f) || !f.unit) return {};

    const uint32_t flags = cam->m_state;
    const float cur = cam->m_distance;
    const bool  firstPerson = std::fabs(DesiredDist(cam)) < kEps, moving = (flags & kFlagMoving) != 0;
    float a, b;
    HeadRange(std::clamp(CvF(kHeadRangeScale), 0.0f, 50.0f), SizeFactor(), a, b);
    // Legion's own gate (0x9E84CF): no view or first-person test here, unlike the other features.
    const bool active = f.player && !(flags & kFlagLocked) && master > 0.0f && cur <= a + b && !OnTaxi(f.unit);

    Vec3 target{};
    if (active) {
        SampleHead(cam, f.unit, h);
        target = h.accum * HeadStrength(master, CvF(kHeadStanding), CvF(kHeadMoving), moving,
                                        (MoveFlags(f.unit) & kMoveFalling) != 0, firstPerson,
                                        f.unit->m_mountModel != nullptr, HeadRangeFactor(cur, a, b));
    }
    const Cv rateCv = firstPerson ? kHeadFirstPersonDamp : (moving ? kHeadMovingDamp : kHeadStandingDamp);
    const float rate = std::clamp(CvF(rateCv), 1.0f / 360.0f, 50.0f);
    h.offset = { ExpDamp(h.offset.x, target.x, rate, f.dt), ExpDamp(h.offset.y, target.y, rate, f.dt),
                 ExpDamp(h.offset.z, target.z, rate, f.dt) };
    // An exponential never arrives; ending it lets the whole feature go idle again.
    if (!active && LenSq(h.offset) < 1e-10f) h.offset = {};
    if (!Finite(h.offset)) h = {};
    return h.offset;
}

// ---- collision (Legion CollisionCheck 0x9E8BB5 around stock 0x00605D60) -------------------------------------
/// Legion's second ray: from the shortened camera along the position-only offset, 1/9 yd of margin.
Vec3 TraceLateral(CGCamera* cam, const Frame& f, float dist, Vec3 pivotOff, Vec3 posOff, bool& hit) {
    Vec3 p = f.base + Vec3{ 0.0f, 0.0f, f.outHeight } - V(cam->Forward()) * dist + pivotOff;
    const float fly = cam->m_flyingMountHeight, denom = std::max(cam->m_distance, DesiredDist(cam));
    if (dist > 0.0f && fly != 0.0f && denom > 0.0f)                     // as GetCameraPosition 0x00601D60
        p = p + V(cam->Up()) * (cam->m_collideExtent * fly * dist / denom);

    const float len = std::sqrt(LenSq(posOff));
    float l = len + kNinth;
    Vec3  d = posOff * (l / len);
    const Vec3 end = p + d;
    C3Vector hitPoint(0.0f, 0.0f, 0.0f);
    float t = 1.0f;
    if (Finite(p) && Finite(end)
        && World::Intersect(C(p), C(end), &hitPoint, &t, CGCamera::GetCollisionQueryFlags(), nullptr)
        && t >= 0.0f && t <= 1.0f) {
        hit = true;
        l *= t;
        d = d * t;
    }
    // Legion also sweeps the near plane along this move (0x9E743A); 3.3.5a has no proven equivalent.
    return l > 0.001f ? d * (std::max(l - kNinth, 0.0f) / l) : Vec3{};
}

// ---- zoom / look-only rotations -----------------------------------------------------------------------------
void ApplyDynamicPitch(CGCamera* cam, const Frame& f) {
    DynPitch& s = g_dyn;
    if (std::fabs(s.strength.value) < kEps || !f.unit) return;
    const float pad  = f.padTarget < 0.0f ? 1.0f : f.padTarget;
    const float half = cam->m_fov * 0.6f * 0.5f;                       // 0.6 hard-coded, no aspect term
    if (!(half > 0.0f) || !std::isfinite(half)) return;

    const float feet = FeetAngle(f.pitch0, f.collided ? f.desired : f.curDist, f.outHeight);
    if (!s.padInit) { s.pad = { InitialPad(feet, f.pitch0, half), 0.0f }; s.padInit = true; }
    SmoothDamp(s.pad, pad, 12.0f, f.dt, 0.0f);

    const float dyn = DynamicPitchAngle(f.pitch0, feet, half, s.pad.value,
                                        std::clamp(CvF(kPadDownScale), 0.0f, 0.9f), s.strength.value);
    if (!std::isfinite(dyn) || std::fabs(dyn) < 0.001f) return;

    // Stock has already applied the pivot pitch (m_targetOffset) and the vehicle pitch, unclamped; only the
    // dynamic part is added. DEVIATION: stock pitch reaches 89 deg where Legion's stops at 88, so the clamp never
    // pulls a pitch that is already beyond 88 deg back (that would pop by 1 deg).
    Mat33 m = ReadMatrix(cam);
    const float p = PitchOf(m.r[0]), lim = std::max(kPitchLimit, std::fabs(p));
    RotateLocalY(m, std::clamp(p + dyn, -lim, lim) - p);
    WriteMatrix(cam, m);
}

void ApplyTargetFocus(CGCamera* cam, const Frame& f) {
    Focus& s = g_focus;
    const bool enemyOn = CvI(kFocusEnemy) != 0, interactOn = CvI(kFocusInteract) != 0;
    if (!s.active && !enemyOn && !interactOn) return;

    uint64_t    guid = 0;
    CGObject_C* target = nullptr;
    bool        isInteract = false;
    if (f.gate && f.h1Ran && f.unit) {
        const uint64_t interact = CGGameUI::m_interactTarget.m_guid;
        const uint64_t locked   = CGGameUI::m_lockedTarget.m_guid;
        if (interactOn && interact) {
            // Interact wins and never falls through to the enemy case. Legion accepts any object; items (reading a
            // letter) have no place in the world, so units and game objects only.
            isInteract = true;
            guid = interact;
            target = ObjectPtr(guid, TYPE_UNIT | TYPE_GAMEOBJECT);
        } else if (enemyOn && locked && locked != cam->m_targetGUID.m_guid) {
            guid = locked;                                              // kept even if the tests below fail
            CGObject_C* unit = ObjectPtr(guid, TYPE_UNIT);
            if (unit && f.unit->CanAttack(static_cast<CGUnit_C*>(unit))) target = unit;
        }
    }
    const bool hasTarget = target != nullptr;
    if (!s.active && !hasTarget) return;                                // idle: nothing touched

    const Mat33 m = ReadMatrix(cam);
    if (!Finite(m)) return;
    const float curPitch = PitchOf(m.r[0]);
    float       desired[2] = { 0.0f, 0.0f };
    FocusKind   kind = FocusKind::None;
    if (hasTarget) {
        s.active = true;
        FocusIn in{};
        in.aim = V(target->GetPosition());
        float height = target->m_height * target->m_scale;
        if (!(height >= 0.0f && height < 100.0f)) height = 0.0f;
        in.aim.z += 0.6f * height;
        in.pivot = f.pivot;
        in.base = f.base;
        in.eye = V(cam->m_position);
        in.curYaw = YawOf(V(cam->Forward()));                           // world yaw, also on a transport
        in.curPitch = curPitch;
        in.curDist = f.curDist;
        in.fov = std::clamp(cam->m_fov + cam->m_targetFov, 0.0f, 3.1241393f); // CGCamera::FOV 0x005FF500
        in.strengthPitch = Clamp01(CvF(isInteract ? kFocusInteractPitch : kFocusEnemyPitch));
        in.strengthYaw   = Clamp01(CvF(isInteract ? kFocusInteractYaw : kFocusEnemyYaw));
        // DEVIATION: with the aim point on top of the eye Legion returns here and the offset is simply missing
        // for that frame; it is sprung to zero and back instead.
        if (Finite(in.aim) && Finite(in.eye) && FocusDesired(in, desired[0], desired[1])
            && std::isfinite(desired[0]) && std::isfinite(desired[1]))
            kind = isInteract ? FocusKind::Interact : FocusKind::Enemy;
        else
            desired[0] = desired[1] = 0.0f;
    }

    if (guid != s.guid) { s.blending = true; s.guid = guid; }           // a GUID change arms the spring
    // DEVIATION: Legion arms it on a GUID change only, so the offset jumps in one frame when the target stops
    // being valid while still targeted or enemy and interact swap on one GUID.
    if (kind != s.kind) { s.blending = true; s.kind = kind; }

    if (s.blending) {
        SmoothCD2(s.off, s.vel, desired, kind != FocusKind::None ? 20.0f : 15.0f, f.dt, 0.0f);
        if (std::fabs(desired[0] - s.off[0]) < 0.00174533f && std::fabs(desired[1] - s.off[1]) < 0.00174533f)
            s.blending = false;                                         // within 0.1 deg: locked
    } else {
        s.off[0] = desired[0]; s.off[1] = desired[1];                   // locked: raw tracking
        s.vel[0] = s.vel[1] = 0.0f;
    }
    if (!std::isfinite(s.off[0]) || !std::isfinite(s.off[1])) { s = {}; return; }

    if (s.off[0] * s.off[0] + s.off[1] * s.off[1] < kEps) {
        if (!hasTarget) { s.active = false; s.blending = false; }       // idle again
        return;
    }
    // Legion rebuilds the matrix from the offset forward vector with a level left vector (roll dropped). The
    // matrix itself is relative to a transport; a yaw offset is the same in both.
    // DEVIATION: the pitch is kept inside the stock +-89 deg so the view cannot flip over the pole.
    WriteMatrix(cam, FromYawPitch(YawOf(m.r[0]) + s.off[1], std::clamp(curPitch + s.off[0], -1.55334f, 1.55334f)));
}

bool AnythingToDo() {
    return CvF(kOverShoulder) != 0.0f || CvI(kDynPitch) != 0 || CvF(kHeadStrength) > 0.0f
        || CvI(kFocusEnemy) != 0 || CvI(kFocusInteract) != 0 || CvI(kLegionZoom) != 0
        // ...or something is still easing out after being switched off
        || g_shoulder.value != 0.0f || g_dyn.strength.value != 0.0f || g_dyn.zoomedIn
        || LenSq(g_head.offset) != 0.0f || g_focus.active || g_zoom.valid;
}

} // namespace

namespace ActionCam {

void RegisterCVars() {
    for (int i = 0; i < kCvCount; ++i)
        g_cvars[i] = CVar::Register(kCVars[i].name, kCVars[i].help, 1, kCVars[i].def, nullptr, CATEGORY_DEFAULT, false, nullptr, false);
}

void Reset() {
    g_frame = {}; g_col = {}; g_shoulder = {}; g_dyn = {}; g_head = {}; g_focus = {}; g_zoom = {};
    g_lastNow = 0;
}

void FrameBegin(CGCamera* cam, CGObject_C* target, uint32_t now) {
    Frame& f = g_frame;
    f = {};
    if (!AnythingToDo()) return;                                        // everything off: stock, untouched
    f.cam    = cam;
    f.unit   = IsUnit(target) ? static_cast<CGUnit_C*>(target) : nullptr;
    f.dt     = FrameDt(now);
    f.player = f.unit && !cam->m_model && cam->m_targetGUID.m_guid == ActivePlayer();
    f.active = true;
}

// Legion UpdateBlends 0x9EFF18.
void AfterSmoothing(CGCamera* cam, uint32_t now) {
    const Frame& f = g_frame;
    if (!f.active || cam != f.cam) return;
    Zoom& z = g_zoom;
    const float desired = DesiredDist(cam), cur = cam->m_distance;
    if (CvI(kLegionZoom) == 0 || !std::isfinite(desired) || !std::isfinite(cur) || desired < 0.0f) { z.valid = false; return; }

    if (!z.valid || z.cam != cam || now - z.lastNow > 1000) { ZoomSnap(z.st, cur); z.cam = cam; z.valid = true; }
    z.lastNow = now;
    // Legion scales the target by 0.75 + 0.25 * model scale; taken as 1 (see the size factor).
    ZoomStep(z.st, desired, desired, 1.0f, (cam->m_state & kFlagLocked) != 0, now, f.dt);
    if (!std::isfinite(z.st.s.value)) { z.valid = false; return; }
    // The state is the authority: stock's own write of m_distance this frame (copy on input, timed blend) is
    // replaced, and its blend is stopped.
    z.written = cam->m_distance = std::max(z.st.s.value, 0.0f);
    cam->m_state &= ~kFlagDistBlend;
}

void CollideBegin(CGCamera* cam, const C3Vector& base, C3Vector& offset) {
    Frame& f = g_frame;
    Collide& c = g_col;
    c = {};
    if (!f.active || cam != f.cam) return;

    f.base   = V(base);
    f.pitch0 = PitchOf(ReadMatrix(cam).r[0]);                           // m_facing is final for collision here
    f.gate   = f.player && ThirdPersonGate(cam);

    UpdateDynamicPitchState(cam, f);                                    // may pull m_distance in
    Vec3 pivotOff = UpdateShoulder(cam, f);
    Vec3 head     = UpdateHead(cam, f);
    if (!Finite(pivotOff) || !Finite(head)) { pivotOff = {}; head = {}; g_shoulder = {}; g_head = {}; }

    c.locked   = (cam->m_state & kFlagLocked) != 0;
    c.ours     = LenSq(pivotOff) > 0.0f || LenSq(head) > 0.0f;
    c.dist0    = std::max(DesiredDist(cam), cam->m_distance);
    c.pivotOff = pivotOff;
    c.head     = head;
    if (c.ours) {
        // Stock adds the offset (shake + bob) to the pivot after collision and shows it to the near-plane sweep;
        // ours rides along. The boom ray itself gets only the shoulder part (BoomShift).
        c.added = pivotOff + head;
        offset  = C(V(offset) + c.added);
        c.armed = !c.locked && LenSq(pivotOff) > 0.0f;
    }
}

// Legion traces from the unshifted pivot to the SHIFTED camera.
C3Vector BoomShift() { return g_col.armed ? C(g_col.pivotOff) : C3Vector(0.0f, 0.0f, 0.0f); }

void BoomHit(float t) {
    if (g_col.armed && t >= 0.0f && t <= 1.0f) { g_col.hit = true; g_col.t = t; }
}

C3Vector SweepOffset(const C3Vector& offset) {
    if (!g_col.hit) return offset;
    return C(V(offset) - g_col.pivotOff * (1.0f - g_col.t));            // pivotOff *= t, before the sweep
}

void CollideEnd(CGCamera* cam, uint32_t bits, float distance, float height, C3Vector& offset) {
    Frame& f = g_frame;
    Collide& c = g_col;
    if (!f.active || cam != f.cam) return;

    bool lateralHit = false;
    f.outHeight = height;
    Vec3 pivotOff = c.pivotOff;
    if (c.ours) {
        Vec3 posOff = c.head;
        if (!c.locked) {                                                // flag 8: full offset, no traces
            if (c.hit && (bits & kFlagBoomHit)) pivotOff = pivotOff * c.t;
            if (std::fabs(c.dist0 - distance) >= kEps && std::fabs(distance) >= kEps && c.dist0 > 0.0f)
                pivotOff = pivotOff * (distance / c.dist0);             // also without a hit, while zooming out
            posOff = posOff + (c.pivotOff - pivotOff);                  // what was removed is re-offered sideways
            if (LenSq(posOff) > 1e-6f) posOff = TraceLateral(cam, f, distance, pivotOff, posOff, lateralHit);
        }
        Vec3 total = pivotOff + posOff;
        if (!Finite(total)) { total = {}; pivotOff = {}; }
        offset = C(V(offset) - c.added + total);                        // stock builds pivot and eye from it
    }
    c.armed = false;

    f.h1Ran   = true;
    f.curDist = cam->m_distance;
    f.desired = DesiredDist(cam);
    f.pivot   = f.base + pivotOff + Vec3{ 0.0f, 0.0f, f.outHeight };
    // Legion's "collided or zoom still easing" (flags 0x70000 | 0x80000).
    f.collided = (bits & (kFlagBoomHit | kFlagHeightHit)) != 0 || lateralHit
        || (g_zoom.valid ? g_zoom.st.running : (cam->m_state & kFlagDistBlend) != 0);
}

void FrameEnd(CGCamera* cam, uint32_t now) {
    Frame& f = g_frame;
    if (!f.active || cam != f.cam) return;
    f.active = false;

    // Whatever moved m_distance after the zoom spring wrote it was the collision feedback (0x006076C2:
    // dist + 1/9). Legion's SnapDistance: take the value, drop the velocity, restart the rate ramp.
    Zoom& z = g_zoom;
    if (z.valid && z.lastNow == now && cam->m_distance != z.written) {
        z.written = cam->m_distance;
        ZoomSnap(z.st, z.written);
    }

    if (f.h1Ran) ApplyDynamicPitch(cam, f);
    ApplyTargetFocus(cam, f);
}

/// Legion CanPivot 0x9E8577: no smart pivot below the cutoff while dynamic pitch is on.
bool RefusePivot(CGCamera* cam) {
    return cam && CvI(kDynPitch) != 0 && ThirdPersonGate(cam) && DesiredDist(cam) < CvF(kSmartPivotCutoff);
}

// Legion's GetCameraZoom (0x9E5129) returns cam+0x224, the distance ASKED for, not the eased one being drawn. The
// DynamicCam addon's eased and reactive zooms steer on this value; the lagging drawn distance makes them overshoot.
float DesiredZoom() {
    CGCamera* cam = CGWorldFrame::GetActiveCamera();
    return cam ? DesiredDist(cam) : 0.0f;
}

} // namespace ActionCam
