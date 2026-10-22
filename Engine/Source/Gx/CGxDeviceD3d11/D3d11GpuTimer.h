#ifndef ENGINE_SOURCE_GX_CGXDEVICED3D11_D3D11GPUTIMER_H
#define ENGINE_SOURCE_GX_CGXDEVICED3D11_D3D11GPUTIMER_H

// CUSTOM (Badgermilk0 port): performance diagnostics, D3D11 device. Times the GPU's work per FrameProfile section: a timestamp query at
// every section switch, bracketed per frame by a disjoint query, read back a few frames later without waiting, and
// handed to FrameProfileGpuRecord. The time between two marks is when the GPU reached them, so it is the work between
// them only while the GPU is busy; when the GPU waits for the CPU (a CPU-bound frame) the gaps include that wait.

#include <cstdint>
#include <d3d11.h>

class D3d11GpuTimer {
    public:
    // the section starting now (FRAMEPROFILE_NONE ends the last)
    void Mark(ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t section);
    // before Present: closes this frame's queries and records every finished frame's results
    void EndFrame(ID3D11Device* device, ID3D11DeviceContext* ctx);
    // the device is going away or being reset
    void Release();

    private:
    static const uint32_t FRAMES = 4;
    static const uint32_t MARKS = 64;

    struct Frame {
        ID3D11Query* m_disjoint = nullptr;
        ID3D11Query* m_stamps[MARKS] = {};
        uint8_t m_sections[MARKS] = {};
        uint32_t m_count = 0;
        // its disjoint query was begun this frame
        bool m_open = false;
        // ended, its results not read yet: the slot cannot be reused
        bool m_pending = false;
    };

    Frame m_frames[FRAMES];
    uint32_t m_current = 0;

    bool Stamp(ID3D11Device* device, ID3D11DeviceContext* ctx, Frame& frame, uint32_t section);
    void ReadFinished(ID3D11DeviceContext* ctx);
};

#endif
