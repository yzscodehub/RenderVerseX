# =============================================================================
# AssetImport - development-only source importer adapters
# =============================================================================
add_library(RVX_AssetImport STATIC
    Private/AudioLoader.cpp
    Private/EnvironmentLoader.cpp
    Private/FBXImporter.cpp
    Private/GLTFImporter.cpp
    Private/HDRTextureLoader.cpp
    Private/ModelLoader.cpp
    Private/SourceResourceLoaders.cpp
    Private/TextureLoader.cpp
)

target_include_directories(RVX_AssetImport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_AssetImport PUBLIC
    RVX::Animation
    RVX::Core
    RVX::Geometry
    RVX::ResourceRuntime
)

target_link_libraries(RVX_AssetImport PRIVATE
    RVX::AudioCodec
)

# Source format parsers are development-only dependencies of AssetImport.
find_package(unofficial-tinygltf CONFIG QUIET)
if(NOT unofficial-tinygltf_FOUND)
    find_package(tinygltf CONFIG QUIET)
endif()
if(unofficial-tinygltf_FOUND)
    target_link_libraries(RVX_AssetImport PRIVATE unofficial::tinygltf::tinygltf)
elseif(tinygltf_FOUND)
    target_link_libraries(RVX_AssetImport PRIVATE tinygltf::tinygltf)
else()
    find_path(TINYGLTF_INCLUDE_DIR tiny_gltf.h PATH_SUFFIXES tinygltf)
    if(TINYGLTF_INCLUDE_DIR)
        target_include_directories(RVX_AssetImport PRIVATE ${TINYGLTF_INCLUDE_DIR})
    else()
        message(WARNING "RVX_AssetImport: tinygltf not found; glTF source import is unavailable")
    endif()
endif()

option(RVX_ENABLE_FBX "Enable development FBX import via ufbx" ON)
if(RVX_ENABLE_FBX)
    include(FetchContent)
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
    FetchContent_Declare(
        ufbx
        GIT_REPOSITORY https://github.com/ufbx/ufbx.git
        GIT_TAG v0.14.3
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(ufbx)
    if(NOT ufbx_POPULATED)
        FetchContent_Populate(ufbx)
    endif()
    if(EXISTS "${ufbx_SOURCE_DIR}/ufbx.h" AND EXISTS "${ufbx_SOURCE_DIR}/ufbx.c")
        target_sources(RVX_AssetImport PRIVATE "${ufbx_SOURCE_DIR}/ufbx.c")
        target_include_directories(RVX_AssetImport PRIVATE "${ufbx_SOURCE_DIR}")
        target_compile_definitions(RVX_AssetImport PRIVATE HAS_UFBX=1)
    else()
        message(WARNING "RVX_AssetImport: ufbx not found; FBX source import is unavailable")
    endif()
endif()

find_path(TINYEXR_INCLUDE_DIR tinyexr.h PATH_SUFFIXES tinyexr)
if(TINYEXR_INCLUDE_DIR)
    target_include_directories(RVX_AssetImport PRIVATE ${TINYEXR_INCLUDE_DIR})
    target_compile_definitions(RVX_AssetImport PRIVATE HAS_TINYEXR=1)
    find_package(miniz CONFIG QUIET)
    if(miniz_FOUND)
        target_link_libraries(RVX_AssetImport PRIVATE miniz::miniz)
    else()
        find_path(MINIZ_INCLUDE_DIR miniz.h PATH_SUFFIXES miniz)
        if(MINIZ_INCLUDE_DIR)
            target_include_directories(RVX_AssetImport PRIVATE ${MINIZ_INCLUDE_DIR})
        endif()
    endif()
endif()

add_library(RVX::AssetImport ALIAS RVX_AssetImport)

rvx_register_module_target(
    TARGET RVX_AssetImport
    MODULE AssetImport
    ALIASES RVX::AssetImport
)
