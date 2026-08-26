# =============================================================================
# RenderVerseSamples - one host executable with independent sample scenes
# =============================================================================

add_executable(RenderVerseSamples
    main.cpp
    RegisterSamples.cpp
    Scenes/AnimationCharacterSample.cpp
    Scenes/AutoExposureSample.cpp
    Scenes/AssetGalleryShowcaseSample.cpp
    Scenes/AssetStreamingSample.cpp
    Scenes/GPUDrivenShowcaseSample.cpp
    Scenes/InteriorRenderingSample.cpp
    Scenes/LightingShadowShowcaseSample.cpp
    Scenes/ModelRenderingSample.cpp
    Scenes/ModelViewerSample.cpp
    Scenes/PBRMaterialsSample.cpp
    Scenes/PhysicsSandboxSample.cpp
    Scenes/PhysicalExposureSample.cpp
    Scenes/PostProcessingShowcaseSample.cpp
    Scenes/RenderPipelineShowcaseSample.cpp
    Scenes/RenderingStressSample.cpp
    Scenes/SceneLifecycleSample.cpp
    Scenes/StylizedMaterialsSample.cpp
    Scenes/TemporalAASample.cpp
    Scenes/TransparentMaterialsSample.cpp
)
target_include_directories(RenderVerseSamples PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(RenderVerseSamples PRIVATE
    RVX::Animation
    RVX::AnimationSceneAdapters
    RVX::Core
    RVX::Engine
    RVX::EngineDevelopmentComposition
    RVX::HAL
    RVX::Physics
    RVX::Render
    RVX::ResourceRuntime
    RVX::ResourceSceneAdapters
    RVX::RHI
    RVX::RHI_BackendFactory
    RVX::RuntimeServices
    RVX::SampleCommon
    RVX::Scene
    RVX::World
)

if(RVX_ENABLE_DX11)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_Vulkan)
endif()
if(RVX_ENABLE_METAL)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_Metal)
endif()
if(RVX_ENABLE_OPENGL)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_OpenGL)
endif()

file(GLOB_RECURSE RVX_SAMPLE_ASSET_INPUTS CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/water-bottle/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/corset/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/casual-female/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/crytek-sponza/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/cooked/casual-female/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/cooked/casual-female-root-motion/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/environments/kloofendal-48d-partly-cloudy-pure-sky/*"
)
list(APPEND RVX_SAMPLE_ASSET_INPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/shadow-plane-caster/scene.gltf"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/r7-triangle/scene.gltf"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/pbr-material-grid/scene.gltf"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/pbr-material-texture-cube/scene.gltf"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/catalog.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/Manifests/renderverse-samples.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.generated-fixtures.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.quaternius-cc0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.khronos-sample-assets-cc0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.crytek-sponza-cc-by-3.0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.polyhaven-cc0.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetRecipes/stylized-materials-toon.rvxprofile"
    "${CMAKE_CURRENT_SOURCE_DIR}/AssetRecipes/transparent-materials-v1.rvxprofile"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMake/StageSampleAssets.cmake"
)

set(RVX_SAMPLE_RUNTIME_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
set(RVX_SAMPLE_ASSET_DESTINATION
    "${RVX_SAMPLE_RUNTIME_DIRECTORY}/Assets/Samples")
set(RVX_SAMPLE_ASSET_RECEIPT
    "${RVX_SAMPLE_ASSET_DESTINATION}/.rvx-staged")
add_custom_command(
    OUTPUT "${RVX_SAMPLE_ASSET_RECEIPT}"
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
        "-DDESTINATION_ROOT=${RVX_SAMPLE_ASSET_DESTINATION}"
        "-DSTAMP_PATH=${RVX_SAMPLE_ASSET_RECEIPT}"
        "-DHDR_FIXTURE_WRITER=$<TARGET_FILE:RVXSampleHDRFixtureWriter>"
        "-DINTERIOR_FIXTURE_WRITER=$<TARGET_FILE:RVXSampleInteriorFixtureWriter>"
        "-DTRANSPARENT_MATERIALS_FIXTURE_WRITER=$<TARGET_FILE:RVXSampleTransparentMaterialsFixtureWriter>"
        "-DCOOK_EXECUTABLE=$<TARGET_FILE:RVXCook>"
        "-DCOOKED_CATALOG_WRITER=$<TARGET_FILE:RVXSampleCookedCatalogWriter>"
        "-DTOON_COOK_PROFILE=${CMAKE_CURRENT_SOURCE_DIR}/AssetRecipes/stylized-materials-toon.rvxprofile"
        "-DTRANSPARENT_MATERIALS_COOK_PROFILE=${CMAKE_CURRENT_SOURCE_DIR}/AssetRecipes/transparent-materials-v1.rvxprofile"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/StageSampleAssets.cmake"
    DEPENDS
        ${RVX_SAMPLE_ASSET_INPUTS}
        RVXSampleHDRFixtureWriter
        RVXSampleInteriorFixtureWriter
        RVXSampleTransparentMaterialsFixtureWriter
        RVXCook
        RVXSampleCookedCatalogWriter
    COMMENT "Staging a clean, dependency-tracked RenderVerseSamples asset tree"
    VERBATIM
)
add_custom_target(RVXSampleAssets DEPENDS "${RVX_SAMPLE_ASSET_RECEIPT}")
add_dependencies(RenderVerseSamples RVXSampleAssets)
set_target_properties(RVXSampleAssets PROPERTIES FOLDER "Samples/Assets")
rvx_register_module_target(
    TARGET RVXSampleAssets
    MODULE Samples
)

set_target_properties(RenderVerseSamples PROPERTIES
    FOLDER "Samples/Product"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
)

rvx_register_module_target(
    TARGET RenderVerseSamples
    MODULE Samples
)
