// Host-side check of WoW/Source/UIUtil/ActionCamMath.h: Tools/ActionCamCheck/run.sh
// Dynamic-pitch numbers are the worked examples of /root/RE/camera/legion/re-dynamic-pitch.md 1.4.
#include "WoW/Source/UIUtil/ActionCamMath.h"

#include <cstdio>
#include <cstdlib>

using namespace ActionCamMath;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failed; } } while (0)

static float Deg(float r) { return r * 180.0f / kPi; }
static float Rad(float d) { return d * kPi / 180.0f; }

// Steady state of dynamic pitch at defaults: fov pi/2, pad 0.4, down scale 0.25, strength 1.
static float DynPitch(float p, float d, float h)
{
    const float half = (kPi / 2.0f) * 0.6f * 0.5f;
    return DynamicPitchAngle(p, FeetAngle(p, d, h), half, 0.4f, 0.25f, 1.0f);
}

static void TestDynamicPitch()
{
    CHECK(std::fabs(DynPitch(0.0f, 10.0f, 2.0f) - (-0.08535f)) < 1e-4f);
    CHECK(std::fabs(Deg(DynPitch(Rad(20.0f), 20.0f, 2.0f)) - (-7.682f)) < 1e-2f);
    CHECK(std::fabs(Deg(FeetAngle(0.0f, 10.0f, 2.0f)) - 11.310f) < 1e-2f);
    CHECK(std::fabs(Deg(FeetAngle(Rad(20.0f), 20.0f, 2.0f)) - 25.192f) < 1e-2f);

    const float pitches[5] = { -20.0f, 0.0f, 10.0f, 20.0f, 45.0f };
    const float dists[4]   = { 5.0f, 10.0f, 20.0f, 30.0f };
    const float table[4][5] = {
        {  2.986f,   5.000f,   3.374f,  1.463f, -1.616f },
        { -2.863f,  -4.890f,  -4.551f, -4.343f, -3.920f },
        { -6.355f, -10.489f,  -8.955f, -7.682f, -5.330f },
        { -7.483f, -12.386f, -10.479f, -8.859f, -5.845f },
    };
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 5; ++j)
        {
            const float got = Deg(DynPitch(Rad(pitches[j]), dists[i], 2.0f));
            if (std::fabs(got - table[i][j]) >= 0.01f)
                std::printf("  table d=%g pitch=%g: got %.3f want %.3f\n", dists[i], pitches[j], got, table[i][j]);
            CHECK(std::fabs(got - table[i][j]) < 0.01f);
        }

    CHECK(DynPitch(0.0f, 1.0f, 2.0f) <= 0.0872665f + 1e-6f);                 // +5 deg cap
    CHECK(DynamicPitchAngle(0.3f, 0.2f, 0.47f, 0.4f, 0.25f, 0.0f) == 0.0f);  // strength 0 = nothing
    CHECK(std::fabs(DynPitch(-kPitchLimit, 10.0f, 2.0f)) < 1e-6f);           // looking up fades to zero

    // the first-frame pad reproduces the current framing: with it, the angle is 0
    const float half = 0.471239f, L = FeetAngle(0.1f, 12.0f, 2.0f);
    const float pad0 = InitialPad(L, 0.1f, half);
    CHECK(pad0 > 0.0f && pad0 < 1.0f);
    CHECK(std::fabs(L - 0.1f - (1.0f - pad0) * half) < 1e-5f);

    PadInputs in{ 0.4f, 0.75f, false, false, 0.0f, 0.4f };
    CHECK(TargetFovPad(in) == 0.4f);
    in.flying = true;
    CHECK(TargetFovPad(in) == 0.75f);
    in.falling = true;                                               // falling overrides flying
    CHECK(TargetFovPad(in) == 0.4f);
    in.fallSpeed = 60.0f;
    CHECK(TargetFovPad(in) == 0.75f);
    CHECK(std::fabs(FallSpeed(0.0f, 1000, false) - 19.291105f) < 1e-4f);
    CHECK(FallSpeed(0.0f, 60000, false) == 60.148f && FallSpeed(0.0f, 60000, true) == 7.0f);

    CHECK(SmartPivotDistance(0.0f, 8.0f) == 8.0f);
    CHECK(std::fabs(SmartPivotDistance(Rad(-70.0f), 8.0f) - 1.0f) < 1e-3f);
}

static void TestSprings()
{
    // converges, monotonic, never overshoots (critically damped)
    for (float fps : { 20.0f, 60.0f, 240.0f })
        for (float omega : { 12.0f, 20.0f, 30.0f })
        {
            Spring s{ 0.0f, 0.0f };
            float prev = 0.0f;
            bool done = false;
            for (int i = 0; i < static_cast<int>(fps * 3.0f) && !done; ++i)
            {
                done = SmoothDamp(s, 1.0f, omega, 1.0f / fps, 0.0f);
                CHECK(s.value >= prev && s.value <= 1.0f);
                prev = s.value;
            }
            CHECK(done && s.value == 1.0f && s.velocity == 0.0f);
        }

    // frame-rate independence: the value after 0.1 s barely depends on the step
    auto after = [](float fps, float omega) {
        Spring s{ 0.0f, 0.0f };
        for (int i = 0; i < static_cast<int>(fps * 0.1f + 0.5f); ++i) SmoothDamp(s, 1.0f, omega, 1.0f / fps, 0.0f);
        return s.value;
    };
    CHECK(std::fabs(after(30.0f, 30.0f) - after(240.0f, 30.0f)) < 0.02f);
    CHECK(std::fabs(after(60.0f, 20.0f) - after(240.0f, 20.0f)) < 0.02f);
    const float exact = 1.0f - (1.0f + 30.0f * 0.1f) * std::exp(-30.0f * 0.1f);    // analytic critical damping
    CHECK(std::fabs(after(240.0f, 30.0f) - exact) < 0.01f);

    // a spring already at the target does nothing; one moving away from it comes back
    Spring s{ 2.0f, 0.0f };
    CHECK(SmoothDamp(s, 2.0f, 30.0f, 0.016f, 0.0f) && s.value == 2.0f);
    s = { 0.0f, -5.0f };
    for (int i = 0; i < 600; ++i) SmoothDamp(s, 1.0f, 30.0f, 1.0f / 60.0f, 0.0f);
    CHECK(s.value == 1.0f);

    // the 2-D spring: converges to the pair, same rate independence
    auto after2 = [](float fps) {
        float cur[2] = { 0.0f, 0.0f }, vel[2] = { 0.0f, 0.0f };
        const float tgt[2] = { 0.2f, -0.3f };
        for (int i = 0; i < static_cast<int>(fps * 0.1f + 0.5f); ++i) SmoothCD2(cur, vel, tgt, 20.0f, 1.0f / fps, 0.0f);
        return cur[1];
    };
    CHECK(std::fabs(after2(30.0f) - after2(240.0f)) < 0.01f);
    float cur[2] = { 0.0f, 0.0f }, vel[2] = { 0.0f, 0.0f };
    const float tgt[2] = { 0.2f, -0.3f };
    bool done = false;
    for (int i = 0; i < 600 && !done; ++i)
    {
        done = SmoothCD2(cur, vel, tgt, 20.0f, 1.0f / 60.0f, 0.0f);
        CHECK(cur[0] >= 0.0f && cur[0] <= 0.2f && cur[1] <= 0.0f && cur[1] >= -0.3f);
    }
    CHECK(done && cur[0] == 0.2f && cur[1] == -0.3f);

    // exponential damping is exactly frame-rate independent
    float a = 0.0f, b = 0.0f;
    for (int i = 0; i < 6; ++i) a = ExpDamp(a, 1.0f, 10.0f, 0.1f / 6.0f);
    for (int i = 0; i < 24; ++i) b = ExpDamp(b, 1.0f, 10.0f, 0.1f / 24.0f);
    CHECK(std::fabs(a - b) < 1e-5f && std::fabs(a - (1.0f - std::exp(-1.0f))) < 1e-5f);
}

static void TestZoom()
{
    CHECK(ZoomStartRate(5.0f, 4.5f, 1.0f, false) == 30.0f);                 // within 1 yd
    CHECK(ZoomStartRate(5.0f, 4.0f, 1.0f, false) == 30.0f);
    CHECK(std::fabs(ZoomStartRate(15.0f, 5.0f, 1.0f, false) - 1.5f) < 1e-5f); // >= 10 yd
    CHECK(ZoomStartRate(15.0f, 5.0f, 1.0f, true) == 20.0f);

    for (float fps : { 30.0f, 144.0f })
    {
        ZoomState z;
        ZoomSnap(z, 5.0f);
        float prev = 5.0f;
        uint32_t now = 1000;
        int frames = 0;
        do
        {
            ZoomStep(z, 15.0f, 15.0f, 1.0f, false, now, 1.0f / fps);
            CHECK(z.s.value >= prev && z.s.value <= 15.0f);          // no overshoot
            prev = z.s.value;
            now += static_cast<uint32_t>(1000.0f / fps);
        } while (z.running && ++frames < 2000);
        CHECK(!z.running && z.s.value == 15.0f && frames > 3);
    }
}

static void TestHead()
{
    float a, b;
    HeadRange(5.0f, 1.0f, a, b);
    CHECK(a == 5.0f && b == 10.0f);
    HeadRange(20.0f, 3.0f, a, b);                                    // size capped at 1.5, fade width at 30 s
    CHECK(a == 30.0f && b == 45.0f);
    CHECK(HeadRangeFactor(4.0f, 5.0f, 10.0f) == 1.0f && HeadRangeFactor(15.0f, 5.0f, 10.0f) == 0.0f);
    CHECK(std::fabs(HeadRangeFactor(10.0f, 5.0f, 10.0f) - 0.5f) < 1e-6f);

    CHECK(DeadZoneStep(0.0f, 0.01f, 0.015f) == 0.0f);                // inside the zone: nothing
    CHECK(std::fabs(DeadZoneStep(0.0f, 0.05f, 0.015f) - 0.035f) < 1e-6f);
    CHECK(std::fabs(DeadZoneStep(0.0f, -0.05f, 0.015f) + 0.035f) < 1e-6f);

    CHECK(HeadStrength(1.0f, 0.3f, 0.5f, false, false, false, false, 1.0f) == 0.3f);
    CHECK(HeadStrength(1.0f, 0.3f, 0.5f, true, false, false, false, 1.0f) == 0.5f);
    CHECK(HeadStrength(1.0f, 0.3f, 0.5f, true, false, true, false, 0.5f) == 0.5f);  // x2, clamp 1, then fade
    CHECK(HeadStrength(1.0f, 0.0f, 0.5f, false, false, false, false, 1.0f) == 0.0f);
    CHECK(HeadStrength(1.0f, 0.3f, 0.5f, true, false, false, true, 1.0f) == 0.25f);
}

static void TestFocus()
{
    CHECK(std::fabs(WrapPi(3.0f * kPi) - kPi) < 1e-5f || std::fabs(WrapPi(3.0f * kPi) + kPi) < 1e-5f);
    CHECK(std::fabs(WrapPi(-1.5f * kPi) - 0.5f * kPi) < 1e-5f);
    CHECK(std::fabs(Heading2D(0.0f, -1.0f) - 1.5f * kPi) < 1e-5f && Heading2D(0.0f, 0.0f) == 0.0f);

    // camera 10 yd behind a character at the origin, looking along +x; target ahead and to the left
    FocusIn in{};
    in.base = { 0.0f, 0.0f, 0.0f };
    in.pivot = { 0.0f, 0.0f, 2.0f };
    in.eye = { -10.0f, 0.0f, 2.0f };
    in.aim = { 10.0f, 5.0f, 2.0f };
    in.curDist = 10.0f;
    in.fov = kPi / 2.0f;
    in.strengthPitch = 0.4f;
    in.strengthYaw = 0.5f;
    float dP = 9.0f, dY = 9.0f;
    CHECK(FocusDesired(in, dP, dY));
    CHECK(dY > 0.0f && dY <= 0.25f * in.fov && std::fabs(dP) < 1e-4f);
    in.aim.y = -5.0f;                                                // mirrored target, mirrored yaw
    float dP2, dY2;
    CHECK(FocusDesired(in, dP2, dY2) && std::fabs(dY2 + dY) < 1e-5f);
    in.aim = { 10.0f, 0.0f, 12.0f };                                 // above: look up = negative pitch
    CHECK(FocusDesired(in, dP, dY) && dP < 0.0f && dP >= -0.15f * in.fov && std::fabs(dY) < 1e-4f);

    in.aim = { -200.0f, 80.0f, 2.0f };                               // limits hold wherever the target is
    in.strengthPitch = in.strengthYaw = 1.0f;
    CHECK(FocusDesired(in, dP, dY) && std::fabs(dY) <= 0.25f * in.fov + 1e-6f && std::fabs(dP) <= 0.15f * in.fov + 1e-6f);
    in.aim = { 100.0f, 0.5f, 2.0f };                                 // beyond 80 yd: faded out
    CHECK(FocusDesired(in, dP, dY) && dP == 0.0f && dY == 0.0f);
    in.aim = { 10.0f, 5.0f, 2.0f };
    in.curPitch = kPitchLimit;                                       // top-down: faded out
    CHECK(FocusDesired(in, dP, dY) && dY == 0.0f);
    in.curPitch = 0.0f;
    in.curYaw = kPi;                                                 // target straight behind the view: faded out
    in.aim = { 10.0f, 0.0f, 2.0f };
    CHECK(FocusDesired(in, dP, dY) && std::fabs(dY) < 1e-4f);

    in.curYaw = 0.0f;
    in.eye = in.aim;                                                 // within the 15 yd radius, so not pulled in
    CHECK(!FocusDesired(in, dP, dY));                                // aim point on top of the eye
}

static void TestMatrix()
{
    const Mat33 m = FromYawPitch(0.7f, 0.3f);
    CHECK(std::fabs(PitchOf(m.r[0]) - 0.3f) < 1e-5f && std::fabs(YawOf(m.r[0]) - 0.7f) < 1e-5f);
    CHECK(std::fabs(Dot(m.r[0], m.r[1])) < 1e-6f && std::fabs(Dot(m.r[0], m.r[2])) < 1e-6f && m.r[1].z == 0.0f);
    CHECK(m.r[0].z < 0.0f);                                          // positive pitch looks down

    Mat33 r = m;
    RotateLocalY(r, 0.2f);
    const Mat33 want = FromYawPitch(0.7f, 0.5f);
    for (int i = 0; i < 3; ++i) CHECK(LenSq(r.r[i] - want.r[i]) < 1e-10f);
}

int main()
{
    TestDynamicPitch();
    TestSprings();
    TestZoom();
    TestHead();
    TestFocus();
    TestMatrix();
    if (g_failed) { std::printf("%d check(s) FAILED\n", g_failed); return 1; }
    std::printf("math_test: all checks passed\n");
    return 0;
}
