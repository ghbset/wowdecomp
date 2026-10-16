// The Vulkan backend's hardware cursor: the engine's 32x32 ARGB cursor image as a Win32 cursor with
// the engine's hotspot, remade when the image changes and shown over the client area by
// WM_SETCURSOR. D3D's SetCursorProperties / ShowCursor pair, on the window instead of the device.
#include <cstdint>
#include <cstring>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"

namespace {
    constexpr int32_t kCursorSize = 32;
}

// Mirrors CGxDeviceD3d::ICursorCreate (0x0068E900): the base decides between the hardware cursor
// (the format asks for it and the caps offer it) and the software one drawn through Draw; with the
// hardware cursor the Win32 cursor is built from the current image.
void CGxDeviceVulkan::ICursorCreate(const CGxFormat& format) {
    CGxDevice::ICursorCreate(format);

    if (this->m_hwCursor) {
        this->m_hwCursorDirty = 1;
        this->ICursorApply();
    }
}

// Mirrors CGxDeviceD3d::ICursorDestroy (0x006A00C0).
void CGxDeviceVulkan::ICursorDestroy() {
    CGxDevice::ICursorDestroy();

    if (this->m_hcursor) {
        if (GetCursor() == this->m_hcursor) {
            SetCursor(LoadCursorA(nullptr, IDC_ARROW));
        }

        DestroyCursor(this->m_hcursor);
        this->m_hcursor = nullptr;
    }
}

// Mirrors CGxDeviceD3d::ICursorDraw (0x0068E810): the software cursor through the base (which
// returns at once with the hardware cursor), then the hardware cursor's image if it changed.
void CGxDeviceVulkan::ICursorDraw() {
    CGxDevice::ICursorDraw();

    if (this->m_hwCursor && this->m_hwCursorDirty && this->m_context) {
        this->ICursorApply();
    }
}

// Mirrors CGxDeviceD3d::CursorSetVisible (0x0068E750): with the hardware cursor and the mouse over
// the client area, the Win32 cursor follows the engine's visibility at once.
void CGxDeviceVulkan::CursorSetVisible(int32_t visible) {
    CGxDevice::CursorSetVisible(visible);

    if (this->m_hwCursor && this->m_context && this->ICursorInClient()) {
        SetCursor(this->m_cursorVisible ? this->m_hcursor : nullptr);
    }
}

// Mirrors CGxDeviceD3d::CursorUnlock (0x0068E7E0): the image changed; rebuilt at the next
// ICursorDraw, once per present.
void CGxDeviceVulkan::CursorUnlock(uint32_t x, uint32_t y) {
    CGxDevice::CursorUnlock(x, y);
    this->m_hwCursorDirty = 1;
}

// Whether the mouse is over the window's client area.
int32_t CGxDeviceVulkan::ICursorInClient() {
    if (!this->m_hwnd) {
        return 0;
    }

    POINT point;
    GetCursorPos(&point);
    ScreenToClient(this->m_hwnd, &point);

    RECT rect;
    GetClientRect(this->m_hwnd, &rect);

    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

// The Win32 cursor from the engine's image: a top-down 32-bit DIB holding the image (the engine's
// uint32 pixels are b, g, r, a bytes, the DIB's own order, and CreateIconIndirect reads the alpha
// channel of a 32-bit colour bitmap) with an empty monochrome mask, the engine's hotspot. The new
// cursor replaces the old one on the window before the old one is destroyed.
void CGxDeviceVulkan::ICursorApply() {
    this->m_hwCursorDirty = 0;

    BITMAPV5HEADER header;
    memset(&header, 0, sizeof(header));
    header.bV5Size = sizeof(header);
    header.bV5Width = kCursorSize;
    header.bV5Height = -kCursorSize;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    HDC dc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP colour = CreateDIBSection(dc, reinterpret_cast<const BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);

    if (!colour || !bits) {
        if (colour) {
            DeleteObject(colour);
        }

        return;
    }

    memcpy(bits, this->m_cursor, sizeof(this->m_cursor));

    HBITMAP mask = CreateBitmap(kCursorSize, kCursorSize, 1, 1, nullptr);

    if (!mask) {
        DeleteObject(colour);
        return;
    }

    ICONINFO info;
    memset(&info, 0, sizeof(info));
    info.fIcon = FALSE;
    info.xHotspot = this->m_cursorHotspotX;
    info.yHotspot = this->m_cursorHotspotY;
    info.hbmMask = mask;
    info.hbmColor = colour;

    HCURSOR cursor = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(colour);

    if (!cursor) {
        return;
    }

    HCURSOR old = this->m_hcursor;
    this->m_hcursor = cursor;

    if (this->m_cursorVisible && this->ICursorInClient()) {
        SetCursor(cursor);
    }

    if (old) {
        DestroyCursor(old);
    }
}
