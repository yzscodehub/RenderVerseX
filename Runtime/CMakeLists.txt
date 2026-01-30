# =============================================================================
# Runtime Module - Engine runtime subsystems
# =============================================================================
# Provides engine subsystems for:
# - Window management (WindowSubsystem)
# - Input handling (InputSubsystem, InputActionMap, Gamepad, Touch, Gestures)
# - Time/clock utilities
# - Camera (to be migrated)
# =============================================================================

add_library(RVX_Runtime STATIC)

target_sources(RVX_Runtime PRIVATE
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
    Private/Camera/Camera.cpp
    Private/Camera/FPSController.cpp
    Private/Camera/OrbitController.cpp
    Private/Camera/CameraRegistry.cpp
)

target_include_directories(RVX_Runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Runtime PUBLIC
    RVX_Core
    RVX_HAL
)

# Windows-specific: Link XInput for gamepad vibration
if(WIN32)
    target_link_libraries(RVX_Runtime PRIVATE xinput)
endif()

add_library(RVX::Runtime ALIAS RVX_Runtime)
