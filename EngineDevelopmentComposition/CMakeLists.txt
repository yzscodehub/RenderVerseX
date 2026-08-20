# =============================================================================
# EngineDevelopmentComposition - development-only runtime dependency wiring
# =============================================================================
add_library(RVX_EngineDevelopmentComposition STATIC
    Private/EngineDevelopmentComposition.cpp
)

target_include_directories(RVX_EngineDevelopmentComposition PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_EngineDevelopmentComposition PUBLIC
    RVX::Engine
    RVX::Resource
    RVX::ShaderContracts
)

target_link_libraries(RVX_EngineDevelopmentComposition PRIVATE
    RVX::ShaderCompiler
    RVX::ResourceSource
)

add_library(
    RVX::EngineDevelopmentComposition
    ALIAS RVX_EngineDevelopmentComposition
)

rvx_register_module_target(
    TARGET RVX_EngineDevelopmentComposition
    MODULE EngineDevelopmentComposition
    ALIASES RVX::EngineDevelopmentComposition
)
