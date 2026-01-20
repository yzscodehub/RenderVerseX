# =============================================================================
# Acceleration Module - Ray casting and spatial acceleration structures
# =============================================================================
add_library(RVX_Acceleration INTERFACE)

target_include_directories(RVX_Acceleration INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Acceleration INTERFACE
    RVX::Core
    RVX::Scene
)

add_library(RVX::Acceleration ALIAS RVX_Acceleration)
