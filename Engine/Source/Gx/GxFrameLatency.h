#ifndef ENGINE_SOURCE_GX_GXFRAMELATENCY_H
#define ENGINE_SOURCE_GX_GXFRAMELATENCY_H

// CUSTOM (Badgermilk0 port). gxFixLag ("prevent cursor lag") used to wait at every present until the GPU had finished the
// frame just submitted, so the CPU and GPU took turns: a frame cost CPU time plus GPU time and both sat idle half the
// time. This keeps the point of it, a short GPU queue so the cursor does not trail the picture, but lets the CPU build
// the next frame while the GPU draws the last one: at present it waits for the frame g_gxMaxFrameLatency frames back.
// 0 is the stock behaviour (wait for the frame just submitted).

#include <cstdint>
#include <windows.h>
#include <d3d9.h>

// gxMaxFrameLatency, 0..3
extern uint32_t g_gxMaxFrameLatency;

// CUSTOM (Badgermilk0 port), performance diagnostics. Where a frame's time goes, smoothed, in milliseconds (GetFrameTimings() in Lua):
// the CPU building the frame, waiting for the GPU (gxFixLag), waiting for the frame cap (maxFPS), and Present (which
// blocks for vsync and a full swap chain)
struct GxFrameStats {
    double m_cpu;
    double m_gpuWait;
    double m_capWait;
    double m_present;
    double m_frame;
};

extern GxFrameStats g_gxFrameStats;

// ScenePresent marks each phase with these
uint64_t GxFrameStatsNow();
void GxFrameStatsRecord(uint64_t frameStart, uint64_t gpuWaitStart, uint64_t capStart, uint64_t presentStart, uint64_t presentEnd);

class CGxFrameLatency {
    public:
    static const uint32_t RING = 4;

    IDirect3DQuery9* m_queries[RING] = {};
    uint32_t m_next = 0;
    uint32_t m_issued = 0;

    // at the end of a frame, before Present
    void EndFrame(IDirect3DDevice9* device);
    // the device was lost, reset or destroyed
    void Release();
};

#endif
