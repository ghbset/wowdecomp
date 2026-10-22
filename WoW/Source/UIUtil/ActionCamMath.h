// ActionCam (not stock, ported from wxl-camera): the pure maths of Legion 7.3.5's ActionCam. No Windows, no engine: everything here is
// built and asserted natively by Tools/ActionCamCheck/math_test.cpp. Formulas and constants are from
// /root/RE/camera/legion/re-*.md; the Legion address each one was read at is given once per item.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ActionCamMath
{
    constexpr float kPi         = 3.14159265f;
    constexpr float kTwoPi      = 6.28318531f;
    constexpr float kEps        = 2.38419e-07f; // 0x01086D8C: NearlyEqual / "is zero"
    constexpr float kPitchLimit = 1.53589f;     // 0x010EFE0C: 88 deg, look-only pitch clamp
    constexpr float kNinth      = 1.0f / 9.0f;  // 0x010D7CB8: gap kept between the eye and a hit

    struct Vec3 { float x, y, z; };
    inline Vec3  operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3  operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec3  operator*(Vec3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }
    inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline float LenSq(Vec3 a) { return Dot(a, a); }
    inline bool  Finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
    inline float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
    inline float Sign(float x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); } // 0x6E9D29

    // ---- springs -------------------------------------------------------------------------------------

    struct Spring { float value = 0.0f, velocity = 0.0f; };

    /// SmoothDamp 0x6E9D59: critically damped spring, omega passed directly. True once on target.
    inline bool SmoothDamp(Spring& s, float target, float omega, float dt, float eps)
    {
        if (!(eps > 1e-5f)) eps = 1e-5f;
        const float diff = s.value - target;
        if (eps >= std::fabs(diff)) { s = { target, 0.0f }; return true; }
        const float x     = omega * dt;
        const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        const float temp  = (s.velocity + omega * diff) * dt;
        s.velocity = (s.velocity - omega * temp) * decay;
        s.value    = target + (diff + temp) * decay;
        if ((s.value - target) * diff > 0.0f) return false;
        s = { target, 0.0f };                                        // crossed the target
        return true;
    }

    /// SmoothCD2 0x6E96B9: the same spring on a (pitch, yaw) pair, with one overshoot guard for both.
    inline bool SmoothCD2(float cur[2], float vel[2], const float tgt[2], float omega, float dt, float eps)
    {
        if (!(eps > 1e-5f)) eps = 1e-5f;
        const float d[2] = { cur[0] - tgt[0], cur[1] - tgt[1] };
        bool done = std::max(std::fabs(d[0]), std::fabs(d[1])) <= eps;
        if (!done)
        {
            const float x = omega * dt;
            const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
            for (int i = 0; i < 2; ++i)
            {
                const float tmp = (vel[i] + omega * d[i]) * dt;
                cur[i] = tgt[i] + (d[i] + tmp) * e;
                vel[i] = (vel[i] - omega * tmp) * e;
            }
            done = (cur[0] - tgt[0]) * d[0] + (cur[1] - tgt[1]) * d[1] <= 0.0f;
        }
        if (done) { cur[0] = tgt[0]; cur[1] = tgt[1]; vel[0] = vel[1] = 0.0f; }
        return done;
    }

    /// First-order low pass of head movement (0x9EF1DF): frame-rate independent, no velocity.
    inline float ExpDamp(float cur, float target, float rate, float dt)
    {
        return target - (target - cur) * std::exp(-rate * dt);
    }

    // ---- zoom smoothing (UpdateBlends 0x9EFF18..0x9F00FC) ---------------------------------------------

    struct ZoomState
    {
        Spring   s;                 // Legion +0x228 / +0x22C
        float    baseRate = 0.0f;   // +0x230, latched once per run
        uint32_t startMs  = 0;      // +0x234
        bool     running  = false;  // flag 0x80000
    };

    /// SnapDistance 0x9EA7A2.
    inline void ZoomSnap(ZoomState& z, float v) { z.s = { v, 0.0f }; z.baseRate = 0.0f; z.running = false; }

    /// Rate a run starts at: 30 for a change within `mult` yards, else 10 falling to 1.5 at 10x that.
    inline float ZoomStartRate(float desired, float cur, float mult, bool locked)
    {
        if (locked) return 20.0f;
        const float d = std::fabs(desired - cur);
        if (mult >= d) return 30.0f;
        const float t = Clamp01((d - mult) / (mult * 10.0f - mult));
        return (1.0f - t) * 10.0f + t * 1.5f;
    }

    /// One frame of the drawn distance chasing `target`. `mult` = cameraDistanceRateMult (1).
    inline void ZoomStep(ZoomState& z, float desired, float target, float mult, bool locked, uint32_t nowMs, float dt)
    {
        if (std::fabs(z.s.value - target) < kEps) { ZoomSnap(z, target); return; }
        if (!z.running)
        {
            z.running  = true;
            z.startMs  = nowMs;
            z.baseRate = ZoomStartRate(desired, z.s.value, mult, locked);
        }
        const float el   = static_cast<float>(nowMs - z.startMs) * 0.001f;
        const float rate = std::min(z.baseRate + mult * 5.0f * el * el, mult * 15.0f);
        if (SmoothDamp(z.s, target, rate, dt, 0.01f)) { z.baseRate = 0.0f; z.running = false; }
    }

    // ---- dynamic pitch (0xA35661, 0xA35983, 0x9EAD7B) -------------------------------------------------

    /// Downward speed of a fall. 3.3.5a 0x986E10 (same physics as Legion 0x103753E).
    inline float FallSpeed(float initialSpeed, uint32_t fallMs, bool slowFall)
    {
        const float term = slowFall ? 7.0f : 60.148f;                // [0xB2D9EC] / [0xB2D9E8]
        return std::min(std::min(initialSpeed, term) + 19.291105f * static_cast<float>(fallMs) * 0.001f, term);
    }

    struct PadInputs
    {
        float basePad, flyingPad;   // CVars, 0..1
        bool  flying;               // flying, swimming, slow fall or on a flight path
        bool  falling;
        float fallSpeed;            // FallSpeed(), positive = down
        float heldPad;              // the smoothed pad, held while airborne
    };

    /// Target pad 0x9EAD7B: fraction of the half vertical FoV kept between the feet and the screen edge.
    inline float TargetFovPad(const PadInputs& in)
    {
        float pad = in.flying ? in.flyingPad : in.basePad;
        if (in.falling)                                              // overrides the line above
        {
            pad = std::max(in.heldPad, in.basePad);
            const float vt = 60.148f;                                // 0x0135F7D0
            if (in.fallSpeed > 0.0f && in.fallSpeed - 0.125f * vt > 0.0f)
            {
                const float t = Clamp01((in.fallSpeed - 0.125f * vt) / (0.3f * vt));
                pad = std::max(pad, (1.0f - t) * in.basePad + t * in.flyingPad);
            }
        }
        return pad;
    }

    /// PitchOf 0x6E516E: downward angle of v, positive = pointing down.
    inline float PitchOf(Vec3 v)
    {
        const float len = std::sqrt(LenSq(v));
        if (len < kEps) return 0.0f;
        const float s = v.z / len;
        if (s >= 1.0f) return -kPi * 0.5f;
        if (s <= -1.0f) return kPi * 0.5f;
        return std::asin(-s);
    }

    /// Downward angle from the camera to the feet: PitchOf(fwd * d - (0, 0, h)) for a camera pitched `pitch`.
    inline float FeetAngle(float pitch, float d, float h)
    {
        return PitchOf({ d * std::cos(pitch), 0.0f, -d * std::sin(pitch) - h });
    }

    /// First active frame: the pad that reproduces the current framing.
    inline float InitialPad(float feetAngle, float pitch, float halfVFov)
    {
        const float x = halfVFov + pitch;                            // downward angle of the bottom edge
        return (feetAngle > x || !(halfVFov > 0.0f)) ? 0.0f : (x - feetAngle) / halfVFov;
    }

    /// The angle 0xA35661, radians, positive = tilt down. `downScale` already clamped to 0..0.9.
    inline float DynamicPitchAngle(float pitch, float feetAngle, float halfVFov, float pad, float downScale, float strength)
    {
        float base = 0.0872665f;                                     // 0x010FED60: +5 deg cap
        const float x1 = (1.0f - pad) * halfVFov + pitch + 0.0872665f;
        if (x1 > feetAngle) base = feetAngle - x1 + 0.0872665f;
        float range = kPitchLimit, other = downScale * base;         // looking down: fade to down * base
        if (pitch < 0.0f) { range = -kPitchLimit; other = 0.0f; }    // looking up: fade to nothing
        const float t = Clamp01(pitch / range);
        const float u = 1.0f - (1.0f - t) * (1.0f - t);
        return ((1.0f - u) * base + u * other) * strength;
    }

    /// Smart-pivot replacement (0xA35A9F): boom pulled in to 1 yd as the view pitches up to -70 deg.
    inline float SmartPivotDistance(float pitchSum, float desired)
    {
        const float t = Clamp01(pitchSum * -0.818511f);              // 0x010FED68
        const float u = 1.0f - (1.0f - t) * (1.0f - t);
        return (1.0f - u) * desired + u * 1.0f;
    }

    // ---- head movement (0x9EAF77, 0x9EAFC3, 0x9E5E3A) -------------------------------------------------

    /// Range 0x9EAF77: full strength within `a`, linear fade over `b`. `size` = per-object size factor.
    inline void HeadRange(float rangeScale, float size, float& a, float& b)
    {
        const float s = std::min(size, 1.5f);                        // 0x0109F9C8
        a = rangeScale * s;
        b = std::min(2.0f * a, 30.0f * s);
    }

    inline float HeadRangeFactor(float curDist, float a, float b)
    {
        // Range 0 (scale 0): Legion divides by it and the result clamps to "no head movement" at every
        // distance, first person included (checked against 0x9EAFC3 with fncheck).
        if (!(b > 0.0f)) return 0.0f;
        return 1.0f - Clamp01((curDist - a) / b);
    }

    /// Per-axis dead zone (backlash): the accumulator follows only what exceeds the zone.
    inline float DeadZoneStep(float acc, float v, float deadZone)
    {
        const float d = v - acc;
        return std::fabs(d) > deadZone ? acc + Sign(d) * (std::fabs(d) - deadZone) : acc;
    }

    /// Strength 0x9EAFC3. Clamped to 1 before the range fade, so values above 1 only saturate.
    inline float HeadStrength(float master, float standing, float moving, bool isMoving, bool falling,
                              bool firstPerson, bool mounted, float rangeFactor)
    {
        if (std::fabs(standing) < kEps && !isMoving && !falling) return 0.0f;
        float k = (isMoving ? moving : standing) * master;
        if (firstPerson) k *= 2.0f;
        if (mounted) k *= 0.5f;
        return Clamp01(k) * rangeFactor;
    }

    // ---- target focus (0xA35DA3) -----------------------------------------------------------------------

    /// WrapPi 0x6EA01B.
    inline float WrapPi(float x)
    {
        x = std::fmod(x, kTwoPi);
        if (x < -kPi) x += kTwoPi;
        else if (x > kPi) x -= kTwoPi;
        return x;
    }

    /// Heading2D 0x6E50B2: [0, 2pi), 0 for a vertical vector.
    inline float Heading2D(float x, float y)
    {
        const float l = std::sqrt(x * x + y * y);
        if (l < kEps) return 0.0f;
        const float a = std::acos(std::clamp(x / l, -1.0f, 1.0f));
        return y < 0.0f ? kTwoPi - a : a;
    }

    inline float YawOf(Vec3 fwd) { return std::atan2(fwd.y, fwd.x); }

    struct FocusIn
    {
        Vec3  aim;                  // target position + 0.6 * height
        Vec3  pivot;                // orbit centre: base + shoulder offset + pivot height
        Vec3  base;                 // followed unit's position
        Vec3  eye;                  // final camera position
        float curYaw, curPitch;     // of the finished orientation, world space
        float curDist;              // current boom
        float fov;                  // GetFov, radians
        float strengthPitch, strengthYaw; // 0..1
    };

    /// The (pitch, yaw) offset focus wants this frame. False when the aim point is on top of the eye.
    inline bool FocusDesired(const FocusIn& in, float& dP, float& dY)
    {
        const Vec3  v       = in.aim - in.pivot;
        const float heading = Heading2D(v.x, v.y);
        const float dist    = std::sqrt(LenSq(in.aim - in.base));

        // fades, 1 = full effect
        const float fDist = dist > 50.0f ? 1.0f - Clamp01((dist - 50.0f) * (1.0f / 30.0f)) : 1.0f;
        const float fYaw  = 1.0f - Clamp01((std::fabs(WrapPi(heading - in.curYaw)) - 1.0472f) * 0.477465f);
        float fPitch = 1.0f;
        if (in.curPitch > 0.523599f)
            fPitch = 1.0f - Clamp01((in.curPitch - 0.523599f) / (kPitchLimit - 0.523599f));
        const float fade = fYaw * fPitch * fDist;

        // virtual aim point: the target as seen from the pivot, pulled in to a maximum radius
        const float maxLen = std::max(in.curDist * 1.5f, 5.0f);
        Vec3 w = v;
        if (LenSq(w) > maxLen * maxLen) w = w * (maxLen / std::sqrt(LenSq(w)));
        Vec3 d = (in.pivot + w) - in.eye;
        if (LenSq(d) < 0.1f) return false;
        d = d * (1.0f / std::sqrt(LenSq(d)));

        dP = WrapPi(PitchOf(d) - in.curPitch) * in.strengthPitch * fade;
        dY = WrapPi(YawOf(d) - in.curYaw) * in.strengthYaw * fade;

        float aY = std::fabs(dY);                                    // yaw only: soft knee above 20 deg
        if (aY > 0.349066f) { aY = 0.349066f + (aY - 0.349066f) * 0.3f; dY = Sign(dY) * aY; }

        if (std::fabs(dP) > 0.15f * in.fov) dP = Sign(dP) * 0.15f * in.fov;   // caps: fraction of the FoV
        if (aY > 0.25f * in.fov) dY = Sign(dY) * 0.25f * in.fov;
        return true;
    }

    // ---- orientation matrix (rows forward / left / up, positive pitch looks down) ---------------------

    struct Mat33 { Vec3 r[3]; };
    inline bool Finite(const Mat33& m) { return Finite(m.r[0]) && Finite(m.r[1]) && Finite(m.r[2]); }

    /// transpose(Rz(yaw) * Ry(pitch)): what C33Matrix__FromEulerAnglesZYX 0x4C56D0 builds at roll 0.
    inline Mat33 FromYawPitch(float yaw, float pitch)
    {
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        return { { { cp * cy, cp * sy, -sp }, { -sy, cy, 0.0f }, { sp * cy, sp * sy, cp } } };
    }

    /// C33Matrix__RotateLocalY 0x4C55B0: pitch about the matrix's own left row, positive = look down.
    inline void RotateLocalY(Mat33& m, float angle)
    {
        const float c = std::cos(angle), s = std::sin(angle);
        const Vec3 f = m.r[0], u = m.r[2];
        m.r[0] = f * c - u * s;
        m.r[2] = f * s + u * c;
    }
}
