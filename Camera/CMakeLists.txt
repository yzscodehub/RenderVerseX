# =============================================================================
# Camera Module - Camera data and controllers
# =============================================================================
add_library(RVX_Camera STATIC)

target_sources(RVX_Camera PRIVATE
    Private/Camera.cpp
    Private/OrbitController.cpp
    Private/FPSController.cpp
    Private/CameraSystem.cpp
)

target_include_directories(RVX_Camera PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Camera PUBLIC
    RVX_Core
    RVX_Input
)

add_library(RVX::Camera ALIAS RVX_Camera)
