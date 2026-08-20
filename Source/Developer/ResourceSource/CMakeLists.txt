# =============================================================================
# ResourceSource - development source composition and change observation
# =============================================================================
add_library(RVX_ResourceSource STATIC
    Private/PollingResourceChangeMonitor.cpp
    Private/ResourceSourceComposition.cpp
)

target_include_directories(RVX_ResourceSource PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_ResourceSource PUBLIC
    RVX::ResourceRuntime
)

target_link_libraries(RVX_ResourceSource PRIVATE
    RVX::AssetImport
    RVX::Core
)

add_library(RVX::ResourceSource ALIAS RVX_ResourceSource)

rvx_register_module_target(
    TARGET RVX_ResourceSource
    MODULE ResourceSource
    ALIASES RVX::ResourceSource
)
