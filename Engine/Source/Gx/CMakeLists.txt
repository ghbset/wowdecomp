file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

if (NOT WIN32)
    list(FILTER PRIVATE_SOURCES EXCLUDE REGEX "/CGxDevice(D3d|D3d9Ex|D3d11|Vulkan|OpenGL)/")
endif ()

add_library(Gx STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(Gx
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(Gx
    PRIVATE
        d3d9.lib
)

# CGxDeviceVulkan: Khronos Vulkan-Headers only (pinned tag v1.4.364); vulkan-1.dll is loaded at run time, no import lib.
set(WOWDECOMP_VULKAN_HEADERS "/root/d3d11-port/vendor/Vulkan-Headers/include" CACHE PATH "Vulkan-Headers include dir")
target_include_directories(Gx PRIVATE ${WOWDECOMP_VULKAN_HEADERS})

if (WIN32)
    target_link_libraries(Gx
        PRIVATE
            nvapi
            opengl32.lib
    )
endif ()
