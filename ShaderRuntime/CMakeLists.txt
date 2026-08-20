# =============================================================================
# ShaderRuntime - cooked-only runtime shader program loading
# =============================================================================
add_library(RVX_ShaderRuntime STATIC
    Private/CookedShaderProgramProvider.cpp
)

target_include_directories(RVX_ShaderRuntime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_ShaderRuntime PUBLIC
    RVX::ShaderContracts
)

target_link_libraries(RVX_ShaderRuntime PRIVATE
    RVX::Core
    RVX::RHI
)

add_library(RVX::ShaderRuntime ALIAS RVX_ShaderRuntime)

rvx_register_module_target(
    TARGET RVX_ShaderRuntime
    MODULE ShaderRuntime
    ALIASES RVX::ShaderRuntime
)
