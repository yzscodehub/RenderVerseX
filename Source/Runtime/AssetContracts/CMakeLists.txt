# =============================================================================
# AssetContracts - backend-neutral runtime asset identity and load contracts
# =============================================================================
add_library(RVX_AssetContracts STATIC
    Private/ResourceTypes.cpp
)

target_include_directories(RVX_AssetContracts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_AssetContracts PUBLIC
    RVX::Core
)

add_library(RVX::AssetContracts ALIAS RVX_AssetContracts)

rvx_register_module_target(
    TARGET RVX_AssetContracts
    MODULE AssetContracts
    ALIASES RVX::AssetContracts
)
