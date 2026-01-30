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
    RVX::Core
)

# Include Audio headers for AudioImporter
target_include_directories(RVX_Tools PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Audio/Include>
)

# Optional: Link asset processing libraries when available
# target_link_libraries(RVX_Tools PRIVATE
#     assimp::assimp
#     stb_image
# )

add_library(RVX::Tools ALIAS RVX_Tools)
