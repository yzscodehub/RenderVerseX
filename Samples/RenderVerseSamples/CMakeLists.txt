# =============================================================================
# RenderVerseSamples - one host executable with independent sample scenes
# =============================================================================

add_executable(RenderVerseSamples
    main.cpp
    RegisterSamples.cpp
    Scenes/AnimationCharacterSample.cpp
    Scenes/AssetGalleryShowcaseSample.cpp
    Scenes/AssetStreamingSample.cpp
    Scenes/GPUDrivenShowcaseSample.cpp
    Scenes/InteriorRenderingSample.cpp
    Scenes/LightingShadowShowcaseSample.cpp
    Scenes/ModelRenderingSample.cpp
    Scenes/ModelViewerSample.cpp
    Scenes/PBRMaterialsSample.cpp
    Scenes/PhysicsSandboxSample.cpp
    Scenes/RenderPipelineShowcaseSample.cpp
    Scenes/RenderingStressSample.cpp
    Scenes/SceneLifecycleSample.cpp
)
add_dependencies(RenderVerseSamples
    RVXSampleHDRFixtureWriter
    RVXSampleInteriorFixtureWriter
)

target_include_directories(RenderVerseSamples PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(RenderVerseSamples PRIVATE
    RVX::Animation
    RVX::AnimationSceneAdapters
    RVX::Core
    RVX::Engine
    RVX::HAL
    RVX::Physics
    RVX::Render
    RVX::Resource
    RVX::ResourceSceneAdapters
    RVX::RHI
    RVX::RHI_BackendFactory
    RVX::Runtime
    RVX::SampleCommon
    RVX::Scene
    RVX::ShaderCompiler
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

set(RVX_MODEL_RENDERING_FIXTURE
    "${CMAKE_SOURCE_DIR}/Tests/Fixtures/ModelViewer/ShadowPlaneCaster.gltf")
set(RVX_MODEL_VIEWER_FIXTURE
    "${CMAKE_SOURCE_DIR}/Tests/Fixtures/ModelViewer/R7Triangle.gltf")
set(RVX_PBR_MATERIAL_GRID_FIXTURE
    "${CMAKE_SOURCE_DIR}/Tests/Fixtures/Samples/PBRMaterialGrid.gltf")
set(RVX_PBR_MATERIAL_TEXTURE_CUBE_FIXTURE
    "${CMAKE_SOURCE_DIR}/Tests/Fixtures/Samples/PBRMaterialTextureCube.gltf")
add_custom_command(TARGET RenderVerseSamples POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/shadow-plane-caster"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/r7-triangle"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/pbr-material-grid"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/pbr-material-texture-cube"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/interior-rendering-p0a"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/environments/pbr-reference"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/water-bottle"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/water-bottle"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/corset"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/corset"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/casual-female"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/casual-female"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/models/crytek-sponza"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/crytek-sponza"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/cooked/casual-female"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/cooked/casual-female-root-motion"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female-root-motion"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/environments/kloofendal-48d-partly-cloudy-pure-sky"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/environments/kloofendal-48d-partly-cloudy-pure-sky"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RVX_MODEL_RENDERING_FIXTURE}"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/shadow-plane-caster/scene.gltf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RVX_MODEL_VIEWER_FIXTURE}"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/r7-triangle/scene.gltf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RVX_PBR_MATERIAL_GRID_FIXTURE}"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/pbr-material-grid/scene.gltf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RVX_PBR_MATERIAL_TEXTURE_CUBE_FIXTURE}"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/pbr-material-texture-cube/scene.gltf"
    COMMAND $<TARGET_FILE:RVXSampleInteriorFixtureWriter>
        --output "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/models/interior-rendering-p0a/scene.gltf"
    COMMAND $<TARGET_FILE:RVXSampleHDRFixtureWriter>
        --output "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/environments/pbr-reference/environment.hdr"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/catalog.json"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/catalog.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.generated-fixtures.txt"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/LICENSE.generated-fixtures.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.quaternius-cc0.txt"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/LICENSE.quaternius-cc0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.khronos-sample-assets-cc0.txt"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/LICENSE.khronos-sample-assets-cc0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.crytek-sponza-cc-by-3.0.txt"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/LICENSE.crytek-sponza-cc-by-3.0.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/LICENSE.polyhaven-cc0.txt"
        "$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/LICENSE.polyhaven-cc0.txt"
    VERBATIM
)

set_target_properties(RenderVerseSamples PROPERTIES
    FOLDER "Samples/Product"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
)
