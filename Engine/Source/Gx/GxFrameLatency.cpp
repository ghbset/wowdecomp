#include "Engine/Source/Gx/GxFrameLatency.h"

// CUSTOM (Badgermilk0 port), see GxFrameLatency.h

uint32_t g_gxMaxFrameLatency = 1;

GxFrameStats g_gxFrameStats;

uint64_t GxFrameStatsNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

void GxFrameStatsRecord(uint64_t frameStart, uint64_t gpuWaitStart, uint64_t capStart, uint64_t presentStart, uint64_t presentEnd) {
    static uint64_t s_lastPresentEnd;
    static double s_msPerClock;

    if (!s_msPerClock) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        s_msPerClock = 1000.0 / static_cast<double>(frequency.QuadPart);
    }

    if (s_lastPresentEnd) {
        // about a second of smoothing at typical frame rates
        const double k = 0.02;
        const auto mix = [k](double& average, double sample) {
            average += (sample - average) * k;
        };

        // the game's own work: from the last Present returning to the frame being handed to the GPU
        mix(g_gxFrameStats.m_cpu, static_cast<double>(gpuWaitStart - s_lastPresentEnd) * s_msPerClock);
        mix(g_gxFrameStats.m_gpuWait, static_cast<double>(capStart - gpuWaitStart) * s_msPerClock);
        mix(g_gxFrameStats.m_capWait, static_cast<double>(presentStart - capStart) * s_msPerClock);
        mix(g_gxFrameStats.m_present, static_cast<double>(presentEnd - presentStart) * s_msPerClock);
        mix(g_gxFrameStats.m_frame, static_cast<double>(presentEnd - s_lastPresentEnd) * s_msPerClock);
    }

    s_lastPresentEnd = presentEnd;
}

void CGxFrameLatency::EndFrame(IDirect3DDevice9* device) {
    if (!device) {
        return;
    }

    uint32_t latency = g_gxMaxFrameLatency < RING ? g_gxMaxFrameLatency : RING - 1;
    uint32_t slot = this->m_next;

    if (!this->m_queries[slot] && FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &this->m_queries[slot]))) {
        this->m_queries[slot] = nullptr;
        return;
    }

    this->m_queries[slot]->Issue(D3DISSUE_END);
    this->m_issued++;
    this->m_next = (slot + 1) % RING;

    // the frame issued `latency` frames ago (this one for 0); nothing to wait for until that many have gone out
    if (this->m_issued <= latency) {
        return;
    }

    IDirect3DQuery9* query = this->m_queries[(slot + RING - latency) % RING];

    if (!query) {
        return;
    }

    while (query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        SwitchToThread();
    }
}

void CGxFrameLatency::Release() {
    for (IDirect3DQuery9*& query : this->m_queries) {
        if (query) {
            query->Release();
            query = nullptr;
        }
    }

    this->m_next = 0;
    this->m_issued = 0;
}
