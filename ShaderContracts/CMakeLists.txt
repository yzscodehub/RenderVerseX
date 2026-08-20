# =============================================================================
# ShaderContracts - runtime shader program request and reflection contracts
# =============================================================================
add_library(RVX_ShaderContracts STATIC
    Private/ShaderLayout.cpp
)

target_include_directories(RVX_ShaderContracts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_ShaderContracts PUBLIC
    RVX::Core
    RVX::RHI
)

add_library(RVX::ShaderContracts ALIAS RVX_ShaderContracts)

rvx_register_module_target(
    TARGET RVX_ShaderContracts
    MODULE ShaderContracts
    ALIASES RVX::ShaderContracts
)
