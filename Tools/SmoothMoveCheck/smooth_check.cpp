// Self-check for the smoothSplines / smoothTurning port (WoW/Source/Object/SmoothMove.*,
// C3Spline_CatmullRom::Smooth + mode 2). Builds the real spline sources natively:
//   Tools/SmoothMoveCheck/run.sh
#include <cassert>
#include <cmath>
#include <cstdio>
#include "Engine/Source/Tempest/C3Spline_CatmullRom.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/Object/SmoothMove.h"

static float Dist2D(const C3Vector& a, const C3Vector& b) { return std::hypot(a.x - b.x, a.y - b.y); }

// distance from p to the polyline pts[1..n-2]
static float ToPolyline(const C3Vector& p, const C3Vector* pts, int n) {
    float best = 1e9f;
    for (int i = 1; i + 1 < n - 1; i++) {
        C3Vector a = pts[i], b = pts[i + 1];
        float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
        float t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        best = std::fmin(best, std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t)));
    }
    return best;
}

int main() {
    // a ground path as the 3.3.5 client builds it: lead-in, start, corners, end, end control. Shortest
    // segment 2 yd, so 4.3.4 resamples every 2 yd and corners round over about that much.
    C3Vector pts[] = { {-1, 0, 0}, {0, 0, 0}, {10, 0, 1}, {10, 2, 1.5f}, {20, 2, 2}, {20, 2, 2} };
    const int n = 6;
    const float step = 2.0f;
    C3Spline_CatmullRom s;
    s.m_splineMode = 0;
    s.SetPoints(pts, n);
    float linearLen = s.m_length;
    C3Vector linearEnd; s.Pos(1.0f, linearEnd, C3Spline::EVAL_ARCLENGTH);

    s.m_splineMode = 2;
    int ok = s.Smooth(1.0f);
    assert(ok);
    assert(s.m_pointCount == 12);   // lead, 3 segment starts, 3 + 0 + 3 samples (step = 2.06 yd), end, end control

    // same end point (server arrival position) and the start is unchanged
    C3Vector end; s.Pos(1.0f, end, C3Spline::EVAL_ARCLENGTH);
    assert(Dist2D(end, linearEnd) < 1e-4f && std::fabs(end.z - linearEnd.z) < 1e-4f);
    C3Vector start; s.Pos(1e-6f, start, C3Spline::EVAL_ARCLENGTH);
    assert(Dist2D(start, pts[1]) < 1e-3f);

    // smoothed length within 3% of the polyline (Catmull-Rom overshoots a little at uneven knots), so the
    // unchanged duration means at most ~3% speed change
    assert(std::fabs(s.m_length - linearLen) < linearLen * 0.03f);

    // stays within half a step of the server path and Z stays between server heights. Catmull-Rom passes
    // through every knot, corners included: what changes is the heading, which turns smoothly (below).
    // (t = 0 is skipped: C3Spline::Pos returns the lead-in control there, stock behaviour.)
    float maxOff = 0;
    for (int i = 1; i <= 1000; i++) {
        C3Vector p; s.Pos(i / 1000.0f, p, C3Spline::EVAL_ARCLENGTH);
        float off = ToPolyline(p, pts, n);
        maxOff = std::fmax(maxOff, off);
        assert(p.z >= -1e-4f && p.z <= 2.0001f);
    }
    assert(maxOff < 0.5f * step);

    // heading is continuous (no snap at the corner) and level
    float prev = -1;
    for (int i = 1; i < 1000; i++) {
        C44Matrix f; s.Frame(i / 1000.0f, f, C3Spline::EVAL_ARCLENGTH);
        float h = std::atan2(f.a1, f.a0);
        assert(std::fabs(f.a2) < 1e-6f);
        if (prev > -1) assert(std::fabs(SmoothMove::Delta(prev, h)) < 0.25f);
        prev = h;
    }

    // refusals: one segment, or more than 100 points
    C3Vector one[] = { {-1, 0, 0}, {0, 0, 0}, {5, 0, 0}, {5, 0, 0} };
    C3Spline_CatmullRom s1; s1.m_splineMode = 0; s1.SetPoints(one, 4); s1.m_splineMode = 2;
    assert(!s1.Smooth(1.0f));
    C3Vector longp[] = { {-1, 0, 0}, {0, 0, 0}, {200, 0, 0}, {200, 1.5f, 0}, {200, 1.5f, 0} };
    C3Spline_CatmullRom s2; s2.m_splineMode = 0; s2.SetPoints(longp, 5); s2.m_splineMode = 2;
    assert(!s2.Smooth(1.0f));
    assert(s2.m_pointCount == 5);

    // turn maths
    const float pi = 3.14159265f;
    assert(std::fabs(SmoothMove::Delta(0.1f, 2 * pi - 0.1f) + 0.2f) < 1e-5f);
    assert(std::fabs(SmoothMove::Approach(0.0f, pi / 2, 0.5f) - 0.5f) < 1e-6f);
    assert(std::fabs(SmoothMove::Approach(0.1f, 2 * pi - 0.1f, 0.5f) - (2 * pi - 0.1f)) < 1e-5f);
    assert(std::fabs(SmoothMove::Approach(0.0f, 2 * pi - 1.0f, 0.5f) - (2 * pi - 0.5f)) < 1e-5f);

    std::printf("smooth_check: ok (%u points, length %.3f vs %.3f, max offset %.3f)\n", s.m_pointCount, s.m_length, linearLen, maxOff);
    return 0;
}
