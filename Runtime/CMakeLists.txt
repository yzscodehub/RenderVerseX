# =============================================================================
# Runtime Module - Engine runtime subsystems
# =============================================================================
# Provides engine subsystems for:
# - Window management (WindowSubsystem)
# - Input handling (InputSubsystem)
# - Time/clock utilities
# - Camera (to be migrated)
# =============================================================================

add_library(RVX_Runtime STATIC)

target_sources(RVX_Runtime PRIVATE
    Private/Window/WindowSubsystem.cpp
    Private/Input/InputSubsystem.cpp
    Private/Time/Time.cpp
)

target_include_directories(RVX_Runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Runtime PUBLIC
    RVX_Core
    RVX_Platform
    RVX_Input
)

add_library(RVX::Runtime ALIAS RVX_Runtime)
