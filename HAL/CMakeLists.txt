# =============================================================================
# HAL Module - Hardware Abstraction Layer
# =============================================================================
# Provides platform-independent abstractions for:
# - Window management
# - Input handling (low-level backend)
# - Gamepad support
# - File system access
# - Timer/Clock utilities
# =============================================================================
add_library(RVX_HAL STATIC)

target_sources(RVX_HAL PRIVATE
    # GLFW implementations
    Private/GLFW/GLFWWindow.cpp
    Private/GLFW/GLFWInputBackend.cpp
    Private/GLFW/GLFWGamepadBackend.cpp
    # Input
    Private/Input.cpp
    Private/InputBackendFactory.cpp
    Private/Input/Input.cpp
)

if(APPLE)
    target_sources(RVX_HAL PRIVATE
        Private/Apple/GLFWMetalLayerBridge.mm
    )
    target_link_libraries(RVX_HAL PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
    )
    set_source_files_properties(
        Private/Apple/GLFWMetalLayerBridge.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
endif()

target_include_directories(RVX_HAL PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(RVX_HAL PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
)

target_link_libraries(RVX_HAL PUBLIC
    RVX_Core
    glfw
)

# Windows-specific: Link XInput for gamepad vibration
if(WIN32)
    target_link_libraries(RVX_HAL PRIVATE xinput)
endif()

add_library(RVX::HAL ALIAS RVX_HAL)
