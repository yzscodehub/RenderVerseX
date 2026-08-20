# =============================================================================
# ResourceRuntime - runtime loader contracts and deterministic loader catalog
# =============================================================================
add_library(RVX_ResourceRuntime STATIC
    Private/ResourceLoaderCatalog.cpp
)

target_include_directories(RVX_ResourceRuntime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_ResourceRuntime PUBLIC
    RVX::AssetContracts
    RVX::Core
)

add_library(RVX::ResourceRuntime ALIAS RVX_ResourceRuntime)

rvx_register_module_target(
    TARGET RVX_ResourceRuntime
    MODULE ResourceRuntime
    ALIASES RVX::ResourceRuntime
)
