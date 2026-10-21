#include <cmath>
#include <cstddef>
#include <cstdint>
#include "Engine/Source/Tempest/C3Spline_CatmullRom.h"
#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/Tempest/C34Matrix.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CMath.h"

// STATIC: 0x00AC37B8
static const C44Matrix s_catmullRomBasis(-0.5f, 1.0f, -0.5f, 0.0f, 1.5f, -2.5f, 0.0f, 1.0f, -1.5f, 2.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f);

// STATIC: 0x00AC37F8
static const C34Matrix s_catmullRomDer1 = { -1.5f, 2.0f, -0.5f, 4.5f, -5.0f, 0.0f, -4.5f, 4.0f, 0.5f, 1.5f, -1.0f, 0.0f };

// VERIFY: e9bcea7f auto
// OFFSET: 0x00409740
C3Spline_CatmullRom::C3Spline_CatmullRom() {
    this->splineMode = MODE_CATMULLROM;
}

// VERIFY: 0afb68ad auto
// OFFSET: 0x00409760
SPLINE_MODE C3Spline_CatmullRom::GetSplineMode() {
    return this->splineMode;
}

// VERIFY: f3bb76de manual
// OFFSET: 0x004C3FD0
void C3Spline_CatmullRom::Evaluate(uint32_t seg, float t, C3Vector& pos) {
    if (this->splineMode != MODE_LINEAR) {
        C3Spline::Evaluate(seg, t, s_catmullRomBasis, pos);

        // CUSTOM: mode 2 (4.3.4 0x004C9C10) = Catmull-Rom in X/Y, linear Z (smoothed ground paths)
        if (this->splineMode == MODE_SMOOTHGROUND) {
            const C3Vector& a = this->Point(seg + 1);
            const C3Vector& b = this->Point(seg + 2);
            pos.z = (b.z - a.z) * t + a.z;
        }

        return;
    }

    const C3Vector& a = this->Point(seg + 1);
    const C3Vector& b = this->Point(seg + 2);
    const C3Vector& base = this->Point(seg + 1);

    C3Vector result((b.x - a.x) * t + base.x, (b.y - a.y) * t + base.y, (b.z - a.z) * t + base.z);
    pos = result;
}

// VERIFY: f0c48125 manual
// OFFSET: 0x004C40B0
float C3Spline_CatmullRom::SegLength(uint32_t seg) {
    if (this->splineMode != MODE_LINEAR) {
        return C3Spline::SegLength(seg, s_catmullRomBasis);
    }

    const C3Vector& a = this->Point(seg + 1);
    const C3Vector& b = this->Point(seg + 2);

    float dy = b.y - a.y;
    float dz = b.z - a.z;
    float dx = b.x - a.x;

    return sqrt(dx * dx + dz * dz + dy * dy);
}

// VERIFY: 6f5dd97e manual
// OFFSET: 0x004C4140
void C3Spline_CatmullRom::ParametricSegT(float t, uint32_t& seg, float& segT) {
    float segCount = static_cast<float>(this->m_pointCount - 3);

    seg = CMath::fint_(segCount * t - 0.5f);
    segT = (t - 1.0f / segCount * seg) * segCount;
}

// VERIFY: 90d58011 auto
// OFFSET: 0x004C41B0
float C3Spline_CatmullRom::ILength() {
    return C3Spline::ILength(this->m_pointCount - 3);
}

// VERIFY: a9242506 manual
// OFFSET: 0x004C41C0
void C3Spline_CatmullRom::IValidateCache() {
    uint32_t segCount = this->m_pointCount - 3;

    for (uint32_t i = 0; i < segCount; i++) {
        this->SetCachedSegLength(this->SegLength(i), i);
    }
}

// VERIFY: 093a92f1 auto
// OFFSET: 0x004C4230
void C3Spline_CatmullRom::IPosArclength(float t, C3Vector& pos) {
    uint32_t seg;
    float segT;

    this->ArclengthSegT(t, s_catmullRomBasis, this->m_pointCount - 3, seg, segT);
    this->Evaluate(seg, segT, pos);
}

// VERIFY: 20c0de0e auto
// OFFSET: 0x004C4280
void C3Spline_CatmullRom::IPosParametric(float t, C3Vector& pos) {
    uint32_t seg;
    float segT;

    this->ParametricSegT(t, seg, segT);
    this->Evaluate(seg, segT, pos);
}

// VERIFY: 7e41051b auto
// OFFSET: 0x004C42C0
void C3Spline_CatmullRom::IVelArclength(float t, C3Vector& vel) {
    float clamped;

    if (t < 0.0f) {
        clamped = 0.0f;
    } else if (t < 1.0f) {
        clamped = t;
    } else {
        clamped = 1.0f;
    }

    uint32_t seg;
    float segT;

    this->ArclengthSegT(clamped, s_catmullRomBasis, this->m_pointCount - 3, seg, segT);
    C3Spline::EvaluateDer1(seg, segT, C44Matrix(s_catmullRomDer1), vel);
}

// VERIFY: f98a7b11 auto
// OFFSET: 0x004C4340
void C3Spline_CatmullRom::IVelParametric(float t, C3Vector& vel) {
    float clamped;

    if (t < 0.0f) {
        clamped = 0.0f;
    } else if (t < 1.0f) {
        clamped = t;
    } else {
        clamped = 1.0f;
    }

    uint32_t seg;
    float segT;

    this->ParametricSegT(clamped, seg, segT);
    C3Spline::EvaluateDer1(seg, segT, C44Matrix(s_catmullRomDer1), vel);
}

// VERIFY: 9b45b540 manual
// OFFSET: 0x004C43B0
void C3Spline_CatmullRom::IFrameArclength(float t, C44Matrix& frame) {
    uint32_t seg;
    float segT;

    this->ArclengthSegT(t, s_catmullRomBasis, this->m_pointCount - 3, seg, segT);
    this->Evaluate(seg, segT, *reinterpret_cast<C3Vector*>(&frame.d0));

    const C3Vector& a = this->Point(seg + 1);
    const C3Vector& b = this->Point(seg + 2);

    C3Vector dir(b.x - a.x, b.y - a.y, b.z - a.z);
    float dirSq = dir.z * dir.z + dir.y * dir.y + dir.x * dir.x;

    if (dirSq > 0.00000023841858f) {
        float inv = 1.0f / sqrt(dirSq);
        dir.x = dir.x * inv;
        dir.y = dir.y * inv;
        dir.z = inv * dir.z;
    }

    if (this->splineMode == MODE_SMOOTHGROUND) {
        // CUSTOM: mode 2 (4.3.4 0x004CA370): heading = the horizontal tangent, no pitch
        C3Vector vel(0.0f, 0.0f, 0.0f);
        C3Spline::EvaluateDer1(seg, segT, C44Matrix(s_catmullRomDer1), vel);
        vel.z = 0.0f;
        float velSq = vel.x * vel.x + vel.y * vel.y;

        if (velSq > 0.000099999997f) {
            float inv = 1.0f / sqrt(velSq);
            frame.a0 = vel.x * inv;
            frame.a1 = vel.y * inv;
        } else {
            frame.a0 = dir.x;
            frame.a1 = dir.y;
        }

        frame.a2 = 0.0f;
    } else if (this->splineMode != MODE_LINEAR) {
        C3Vector vel(0.0f, 0.0f, 0.0f);
        C3Spline::EvaluateDer1(seg, segT, C44Matrix(s_catmullRomDer1), vel);

        float velSq = vel.z * vel.z + vel.y * vel.y + vel.x * vel.x;

        if (velSq > 0.000099999997f) {
            float inv = 1.0f / sqrt(velSq);
            vel.x = vel.x * inv;
            vel.y = vel.y * inv;
            vel.z = inv * vel.z;

            if (vel.z * dir.z + vel.y * dir.y + vel.x * dir.x < 0.5f) {
                frame.a0 = dir.x;
                frame.a1 = dir.y;
                frame.a2 = dir.z;
            } else {
                frame.a0 = vel.x;
                frame.a1 = vel.y;
                frame.a2 = vel.z;
            }
        }
    } else {
        frame.a0 = dir.x;
        frame.a1 = dir.y;
        frame.a2 = dir.z;
    }

    frame.b0 = -frame.a1;
    frame.b1 = frame.a0;
    frame.b2 = 0.0f;

    float sideSq = frame.b1 * frame.b1 + frame.b0 * frame.b0;

    if (sideSq > 0.00000023841858f) {
        float inv = 1.0f / sqrt(sideSq);
        frame.b0 = inv * frame.b0;
        frame.b1 = inv * frame.b1;
    }

    frame.c0 = -(frame.a2 * frame.b1);
    frame.c1 = frame.a2 * frame.b0;
    frame.c2 = frame.a0 * frame.b1 - frame.a1 * frame.b0;
}

// VERIFY: 936d407a manual
// OFFSET: 0x004C4600
uint32_t C3Spline_CatmullRom::IGetPoints(float t, C3Vector* points, uint32_t count) {
    uint32_t pointCount = this->m_pointCount;
    uint32_t seg;
    float segT;

    this->ArclengthSegT(t, s_catmullRomBasis, pointCount - 3, seg, segT);

    uint32_t remaining = pointCount - seg - 3;

    if (!points) {
        return remaining;
    }

    if (remaining >= count) {
        remaining = count;
    }

    for (uint32_t i = 0; i < remaining; i++) {
        points[i] = this->Point(seg + 2 + i);
    }

    return remaining;
}

// CUSTOM: port of 4.3.4 C3Spline_CatmullRom::Smooth (0x004CABF0, pathSmoothing). Resamples the current
// (linear, cache valid) path piecewise-linearly every `step` yd, keeping the corner points, where step =
// the shortest segment longer than minLength (at least minLength). The caller then evaluates it as
// Catmull-Rom (mode 2): corners round over about one step, the end point and end controls are kept, and
// the lead-in control keeps its direction at step length. Returns 0 (spline untouched) if it would need
// more than 100 points or there are fewer than 2 segments. keepStart (4.3.4: cyclic re-entry) is not used.
int32_t C3Spline_CatmullRom::Smooth(float minLength) {
    const int32_t maxAllowed = 100;
    int32_t segCount = static_cast<int32_t>(this->m_pointCount) - 3;

    if (segCount <= 1 || this->cachedLength <= 0.0f) {
        return 0;
    }

    float stepLength = this->GetCachedSegLength(0);

    if (minLength > stepLength) {
        stepLength = minLength;
    }

    for (int32_t i = 1; i < segCount; i++) {
        float length = this->GetCachedSegLength(i);

        if (length > minLength && stepLength > length) {
            stepLength = length;
        }
    }

    float maxPointsF = this->cachedLength / stepLength + static_cast<float>(segCount + 4);

    if (!(maxPointsF <= static_cast<float>(maxAllowed))) {
        return 0;
    }

    int32_t maxPoints = static_cast<int32_t>(maxPointsF);
    C3Vector points[maxAllowed];
    points[0] = this->points[0];

    float totalLength = this->cachedLength;
    float traveled = 0.0f;
    int32_t count = 1;
    SPLINE_MODE mode = this->splineMode;
    this->splineMode = MODE_LINEAR;

    for (int32_t seg = 0; seg < segCount && count < maxPoints; seg++) {
        const C3Vector& point = this->Point(seg + 1);
        C3Vector delta = points[count - 1] - point;

        if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z >= 0.00000023841858f) {
            points[count++] = point;
        }

        float length = this->GetCachedSegLength(seg);

        if (length > stepLength) {
            int32_t steps = static_cast<int32_t>(length / stepLength);
            float step = length / static_cast<float>(steps);

            for (int32_t i = 0; i < steps - 1 && count < maxPoints; i++) {
                float t = ((static_cast<float>(i) + 1.0f) * step + traveled) / totalLength;
                C3Vector pos;

                if (t >= 1.0f) {
                    pos = this->Point(this->m_pointCount - 2);
                } else {
                    this->IPosArclength(t, pos);
                }

                C3Vector last = points[count - 1] - pos;

                if (last.x * last.x + last.y * last.y + last.z * last.z >= 0.00000023841858f) {
                    points[count++] = pos;
                }
            }
        }

        traveled += length;
    }

    this->splineMode = mode;

    if (count >= maxPoints - 2 || count < 3) {
        return 0;
    }

    C3Vector dir = points[0] - points[1];
    float dirSq = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;

    if (dirSq > 0.00000023841858f) {
        float inv = 1.0f / sqrt(dirSq);
        dir.x *= inv;
        dir.y *= inv;
        dir.z *= inv;
    }

    points[0].x = points[1].x + dir.x * stepLength;
    points[0].y = points[1].y + dir.y * stepLength;
    points[0].z = points[1].z + dir.z * stepLength;

    // the last path point was added by the loop only if it starts a segment; add it and the end control
    C3Vector end = this->Point(segCount + 1);
    C3Vector endControl = this->Point(segCount + 2);
    points[count++] = end;
    points[count++] = endControl;

    this->ISetPoints(points, count);

    if (this->m_pointCount > 3) {
        this->IValidateCache();
        this->cachedLength = this->ILength();
    }

    return 1;
}

// VERIFY: ad1247b6 auto
// OFFSET: 0x004C4DA0
void C3Spline_CatmullRom::ISetPoints(const C3Vector* points, uint32_t count) {
    if (count) {
        uint32_t segCount = count - 3;
        this->m_segCount = segCount;

        if (segCount > 25) {
            this->m_extraSegLength.SetCount(segCount - 25);
        }
    } else {
        this->m_segCount = 0;
        this->m_extraSegLength.SetCount(0);
    }

    C3Spline::ISetPoints(points, count);
}

// VERIFY: 4e0fa75c manual
// OFFSET: 0x004F4AE0
CDataStore& operator>>(CDataStore& msg, C3Spline_CatmullRom& spline) {
    uint32_t count = 0;
    msg.Get(count);

    void* points;
    msg.GetDataInSitu(points, sizeof(C3Vector) * count);

    uint8_t mode;
    msg.Get(mode);
    spline.splineMode = static_cast<SPLINE_MODE>(mode);

    if (count && msg.m_read <= msg.m_size) {
        spline.SetPoints(static_cast<const C3Vector*>(points), count);
    }

    return msg;
}
