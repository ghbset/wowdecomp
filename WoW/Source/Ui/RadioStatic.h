// Radio (port of wxl-radio): the between-stations sound, generated rather than shipped as a file. Kept apart from BASS so
// Tools/RadioCheck/check.cpp can run it natively.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Radio
{
    // Tuning knobs, by ear: nobody has measured these.
    constexpr float kStaticHiss      = 0.8f;    // softened white noise
    constexpr float kStaticFlutterHz = 7.0f;    // how fast the hiss wavers
    constexpr float kStaticWhistle   = 0.12f;   // the heterodyne whine sweeping through
    constexpr float kStaticSweepHz   = 0.9f;    // sweeps of the whine per second
    constexpr float kStaticLowHz     = 500.0f, kStaticHighHz = 2700.0f;   // range of the whine

    struct StaticGen { uint32_t rng = 0x1234567u; float low = 0, phase = 0, t = 0; };

    /// Mono float samples in [-1, 1].
    inline void FillStatic(StaticGen& g, float* out, size_t count, float rate)
    {
        constexpr float kTau = 6.2831853f;
        for (size_t i = 0; i < count; ++i)
        {
            g.rng = g.rng * 1664525u + 1013904223u;
            const float white = static_cast<float>(static_cast<int32_t>(g.rng)) * (1.0f / 2147483648.0f);
            g.low += 0.35f * (white - g.low);                                  // take the harsh top off
            g.t += 1.0f / rate;
            if (g.t > 1000.0f) g.t -= 1000.0f;                                 // keep float precision
            const float sweep = 0.5f + 0.5f * std::sin(kTau * kStaticSweepHz * g.t);
            g.phase += kTau * (kStaticLowHz + (kStaticHighHz - kStaticLowHz) * sweep * sweep) / rate;
            if (g.phase > kTau) g.phase -= kTau;
            const float flutter = 0.75f + 0.25f * std::sin(kTau * kStaticFlutterHz * g.t);
            out[i] = g.low * kStaticHiss * flutter + std::sin(g.phase) * kStaticWhistle;
        }
    }
}
