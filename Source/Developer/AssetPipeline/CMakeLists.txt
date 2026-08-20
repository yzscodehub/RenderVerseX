# =============================================================================
# Tools Module - Asset pipeline and utilities
# =============================================================================
add_library(RVX_AssetPipeline STATIC)

target_sources(RVX_AssetPipeline PRIVATE
    Private/AssetPipeline.cpp
    Private/ShaderProgramCatalog.cpp
    Private/AssetDatabase.cpp
    Private/AnimationRootMotionDeriver.cpp
    Private/Importers/AudioImporter.cpp
)

target_include_directories(RVX_AssetPipeline PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_AssetPipeline PUBLIC
    RVX::Audio
    RVX::Core
    RVX::RHI
    RVX::ShaderContracts
)

target_link_libraries(RVX_AssetPipeline PRIVATE
    RVX::AssetImport
    RVX::ResourceRuntime
    RVX::ShaderCompiler
)

# Optional: Link asset processing libraries when available
# target_link_libraries(RVX_AssetPipeline PRIVATE
#     assimp::assimp
#     stb_image
# )

add_library(RVX::AssetPipeline ALIAS RVX_AssetPipeline)

if(RVX_BUILD_PROGRAMS)
    add_executable(RVXCook
        Private/CookMain.cpp
    )

    target_link_libraries(RVXCook PRIVATE
        RVX::AssetPipeline
    )

    target_compile_features(RVXCook PRIVATE cxx_std_20)

    add_executable(RVXAnimationDerive
        Private/AnimationRootMotionDeriveMain.cpp
    )

    target_link_libraries(RVXAnimationDerive PRIVATE
        RVX::AssetPipeline
    )

    target_compile_features(RVXAnimationDerive PRIVATE cxx_std_20)
endif()

rvx_register_module_target(
    TARGET RVX_AssetPipeline
    MODULE AssetPipeline
    ALIASES RVX::AssetPipeline
)
if(RVX_BUILD_PROGRAMS)
    rvx_register_module_target(
        TARGET RVXCook
        MODULE AssetPipeline
    )
    rvx_register_module_target(
        TARGET RVXAnimationDerive
        MODULE AssetPipeline
    )
endif()
