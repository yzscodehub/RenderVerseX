# =============================================================================
# Tools Module - Asset pipeline and utilities
# =============================================================================
add_library(RVX_Tools STATIC)

target_sources(RVX_Tools PRIVATE
    Private/AssetPipeline.cpp
    Private/AssetDatabase.cpp
    Private/Importers/AudioImporter.cpp
)

target_include_directories(RVX_Tools PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Tools PUBLIC
    RVX::Audio
    RVX::Core
    RVX::RHI
)

target_link_libraries(RVX_Tools PRIVATE
    RVX::Resource
    RVX::ShaderCompiler
)

# Optional: Link asset processing libraries when available
# target_link_libraries(RVX_Tools PRIVATE
#     assimp::assimp
#     stb_image
# )

add_library(RVX::Tools ALIAS RVX_Tools)

add_executable(RVXCook
    Private/CookMain.cpp
)

target_link_libraries(RVXCook PRIVATE
    RVX::Tools
)

target_compile_features(RVXCook PRIVATE cxx_std_20)
