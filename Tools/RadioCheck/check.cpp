// clang++ -std=c++17 -I. Tools/RadioCheck/check.cpp -o /tmp/rc && /tmp/rc   (from the repo root)
#include "WoW/Source/Ui/RadioStreamTitle.h"
#include "WoW/Source/Ui/RadioStatic.h"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace Radio;
    char t[8] = "keep";
    assert(ParseStreamTitle("StreamTitle='A - B';StreamUrl='';", t, sizeof t) && !std::strcmp(t, "A - B"));
    assert(ParseStreamTitle("StreamTitle='It's on';", t, sizeof t) && !std::strcmp(t, "It's on"));
    assert(ParseStreamTitle("StreamTitle='0123456789';", t, sizeof t) && !std::strcmp(t, "0123456"));  // truncated
    assert(ParseStreamTitle("StreamTitle='';", t, sizeof t) && !t[0]);
    std::strcpy(t, "keep");
    assert(!ParseStreamTitle("StreamUrl='x';", t, sizeof t) && !ParseStreamTitle("StreamTitle='open", t, sizeof t)
           && !ParseStreamTitle(nullptr, t, sizeof t) && !std::strcmp(t, "keep"));
    assert(IsStreamUrl("http://a/b") && IsStreamUrl("https://a") && !IsStreamUrl("file:///c") && !IsStreamUrl("C:\\x.mp3")
           && !IsStreamUrl("") && !IsStreamUrl(nullptr));
    StaticGen g; static float buf[44100 * 3]; float peak = 0; double sum = 0;
    FillStatic(g, buf, 44100 * 3, 44100.0f);
    for (float v : buf) { assert(v >= -1.0f && v <= 1.0f); peak = std::fabs(v) > peak ? std::fabs(v) : peak; sum += v; }
    assert(peak > 0.3f && std::fabs(sum / (44100 * 3)) < 0.01);   // audible, never clips, no DC offset
    std::puts("title_test ok");
}
