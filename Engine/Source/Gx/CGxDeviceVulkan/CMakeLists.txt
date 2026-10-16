# The Vulkan backend: CGxDeviceVulkan (the CGxDevice subclass the engine's factory creates for GxApi_Vulkan)
# over the engine-agnostic Vulkan tier under Vulkan/ (loader, context, memory, frame, textures, pipelines).
#
# A target of its own, apart from Gx, for one reason: the C++ standard. The decompiled engine is built as
# C++17 and stays that way; the Vulkan tier is C++20. Gx and GxVulkan need each other (Gx.cpp calls
# CreateCGxDeviceVulkan, the device calls into CGxDevice), which two static libraries can do: the executable
# links both and the MSVC linker resolves across them in any order.
file(GLOB_RECURSE GX_VULKAN_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(GxVulkan STATIC
    ${GX_VULKAN_SOURCES}
)

set_target_properties(GxVulkan PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(GxVulkan
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(GxVulkan
    PUBLIC
        VulkanHeaders
)
