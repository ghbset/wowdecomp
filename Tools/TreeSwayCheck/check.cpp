// clang++ -std=c++17 -I. Tools/TreeSwayCheck/check.cpp -o /tmp/tsc && /tmp/tsc
#include "Engine/Source/Model2/M2TreeSway.h"
#include <cassert>
#include <cstdio>

namespace M2TreeSway { int32_t s_enabled = 1; float s_strength = 1.0f; }
using namespace M2TreeSway;

int main() {
    assert(Classify("World\\Azeroth\\Elwynn\\PassiveDoodads\\Trees\\ElwynnTreeCanopy01.m2", 0) == Tree);
    assert(Classify("World\\Kalimdor\\Ashenvale\\PassiveDoodads\\Trees\\AshenvaleTree01.m2", 0) == Tree);
    assert(Classify("World\\Generic\\PassiveDoodads\\Trees\\DeadTree01.m2", 0) == None);
    assert(Classify("World\\Azeroth\\Duskwood\\PassiveDoodads\\Trees\\BurntPine01.m2", 0) == None);
    assert(Classify("World\\Generic\\Trees\\TreeStump01.m2", 0) == None);
    assert(Classify("World\\Expansion02\\Doodads\\DragonBlight\\Trees\\DragonblightTree01.m2", 0) == Tree);
    assert(Classify("World\\Lordaeron\\Plaguelands\\BlightTree01.m2", 0) == None);
    assert(Classify("World\\Generic\\Human\\StreetLamp01.m2", 0) == None);
    assert(Classify("World\\Generic\\Trees\\Tree01.m2", 2) == None);          // has lights
    assert(Classify("World\\Azeroth\\Elwynn\\Bush\\ElwynnBush01.m2", 0) == Bush);
    assert(Classify("World\\Azeroth\\Elwynn\\Bush\\ElwynnBush01.m2", 0, false) == None);
    assert(Classify("World\\Generic\\Infernal01.m2", 0) == None);

    // Tilt keeps the origin, is a rotation, stays under ~2 degrees at strength 1, and moves over time.
    State a = Evaluate(kTreeWind, 100.0f, 200.0f, 10.0f, 1.0f);
    State b = Evaluate(kTreeWind, 100.0f, 200.0f, 11.0f, 1.0f);
    float tilt = std::sqrt(a.tx * a.tx + a.ty * a.ty);
    assert(tilt > 0.0f && tilt < 2.0f * 3.14159265f / 180.0f);
    assert(a.tx != b.tx);
    State z = Evaluate(kTreeWind, 100.0f, 200.0f, 10.0f, 0.0f);
    assert(z.tx == 0.0f && z.ty == 0.0f);
    float m[9];
    TiltMatrix(0.3f, -0.2f, m);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float d = m[i * 3] * m[j * 3] + m[i * 3 + 1] * m[j * 3 + 1] + m[i * 3 + 2] * m[j * 3 + 2];
            assert(std::fabs(d - (i == j ? 1.0f : 0.0f)) < 1e-5f);
        }
    // Up (row-vector 0,0,1) leans toward +x, -y.
    assert(m[6] > 0.0f && m[7] < 0.0f);
    std::puts("TreeSwayCheck ok");
}
