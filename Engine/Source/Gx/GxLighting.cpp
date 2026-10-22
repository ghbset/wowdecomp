#include "Engine/Source/Gx/GxLighting.h"

// CUSTOM: set by the D3D11 and Vulkan devices while they are up (GxLighting.h)
IGxLighting* g_gxLighting = nullptr;

// CUSTOM: the lit shaders' constants (GxLighting.h); all zero = off
GxLightingGpu g_gxLightingGpu = {};
uint32_t g_gxLightingGpuSerial = 0;
