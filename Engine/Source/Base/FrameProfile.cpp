#include "Engine/Source/Base/FrameProfile.h"
#include <windows.h>

// CUSTOM (Badgermilk0 port): performance diagnostics; see FrameProfile.h

FrameProfileStats g_frameProfile;
void (*g_frameProfileGpuMark)(FrameProfileSection section);

static uint64_t s_ticks[FRAMEPROFILE_COUNT];
static uint32_t s_counters[FRAMEPROFILE_COUNTERS];
static FrameProfileSection s_current = FRAMEPROFILE_NONE;
static uint64_t s_mark;

uint64_t FrameProfileNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

void FrameProfileAdd(FrameProfileSection section, uint64_t start, uint64_t end) {
    s_ticks[section] += end - start;
}

void FrameProfileSwitch(FrameProfileSection section) {
    uint64_t now = FrameProfileNow();

    if (s_current != FRAMEPROFILE_NONE) {
        s_ticks[s_current] += now - s_mark;
    }

    s_current = section;
    s_mark = now;

    if (g_frameProfileGpuMark) {
        g_frameProfileGpuMark(section);
    }
}

void FrameProfileGpuRecord(const double* ms, double frame) {
    // the CPU sections' smoothing (FrameProfileEndFrame)
    const double k = 0.02;

    for (uint32_t i = 0; i < FRAMEPROFILE_COUNT; i++) {
        g_frameProfile.m_gpuMs[i] += (ms[i] - g_frameProfile.m_gpuMs[i]) * k;
    }

    g_frameProfile.m_gpuFrame += (frame - g_frameProfile.m_gpuFrame) * k;
    g_frameProfile.m_gpuValid = true;
}

void FrameProfileCount(FrameProfileCounter counter, uint32_t value) {
    s_counters[counter] += value;
}

void FrameProfileEndFrame() {
    static double s_msPerTick;

    if (!s_msPerTick) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        s_msPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);
    }

    // the smoothing GxFrameStatsRecord uses: about a second at typical frame rates
    const double k = 0.02;

    for (uint32_t i = 0; i < FRAMEPROFILE_COUNT; i++) {
        g_frameProfile.m_ms[i] += (static_cast<double>(s_ticks[i]) * s_msPerTick - g_frameProfile.m_ms[i]) * k;
        s_ticks[i] = 0;
    }

    for (uint32_t i = 0; i < FRAMEPROFILE_COUNTERS; i++) {
        g_frameProfile.m_counters[i] += (static_cast<double>(s_counters[i]) - g_frameProfile.m_counters[i]) * k;
        s_counters[i] = 0;
    }
}
