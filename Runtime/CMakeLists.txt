# =============================================================================
# Runtime Module - Engine runtime subsystems
# =============================================================================
# Provides engine subsystems for:
# - Window management (WindowSubsystem)
# - Input handling (InputSubsystem, InputActionMap, Gamepad, Touch, Gestures)
# - Time/clock utilities
# - Camera (to be migrated)
# =============================================================================

add_library(RVX_RuntimeServices STATIC)

target_sources(RVX_RuntimeServices PRIVATE
    # Window
    Private/Window/WindowSubsystem.cpp
    # Input
    Private/Input/InputSubsystem.cpp
    Private/Input/InputActionMap.cpp
    Private/Input/GestureRecognizer.cpp
    Private/Input/VirtualJoystick.cpp
    # Time
    Private/Time/Time.cpp
    Private/Time/TimeSubsystem.cpp
    # Camera
    Private/Camera/FPSController.cpp
    Private/Camera/OrbitCameraRig.cpp
    Private/Camera/OrbitController.cpp
    Private/Camera/CameraRegistry.cpp
)

target_include_directories(RVX_RuntimeServices PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RuntimeServices PUBLIC
    RVX_Core
    RVX_HAL
)

add_library(RVX::RuntimeServices ALIAS RVX_RuntimeServices)

rvx_register_module_target(
    TARGET RVX_RuntimeServices
    MODULE RuntimeServices
    ALIASES RVX::RuntimeServices
)
