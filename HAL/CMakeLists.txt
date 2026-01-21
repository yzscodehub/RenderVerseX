# =============================================================================
# HAL Module - Hardware Abstraction Layer
# =============================================================================
# Provides platform-independent abstractions for:
# - Window management
# - Input handling (low-level backend)
# - File system access
# - Timer/Clock utilities
# =============================================================================
add_library(RVX_HAL STATIC)

target_sources(RVX_HAL PRIVATE
    # GLFW implementations
    Private/GLFW/GLFWWindow.cpp
    Private/GLFW/GLFWInputBackend.cpp
    # Input
    Private/Input.cpp
    Private/Input/Input.cpp
)

target_include_directories(RVX_HAL PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Private>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_HAL PUBLIC
    RVX_Core
    glfw
)

add_library(RVX::HAL ALIAS RVX_HAL)
