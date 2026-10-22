#include "Engine/Source/Gx/CGxDeviceD3d11/D3d11GpuTimer.h"
#include "Engine/Source/Base/FrameProfile.h"

// CUSTOM (Badgermilk0 port): performance diagnostics, see D3d11GpuTimer.h

bool D3d11GpuTimer::Stamp(ID3D11Device* device, ID3D11DeviceContext* ctx, Frame& frame, uint32_t section) {
    if (frame.m_count >= MARKS) {
        return false;
    }

    ID3D11Query*& query = frame.m_stamps[frame.m_count];

    if (!query) {
        D3D11_QUERY_DESC desc = { D3D11_QUERY_TIMESTAMP, 0 };

        if (FAILED(device->CreateQuery(&desc, &query))) {
            query = nullptr;
            return false;
        }
    }

    ctx->End(query);
    frame.m_sections[frame.m_count++] = static_cast<uint8_t>(section);
    return true;
}

void D3d11GpuTimer::Mark(ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t section) {
    Frame& frame = this->m_frames[this->m_current];

    // the GPU has not finished the frame that last used this slot: this one goes untimed
    if (frame.m_pending) {
        return;
    }

    if (!frame.m_open) {
        if (!frame.m_disjoint) {
            D3D11_QUERY_DESC desc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };

            if (FAILED(device->CreateQuery(&desc, &frame.m_disjoint))) {
                frame.m_disjoint = nullptr;
                return;
            }
        }

        ctx->Begin(frame.m_disjoint);
        frame.m_open = true;
        frame.m_count = 0;
    }

    // the last mark is kept for EndFrame's
    if (frame.m_count < MARKS - 1) {
        this->Stamp(device, ctx, frame, section);
    }
}

void D3d11GpuTimer::EndFrame(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    Frame& frame = this->m_frames[this->m_current];

    if (frame.m_open) {
        this->Stamp(device, ctx, frame, FRAMEPROFILE_NONE);
        ctx->End(frame.m_disjoint);
        frame.m_open = false;
        frame.m_pending = true;
        this->m_current = (this->m_current + 1) % FRAMES;
    }

    this->ReadFinished(ctx);
}

void D3d11GpuTimer::ReadFinished(ID3D11DeviceContext* ctx) {
    // oldest first: the slot after the current one was filled longest ago
    for (uint32_t k = 0; k < FRAMES; k++) {
        Frame& frame = this->m_frames[(this->m_current + k) % FRAMES];

        if (!frame.m_pending) {
            continue;
        }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;

        if (ctx->GetData(frame.m_disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            // not done; the later ones are not either
            return;
        }

        uint64_t stamps[MARKS];
        bool complete = true;

        for (uint32_t i = 0; i < frame.m_count; i++) {
            if (ctx->GetData(frame.m_stamps[i], &stamps[i], sizeof(stamps[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
                complete = false;
                break;
            }
        }

        if (!complete) {
            return;
        }

        frame.m_pending = false;

        // the clock changed speed mid-frame (power state), or nothing to measure
        if (disjoint.Disjoint || !disjoint.Frequency || frame.m_count < 2) {
            continue;
        }

        double msPerTick = 1000.0 / static_cast<double>(disjoint.Frequency);
        double ms[FRAMEPROFILE_COUNT] = {};

        for (uint32_t i = 0; i + 1 < frame.m_count; i++) {
            uint32_t section = frame.m_sections[i];

            if (section < FRAMEPROFILE_COUNT && stamps[i + 1] >= stamps[i]) {
                ms[section] += static_cast<double>(stamps[i + 1] - stamps[i]) * msPerTick;
            }
        }

        double total = stamps[frame.m_count - 1] >= stamps[0] ? static_cast<double>(stamps[frame.m_count - 1] - stamps[0]) * msPerTick : 0.0;
        FrameProfileGpuRecord(ms, total);
    }
}

void D3d11GpuTimer::Release() {
    for (Frame& frame : this->m_frames) {
        if (frame.m_disjoint) {
            frame.m_disjoint->Release();
            frame.m_disjoint = nullptr;
        }

        for (ID3D11Query*& query : frame.m_stamps) {
            if (query) {
                query->Release();
                query = nullptr;
            }
        }

        frame.m_count = 0;
        frame.m_open = false;
        frame.m_pending = false;
    }

    this->m_current = 0;
}
