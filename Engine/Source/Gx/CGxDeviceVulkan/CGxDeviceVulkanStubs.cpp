// The Vulkan backend's virtuals with nothing to do on the device: the render-state send (the draw
// reads the states itself), the depth read-back (D3D9 cannot read its depth buffer either), the
// point and line sizes, stereo. Where the base class has a complete engine-side implementation it
// is called, so the engine's own bookkeeping stays correct; the rest answer the way the engine
// expects.
#include <cstdint>
#include <cstring>
#include "Engine/Source/Gx/CGxDeviceVulkan/CGxDeviceVulkan.h"

void CGxDeviceVulkan::PrimPointSize(float size) {
}

void CGxDeviceVulkan::PrimLineWidth(float width) {
}

// IRsSync calls this for each state whose app value differs from the shadow and then copies the
// value into the shadow itself; the draw reads the app array (the blend mode, the textures, the
// stage ops, the alpha reference, the shaders, the depth and culling states, the colour mask, the
// polygon offset, the blend factor), so nothing is sent here.
void CGxDeviceVulkan::IRsSendToHw(EGxRenderState which) {
}

// Mirrors CGxDeviceD3d::DeviceReadDepths (0x006A1950): zeros of the right size.
void CGxDeviceVulkan::DeviceReadDepths(CiRect& rect, TSGrowableArray<float>& depths) {
    this->ClampRectToWindow(rect);

    int32_t width = rect.maxX - rect.minX;
    int32_t height = rect.maxY - rect.minY;

    if (width <= 0 || height <= 0) {
        depths.SetCount(0);
        return;
    }

    depths.SetCount(static_cast<uint32_t>(width) * static_cast<uint32_t>(height));
    memset(depths.Ptr(), 0, depths.Count() * sizeof(float));
}

// --- stereo: not offered (m_stereoAvailable is 0) ---------------------------------------------------

void CGxDeviceVulkan::StereoSetConvergence(float convergence) {
}

float CGxDeviceVulkan::StereoGetConvergence() {
    return 0.0f;
}

void CGxDeviceVulkan::StereoSetSeparation(float separation) {
}

float CGxDeviceVulkan::StereoGetSeparation() {
    return 0.0f;
}

bool CGxDeviceVulkan::StereoEnabled() {
    return false;
}

void CGxDeviceVulkan::DeviceUnk81(uint32_t a, uint32_t b) {
}

void CGxDeviceVulkan::DeviceUnk82() {
}

void CGxDeviceVulkan::DeviceUnk83(uint32_t a, uint32_t b) {
}
