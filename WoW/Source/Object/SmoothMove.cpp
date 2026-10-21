#include "WoW/Source/Object/SmoothMove.h"
#include <vector>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "WoW/Source/Object/CMoveSpline.h"
#include "WoW/Source/Object/Movement.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/Object/Types.h"
#include "WoW/Source/Object/Unit.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"

namespace {
    CVar* s_cvSmoothSplines;
    CVar* s_cvSmoothTurning;
    CVar* s_cvSmoothTurnRate;

    // a final facing still being turned toward
    struct Pending {
        WOWGUID guid;
        float target;
        float written;   // what we last wrote; anything else in m_facing means someone else set it
    };

    std::vector<Pending> s_pending;

    bool IsActiveMover(CMovement_C* mov) {
        return !mov->m_unit || mov->m_unit->m_obj->m_guid == CGUnit_C::s_activeMover;
    }

    float RateFor(CMovement_C* mov) {
        float rate = s_cvSmoothTurnRate ? s_cvSmoothTurnRate->m_floatValue : 360.0f;

        if (rate <= 0.0f) {
            rate = 360.0f;
        }

        // UNIT_FLAG_IN_COMBAT: twice as fast, so a creature turning to face its target is not sluggish
        if (mov->m_unit->m_unit && (mov->m_unit->m_unit->m_unitFieldFlags & 0x80000)) {
            rate *= 2.0f;
        }

        return rate * (SmoothMove::kTwoPi / 360.0f);
    }

    void DropPending(const WOWGUID& guid) {
        for (size_t i = 0; i < s_pending.size(); i++) {
            if (s_pending[i].guid == guid) {
                s_pending[i] = s_pending.back();
                s_pending.pop_back();
                return;
            }
        }
    }

    bool TurningOn() {
        return s_cvSmoothTurning && s_cvSmoothTurning->m_intValue;
    }
}

void SmoothMove::RegisterCVars() {
    s_cvSmoothSplines = CVar::Register("smoothSplines", "Round the corners of creature ground paths (Cataclysm path smoothing)", 1, "1", nullptr, 4, false, nullptr, false);
    s_cvSmoothTurning = CVar::Register("smoothTurning", "Creatures turn at a limited rate instead of snapping their facing", 1, "1", nullptr, 4, false, nullptr, false);
    s_cvSmoothTurnRate = CVar::Register("smoothTurnRate", "smoothTurning rate in degrees per second (doubled in combat)", 1, "360", nullptr, 4, false, nullptr, false);
}

void SmoothMove::SmoothSpline(CMovement_C* mov) {
    CMoveSpline* spline = mov->m_spline;

    if (!s_cvSmoothSplines || !s_cvSmoothSplines->m_intValue || !spline || IsActiveMover(mov)) {
        return;
    }

    // linear ground paths only: Catmull-Rom/flying paths are already curves; falls, jumps and cycles keep stock
    const uint32_t skip = SPLINEFLAG_NO_SPLINE | SPLINEFLAG_FALLING | SPLINEFLAG_PARABOLIC | SPLINEFLAG_FLYING | SPLINEFLAG_CATMULLROM | SPLINEFLAG_CYCLIC | SPLINEFLAG_ENTER_CYCLE | SPLINEFLAG_BACKWARD | SPLINEFLAG_TRANSPORT_ENTER | SPLINEFLAG_TRANSPORT_EXIT;
    C3Spline_CatmullRom& curve = spline->m_spline;

    if ((spline->m_flags & skip) || curve.splineMode != MODE_LINEAR || curve.m_pointCount < 5) {
        return;
    }

    // 4.3.4 lead-in (OnMonsterMove 0x005CB460): leave along the current facing when the first leg is within
    // ~45 degrees of it, else straight along the leg. The 3.3.5 builder put the facing direction in points[0].
    C3Vector& p0 = curve.Point(0);
    const C3Vector& p1 = curve.Point(1);
    const C3Vector& p2 = curve.Point(2);
    C3Vector face(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
    C3Vector leg(p2.x - p1.x, p2.y - p1.y, p2.z - p1.z);
    face.NormalizeSafe();
    leg.NormalizeSafe();
    const C3Vector& lead = (face.x * leg.x + face.y * leg.y + face.z * leg.z > 0.7f) ? face : leg;
    C3Vector oldP0 = p0;
    p0 = C3Vector(p1.x - lead.x * 2.0f, p1.y - lead.y * 2.0f, p1.z - lead.z * 2.0f);

    curve.splineMode = MODE_SMOOTHGROUND;

    if (!curve.Smooth(1.0f)) {
        curve.splineMode = MODE_LINEAR;
        curve.Point(0) = oldP0;
    }
}

void SmoothMove::LimitTurn(CMovement_C* mov, float before, uint32_t elapsedMs) {
    CMoveSpline* spline = mov->m_spline;

    if (!TurningOn() || !spline || IsActiveMover(mov) || (spline->m_flags & (SPLINEFLAG_ORIENTATION_FIXED | SPLINEFLAG_FALLING))) {
        return;
    }

    DropPending(mov->m_unit->m_obj->m_guid);   // a new move replaces an unfinished final turn

    if (elapsedMs > 100) {
        elapsedMs = 100;   // hitch: cap one step
    }

    mov->m_facing = SmoothMove::Approach(before, mov->m_facing, RateFor(mov) * elapsedMs * 0.001f);
}

void SmoothMove::DeferFacing(CMovement_C* mov, float before) {
    if (!TurningOn() || IsActiveMover(mov)) {
        return;
    }

    float target = SmoothMove::Wrap(mov->m_facing);

    if (std::fabs(SmoothMove::Delta(before, target)) < 0.01f) {
        return;   // no real turn
    }

    const WOWGUID& guid = mov->m_unit->m_obj->m_guid;
    DropPending(guid);
    s_pending.push_back({ guid, target, before });
    mov->m_facing = before;
}

void SmoothMove::Tick(uint32_t elapsedMs) {
    if (s_pending.empty()) {
        return;
    }

    if (elapsedMs > 100) {
        elapsedMs = 100;
    }

    bool on = TurningOn();

    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        CGUnit_C* unit = static_cast<CGUnit_C*>(ClntObjMgrObjectPtr(p.guid, TYPE_UNIT, __FILE__, __LINE__));
        CMovement_C* mov = unit ? unit->m_movement : nullptr;
        bool done = true;

        // gone, or something else set its facing since (teleport, new move, server facing): let that stand
        if (mov && std::fabs(mov->m_facing - p.written) <= 1e-4f) {
            p.written = on ? SmoothMove::Approach(p.written, p.target, RateFor(mov) * elapsedMs * 0.001f) : p.target;
            mov->m_facing = p.written;
            done = p.written == p.target;
        }

        if (done) {
            s_pending[i] = s_pending.back();
            s_pending.pop_back();
        } else {
            i++;
        }
    }
}
