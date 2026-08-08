# =============================================================================
# Samples - Example applications
# =============================================================================

add_subdirectory(Common)
add_subdirectory(RenderVerseSamples)

if(RVX_BUILD_LEGACY_SAMPLES)
    add_subdirectory(Basic)
    add_subdirectory(Showcase)
endif()
