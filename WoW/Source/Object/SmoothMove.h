#ifndef WOW_SOURCE_OBJECT_SMOOTHMOVE_H
#define WOW_SOURCE_OBJECT_SMOOTHMOVE_H

// CUSTOM: smooth creature movement (not in stock 3.3.5a).
//  - smoothSplines: the 4.3.4 client's pathSmoothing (C3Spline_CatmullRom::Smooth 0x004CABF0 + spline mode 2,
//    Catmull-Rom in X/Y with linear Z), applied to every linear ground spline of a unit we do not control.
//    Duration and end point are unchanged and evaluation is by arc length, so arrival time matches the server.
//  - smoothTurning: turn-rate cap on spline facing, and final / in-place facings turned toward over the next
//    frames instead of snapped (4.3.4 has no cap; this is the ClientExtensions SmoothTurn design, natively).
// The active mover (your character / vehicle) is never touched.

#include <cmath>
#include <cstdint>

class CMovement_C;

namespace SmoothMove {
    void RegisterCVars();

    // after a spline was set on `mov` (OnSpline / create block): round its corners if eligible
    void SmoothSpline(CMovement_C* mov);

    // after the spline evaluator wrote m_facing: limit how far it moved from `before` in `elapsedMs`
    void LimitTurn(CMovement_C* mov, float before, uint32_t elapsedMs);

    // after something snapped m_facing to a final facing: put `before` back and turn toward it from Tick
    void DeferFacing(CMovement_C* mov, float before);

    // once per movement update
    void Tick(uint32_t elapsedMs);

    // ---- maths (pure, also used by the self-check) --------------------------------------------------------
    constexpr float kTwoPi = 6.2831853f;

    inline float Wrap(float a) {
        a = std::fmod(a, kTwoPi);
        return a < 0.0f ? a + kTwoPi : a;
    }

    // signed shortest difference to - from, in (-pi, pi]
    inline float Delta(float from, float to) {
        float d = Wrap(to - from);
        return d > kTwoPi * 0.5f ? d - kTwoPi : d;
    }

    // move `from` toward `to` by at most `maxStep` radians; returns exactly Wrap(to) when within reach
    inline float Approach(float from, float to, float maxStep) {
        float d = Delta(from, to);

        if (std::fabs(d) <= maxStep) {
            return Wrap(to);
        }

        return Wrap(from + (d > 0.0f ? maxStep : -maxStep));
    }
}

#endif
