# =============================================================================
# AssetImport - development-only source importer adapters
# =============================================================================
add_library(RVX_AssetImport STATIC
    Private/AudioLoader.cpp
    Private/SourceResourceLoaders.cpp
)

target_include_directories(RVX_AssetImport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_AssetImport PUBLIC
    RVX::Resource
    RVX::ResourceRuntime
)

target_link_libraries(RVX_AssetImport PRIVATE
    RVX::AudioCodec
    RVX::Core
)

add_library(RVX::AssetImport ALIAS RVX_AssetImport)

rvx_register_module_target(
    TARGET RVX_AssetImport
    MODULE AssetImport
    ALIASES RVX::AssetImport
)
