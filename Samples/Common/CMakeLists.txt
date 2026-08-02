# =============================================================================
# Sample Common - shared CLI and report helpers for sample applications
# =============================================================================
add_library(RVX_SampleCommon STATIC
    Private/ModelCameraFraming.cpp
    Private/RuntimeFrameDriver.cpp
    Private/SampleCLI.cpp
)

target_include_directories(RVX_SampleCommon PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
)

target_link_libraries(RVX_SampleCommon PUBLIC
    RVX::Core
    RVX::Engine
    RVX::Render
    RVX::RHI
)

add_library(RVX::SampleCommon ALIAS RVX_SampleCommon)
