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
    RVX::HAL
    RVX::Physics
    RVX::Render
    RVX::ResourceRuntime
    RVX::ResourceSceneAdapters
    RVX::RHI
    RVX::RHI_BackendFactory
    RVX::RuntimeServices
    RVX::SampleCommon
    RVX::SampleDevelopmentComposition
    RVX::Scene
    RVX::World
    RVX::Gfx17SampleScenarios
    RVX::Gfx18SampleScenarios
    RVX::Gfx24SampleScenarios
    RVX::Gfx25SampleScenarios
)

# A separately-linked cooked product root demonstrates that the GFX24/GFX25
# qualification scenarios have no runtime dependency on source importers,
# shader compilers, or the broad development Sample host.
add_executable(RenderVerseSamplesCooked
    CookedMain.cpp
    RegisterCookedSamples.cpp
)
target_compile_features(RenderVerseSamplesCooked PRIVATE cxx_std_20)
target_include_directories(RenderVerseSamplesCooked PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(RenderVerseSamplesCooked PRIVATE
    RVX::Core
    RVX::HAL
    RVX::RHI
    RVX::RHI_BackendFactory
    RVX::SampleCookedComposition
    RVX::Gfx24SampleScenarios
    RVX::Gfx25SampleScenarios
)

# GFX17 scenarios compile against the narrow scenario contracts only. Their
# final symbols are resolved by the existing Sample host/SampleCommon runtime,
# but their target closure intentionally cannot pull Renderer, RHI, a backend,
# or Developer composition into scene code.
add_library(RVX_Gfx17SampleScenarios STATIC
    Scenes/ParticleVfxSamples.cpp
)
target_compile_features(RVX_Gfx17SampleScenarios PRIVATE cxx_std_20)
target_include_directories(RVX_Gfx17SampleScenarios PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
rvx_apply_scenario_contract_include_roots(
    RVX_Gfx17SampleScenarios
    RVX_Animation
    RVX_AssetContracts
    RVX_Core
    RVX_ECS
    RVX_Geometry
    Particle
    RVX_Physics
    RVX_RenderContracts
    RVX_ResourceRuntime
    RVX_ResourceSceneAdapters
    RVX_Scene
    RVX_World
)
target_link_libraries(RVX_Gfx17SampleScenarios PRIVATE
    RVX::SampleScenarioContracts
    glm::glm-header-only
)
add_library(RVX::Gfx17SampleScenarios ALIAS RVX_Gfx17SampleScenarios)
rvx_register_module_target(
    TARGET RVX_Gfx17SampleScenarios
    MODULE Samples
    ALIASES RVX::Gfx17SampleScenarios
)

# GFX18 follows the narrow scenario boundary established for GFX17.  It may
# author public Water/ECS values and use the public gameplay evaluator, but it
# cannot pull renderer, RHI, backend, importer, or compiler implementation
# objects into scenario code.
add_library(RVX_Gfx18SampleScenarios STATIC
    Scenes/WaterSurfaceSample.cpp
)
target_compile_features(RVX_Gfx18SampleScenarios PRIVATE cxx_std_20)
target_include_directories(RVX_Gfx18SampleScenarios PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
rvx_apply_scenario_contract_include_roots(
    RVX_Gfx18SampleScenarios
    RVX_Animation
    RVX_AssetContracts
    RVX_Core
    RVX_ECS
    RVX_Geometry
    RVX_Physics
    RVX_RenderContracts
    RVX_ResourceRuntime
    RVX_ResourceSceneAdapters
    RVX_Scene
    RVX_Water
    RVX_World
)
target_link_libraries(RVX_Gfx18SampleScenarios PRIVATE
    RVX::SampleScenarioContracts
    RVX::Water
    glm::glm-header-only
)
add_library(RVX::Gfx18SampleScenarios ALIAS RVX_Gfx18SampleScenarios)
rvx_register_module_target(
    TARGET RVX_Gfx18SampleScenarios
    MODULE Samples
    ALIASES RVX::Gfx18SampleScenarios
)

# GFX24 is intentionally a composition-only scenario.  It consumes the
# public Sample/World, Particle, and Water contracts while keeping all render
# feature, graph, backend, importer, and compiler ownership in the host.
add_library(RVX_Gfx24SampleScenarios STATIC
    Scenes/RenderQualityLabSample.cpp
)
target_compile_features(RVX_Gfx24SampleScenarios PRIVATE cxx_std_20)
target_include_directories(RVX_Gfx24SampleScenarios PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
rvx_apply_scenario_contract_include_roots(
    RVX_Gfx24SampleScenarios
    RVX_Animation
    RVX_AnimationSceneAdapters
    RVX_AssetContracts
    RVX_Core
    RVX_ECS
    RVX_Engine
    RVX_Geometry
    RVX_HAL
    Particle
    RVX_Physics
    RVX_RenderContracts
    RVX_ResourceRuntime
    RVX_ResourceSceneAdapters
    RVX_Scene
    RVX_Water
    RVX_World
)
target_link_libraries(RVX_Gfx24SampleScenarios PRIVATE
    RVX::SampleScenarioContracts
    RVX::Water
    glm::glm-header-only
)
add_library(RVX::Gfx24SampleScenarios ALIAS RVX_Gfx24SampleScenarios)
rvx_register_module_target(
    TARGET RVX_Gfx24SampleScenarios
    MODULE Samples
    ALIASES RVX::Gfx24SampleScenarios
)

# GFX25 contains the model and PBR scenarios in the same narrow scenario
# boundary. They author public scene/resource contracts only; the executable
# host remains responsible for renderer/backend composition and cooked assets.
add_library(RVX_Gfx25SampleScenarios STATIC
    Scenes/ModelViewerSample.cpp
    Scenes/PBRMaterialsSample.cpp
)
target_compile_features(RVX_Gfx25SampleScenarios PRIVATE cxx_std_20)
target_include_directories(RVX_Gfx25SampleScenarios PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
rvx_apply_scenario_contract_include_roots(
    RVX_Gfx25SampleScenarios
    RVX_Animation
    RVX_AnimationSceneAdapters
    RVX_AssetContracts
    RVX_Core
    RVX_ECS
    RVX_Engine
    RVX_Geometry
    RVX_Physics
    RVX_RenderContracts
    RVX_ResourceRuntime
    RVX_ResourceSceneAdapters
    RVX_RuntimeServices
    RVX_Scene
    RVX_World
)
target_link_libraries(RVX_Gfx25SampleScenarios PRIVATE
    RVX::SampleScenarioContracts
    glm::glm-header-only
)
add_library(RVX::Gfx25SampleScenarios ALIAS RVX_Gfx25SampleScenarios)
rvx_register_module_target(
    TARGET RVX_Gfx25SampleScenarios
    MODULE Samples
    ALIASES RVX::Gfx25SampleScenarios
)

# Keep the GFX17 scene target honest even while the legacy SampleCommon host
# still has broader historical dependencies. This configure-time check walks
# its complete CMake target closure, including static-library LINK_ONLY
# dependencies, and rejects forbidden composition modules; the source scan
# independently rejects direct private renderer/backend includes.
function(rvx_gfx_sample_link_item_is_forbidden _rvx_link _rvx_out_result)
    set(_rvx_normalized "${_rvx_link}")
    if(_rvx_normalized MATCHES "^\\$<LINK_ONLY:(.+)>$")
        set(_rvx_normalized "${CMAKE_MATCH_1}")
    endif()
    set(_rvx_forbidden
        RVX::RHI RVX_RHI
        RVX::Render RVX_Render
        RVX::AssetImport RVX_AssetImport
        RVX::ShaderCompiler RVX_ShaderCompiler
        RVX::EngineDevelopmentComposition RVX_EngineDevelopmentComposition)
    list(FIND _rvx_forbidden "${_rvx_normalized}" _rvx_forbidden_index)
    set(${_rvx_out_result} FALSE PARENT_SCOPE)
    if(NOT _rvx_forbidden_index EQUAL -1)
        set(${_rvx_out_result} TRUE PARENT_SCOPE)
    endif()
endfunction()

function(rvx_assert_gfx_sample_scenario_closure _rvx_target)
    cmake_parse_arguments(RVX_GFX_SCENARIO "" ""
        "SOURCE_CONTRACT_FILES" ${ARGN})
    if(NOT RVX_GFX_SCENARIO_SOURCE_CONTRACT_FILES)
        message(FATAL_ERROR
            "GFX scenario closure check requires SOURCE_CONTRACT_FILES")
    endif()
    set(_rvx_pending "${_rvx_target}")
    set(_rvx_visited)
    set(_rvx_forbidden
        RVX::RHI RVX_RHI
        RVX::Render RVX_Render
        RVX::AssetImport RVX_AssetImport
        RVX::ShaderCompiler RVX_ShaderCompiler
        RVX::EngineDevelopmentComposition RVX_EngineDevelopmentComposition)
    while(_rvx_pending)
        list(POP_FRONT _rvx_pending _rvx_current)
        list(FIND _rvx_visited "${_rvx_current}" _rvx_seen)
        if(NOT _rvx_seen EQUAL -1)
            continue()
        endif()
        list(APPEND _rvx_visited "${_rvx_current}")
        foreach(_rvx_forbidden_target IN LISTS _rvx_forbidden)
            if("${_rvx_current}" STREQUAL "${_rvx_forbidden_target}")
                message(FATAL_ERROR
                    "GFX scenario closure reached forbidden target ${_rvx_current}")
            endif()
        endforeach()
        rvx_gfx_sample_link_item_is_forbidden(
            "${_rvx_current}" _rvx_current_is_forbidden)
        if(_rvx_current MATCHES "^\\$<LINK_ONLY:(.+)>$")
            set(_rvx_current "${CMAKE_MATCH_1}")
        endif()
        if(_rvx_current_is_forbidden)
            message(FATAL_ERROR
                "GFX scenario closure reached forbidden target ${_rvx_current}")
        endif()
        if(NOT TARGET "${_rvx_current}")
            continue()
        endif()
        foreach(_rvx_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_rvx_links "${_rvx_current}" ${_rvx_property})
            if(_rvx_links AND NOT _rvx_links STREQUAL "_rvx_links-NOTFOUND")
                foreach(_rvx_link IN LISTS _rvx_links)
                    if(_rvx_link MATCHES "^::@")
                        continue()
                    endif()
                    list(APPEND _rvx_pending "${_rvx_link}")
                endforeach()
            endif()
        endforeach()
    endwhile()

    foreach(_rvx_gfx_source_contract_file IN LISTS RVX_GFX_SCENARIO_SOURCE_CONTRACT_FILES)
        file(STRINGS "${_rvx_gfx_source_contract_file}"
             _rvx_gfx_private_includes
             REGEX "^[ \\t]*#include[ \\t]+\"(RHI/|Render/|RenderGraph|AssetImport|ShaderCompiler|EngineDevelopmentComposition)")
        if(_rvx_gfx_private_includes)
            message(FATAL_ERROR
                "GFX narrow scenario source contract includes forbidden renderer/developer headers in ${_rvx_gfx_source_contract_file}: ${_rvx_gfx_private_includes}")
        endif()
    endforeach()
endfunction()

# Product closure is intentionally stronger than the scenario rule above:
# the executable itself must have exactly the cooked composition in its full
# static-link closure.  Inspect both ordinary and LINK_ONLY values because
# static libraries otherwise allow an importer/compiler dependency to hide in
# a consumer's link implementation.
function(rvx_assert_sample_cooked_product_closure _rvx_target)
    if(NOT TARGET "${_rvx_target}")
        message(FATAL_ERROR
            "Cooked Sample closure check requires existing target ${_rvx_target}")
    endif()
    set(_rvx_pending "${_rvx_target}")
    set(_rvx_visited)
    set(_rvx_seen_cooked FALSE)
    set(_rvx_seen_development FALSE)
    set(_rvx_forbidden
        RVX::AssetImport RVX_AssetImport
        RVX::ShaderCompiler RVX_ShaderCompiler
        RVX::EngineDevelopmentComposition RVX_EngineDevelopmentComposition
        RVX::SampleDevelopmentComposition RVX_SampleDevelopmentComposition)
    while(_rvx_pending)
        list(POP_FRONT _rvx_pending _rvx_current)
        if(_rvx_current MATCHES "^\\$<LINK_ONLY:(.+)>$")
            set(_rvx_current "${CMAKE_MATCH_1}")
        endif()
        list(FIND _rvx_visited "${_rvx_current}" _rvx_seen_index)
        if(NOT _rvx_seen_index EQUAL -1)
            continue()
        endif()
        list(APPEND _rvx_visited "${_rvx_current}")
        if("${_rvx_current}" STREQUAL "RVX::SampleCookedComposition" OR
           "${_rvx_current}" STREQUAL "RVX_SampleCookedComposition")
            set(_rvx_seen_cooked TRUE)
        endif()
        if("${_rvx_current}" STREQUAL "RVX::SampleDevelopmentComposition" OR
           "${_rvx_current}" STREQUAL "RVX_SampleDevelopmentComposition")
            set(_rvx_seen_development TRUE)
        endif()
        list(FIND _rvx_forbidden "${_rvx_current}" _rvx_forbidden_index)
        if(NOT _rvx_forbidden_index EQUAL -1)
            message(FATAL_ERROR
                "Cooked Sample product closure reached forbidden target ${_rvx_current}")
        endif()
        if(NOT TARGET "${_rvx_current}")
            continue()
        endif()
        foreach(_rvx_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_rvx_links "${_rvx_current}" ${_rvx_property})
            if(_rvx_links AND NOT _rvx_links STREQUAL "_rvx_links-NOTFOUND")
                foreach(_rvx_link IN LISTS _rvx_links)
                    if(NOT _rvx_link MATCHES "^::@")
                        list(APPEND _rvx_pending "${_rvx_link}")
                    endif()
                endforeach()
            endif()
        endforeach()
    endwhile()
    if(NOT _rvx_seen_cooked)
        message(FATAL_ERROR
            "Cooked Sample product closure must reach RVX::SampleCookedComposition")
    endif()
    if(_rvx_seen_development)
        message(FATAL_ERROR
            "Cooked Sample product closure reaches both Sample compositions")
    endif()
endfunction()

# Regression proof for the static closure checker itself. A future attempt to
# hide RHI behind LINK_ONLY must remain detectable rather than weakening this
# check to a consumer-interface-only inspection.
rvx_gfx_sample_link_item_is_forbidden(
    "$<LINK_ONLY:RVX::RHI>" _rvx_gfx17_link_only_rhi_rejected)
if(NOT _rvx_gfx17_link_only_rhi_rejected)
    message(FATAL_ERROR
        "GFX17 closure check does not reject LINK_ONLY RHI dependencies")
endif()
rvx_assert_sample_cooked_product_closure(RenderVerseSamplesCooked)
rvx_assert_gfx_sample_scenario_closure(
    RVX_Gfx17SampleScenarios
    SOURCE_CONTRACT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/ParticleVfxSamples.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/ParticleVfxSamples.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleContext.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleFeatureReporter.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleParticleEffectLoader.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleParticleVfxDiagnostics.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleRenderPathPolicy.h")
rvx_assert_gfx_sample_scenario_closure(
    RVX_Gfx18SampleScenarios
    SOURCE_CONTRACT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/WaterSurfaceSample.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/WaterSurfaceSample.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleContext.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleFeatureReporter.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleModelLoader.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleWaterSurfaceDiagnostics.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleRenderPathPolicy.h")
rvx_assert_gfx_sample_scenario_closure(
    RVX_Gfx24SampleScenarios
    SOURCE_CONTRACT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/RenderQualityLabSample.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/RenderQualityLabSample.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleContext.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleFeatureReporter.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleEnvironmentLoader.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleModelLoader.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleParticleEffectLoader.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleRenderQualityDiagnostics.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleRenderPathPolicy.h")
rvx_assert_gfx_sample_scenario_closure(
    RVX_Gfx25SampleScenarios
    SOURCE_CONTRACT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/ModelViewerSample.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/ModelViewerSample.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/PBRMaterialsSample.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Scenes/PBRMaterialsSample.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleContext.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleFeatureReporter.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleEnvironmentLoader.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleModelLoader.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common/Include/Samples/SampleRenderPathPolicy.h")

if(RVX_ENABLE_DX11)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_DX11)
    target_link_libraries(RenderVerseSamplesCooked PRIVATE RVX::RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_DX12)
    target_link_libraries(RenderVerseSamplesCooked PRIVATE RVX::RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_Vulkan)
    target_link_libraries(RenderVerseSamplesCooked PRIVATE RVX::RHI_Vulkan)
endif()
if(RVX_ENABLE_METAL)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_Metal)
    target_link_libraries(RenderVerseSamplesCooked PRIVATE RVX::RHI_Metal)
endif()
if(RVX_ENABLE_OPENGL)
    target_link_libraries(RenderVerseSamples PRIVATE RVX::RHI_OpenGL)
    target_link_libraries(RenderVerseSamplesCooked PRIVATE RVX::RHI_OpenGL)
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
        "-DPARTICLE_VFX_FIXTURE_WRITER=$<TARGET_FILE:RVXSampleParticleVfxFixtureWriter>"
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
        RVXSampleParticleVfxFixtureWriter
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

# The normal Sample staging tree contains source assets for development
# scenarios.  The Cooked product receives a separate nine-package subset so
# its final runtime directory has no authoring model, image, HDR, or shader
# source fallback to accidentally mount.
set(RVX_SAMPLE_COOKED_PRODUCT_ASSET_DESTINATION
    "${RVX_SAMPLE_RUNTIME_DIRECTORY}/Assets/RenderQualityLabCooked")
set(RVX_SAMPLE_COOKED_PRODUCT_RECEIPT
    "${RVX_SAMPLE_COOKED_PRODUCT_ASSET_DESTINATION}/.rvx-staged")
add_custom_command(
    OUTPUT "${RVX_SAMPLE_COOKED_PRODUCT_RECEIPT}"
    COMMAND ${CMAKE_COMMAND}
        "-DFULL_ASSET_ROOT=${RVX_SAMPLE_ASSET_DESTINATION}"
        "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
        "-DDESTINATION_ROOT=${RVX_SAMPLE_COOKED_PRODUCT_ASSET_DESTINATION}"
        "-DSTAMP_PATH=${RVX_SAMPLE_COOKED_PRODUCT_RECEIPT}"
        "-DCATALOG_WRITER=$<TARGET_FILE:RVXSampleCookedProductCatalogWriter>"
        "-DMANIFEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Manifests/renderverse-samples.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/StageRenderQualityLabCookedAssets.cmake"
    DEPENDS
        RVXSampleAssets
        RVXSampleCookedProductCatalogWriter
        "${CMAKE_CURRENT_SOURCE_DIR}/CMake/StageRenderQualityLabCookedAssets.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/Manifests/renderverse-samples.json"
    COMMENT "Staging strict cooked-only RenderQualityLab product assets"
    VERBATIM
)
add_custom_target(RVXRenderQualityLabCookedAssets
    DEPENDS "${RVX_SAMPLE_COOKED_PRODUCT_RECEIPT}")
add_dependencies(RenderVerseSamplesCooked RVXRenderQualityLabCookedAssets)
set_target_properties(RVXRenderQualityLabCookedAssets PROPERTIES
    FOLDER "Samples/Assets")
rvx_register_module_target(
    TARGET RVXRenderQualityLabCookedAssets
    MODULE Samples
)

set_target_properties(RenderVerseSamples PROPERTIES
    FOLDER "Samples/Product"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
)

set_target_properties(RenderVerseSamplesCooked PROPERTIES
    FOLDER "Samples/Product"
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamplesCooked>"
)

rvx_register_module_target(
    TARGET RenderVerseSamples
    MODULE Samples
)
rvx_register_module_target(
    TARGET RenderVerseSamplesCooked
    MODULE Samples
)
