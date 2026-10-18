#ifndef ENGINE_GX_CGXDEVICED3D11_D3D11UTIL_H
#define ENGINE_GX_CGXDEVICED3D11_D3D11UTIL_H

// Helpers shared by the CGxDeviceD3d11*.cpp files.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include "Engine/Source/Gx/CGxDeviceD3d11/CGxDeviceD3d11.h"
#include "Engine/Source/Storm/H/storm.h"

// m_stateDirty bits
enum {
    Dirty_Raster = 0x1,
    Dirty_Blend = 0x2,
    Dirty_Depth = 0x4,
};

// IStateGet kinds
enum {
    State_Raster,
    State_Blend,
    State_Depth,
    State_Sampler,
    State_InputLayout,
};

// Every line flushed: an exit hang otherwise loses gx.log's tail (VkLog pattern, shared Log untouched).
inline void D3d11Log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    CGxDevice::Log(format, args);
    va_end(args);

    if (CGxDevice::m_log) {
        SLogFlush(CGxDevice::m_log);
    }
}

// Once per distinct message, so a failure repeated every frame shows up once in gx.log.
inline void D3d11LogOnce(const char* format, ...) {
    static std::unordered_set<std::string> s_seen;
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (s_seen.insert(buffer).second) {
        D3d11Log("%s", buffer);
    }
}

template <class T>
inline void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

inline void UploadConstants(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, uint32_t size) {
    D3D11_MAPPED_SUBRESOURCE mapped;

    if (buffer && SUCCEEDED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, data, size);
        context->Unmap(buffer, 0);
    }
}

#endif
