# =============================================================================
# Tests - Validation test suites
# =============================================================================

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

option(RVX_ENABLE_EXTERNAL_ASSET_TESTS
    "Register opt-in rendering tests backed by a developer or CI asset library"
    OFF)
option(RVX_ENABLE_EXTERNAL_ENVIRONMENT_TESTS
    "Register opt-in PBR/IBL tests backed by cataloged external environments"
    OFF)
option(RVX_ENABLE_LONG_RUNNING_QUALIFICATION_TESTS
    "Register 10,000-frame / five-minute sustained rendering qualifications"
    OFF)

# The P0 delivery gate is a Windows DX12/Vulkan qualification matrix.  Keep
# normal non-Windows configurations explicit rather than quietly registering a
# partial backend subset.  Consumers can opt out locally, but an enabled gate
# is never permitted to degrade into a skip-pass.
set(RVX_P0_QUALIFICATION_MATRIX_DEFAULT OFF)
if(WIN32 AND RVX_BUILD_SAMPLES AND RVX_BUILD_TESTS)
    set(RVX_P0_QUALIFICATION_MATRIX_DEFAULT ON)
endif()
option(RVX_ENABLE_P0_QUALIFICATION_MATRIX
    "Register the strict DX12/Vulkan P0 sample qualification matrix"
    ${RVX_P0_QUALIFICATION_MATRIX_DEFAULT})
if(RVX_ENABLE_P0_QUALIFICATION_MATRIX)
    if(NOT WIN32)
        message(FATAL_ERROR
            "RVX_ENABLE_P0_QUALIFICATION_MATRIX requires Windows DX12 and Vulkan backends")
    endif()
    if(NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN)
        message(FATAL_ERROR
            "RVX_ENABLE_P0_QUALIFICATION_MATRIX requires both RVX_ENABLE_DX12 and RVX_ENABLE_VULKAN")
    endif()
    if(NOT TARGET RenderVerseSamples)
        message(FATAL_ERROR
            "RVX_ENABLE_P0_QUALIFICATION_MATRIX requires RVX_BUILD_SAMPLES with RenderVerseSamples")
    endif()
elseif(NOT WIN32)
    message(STATUS
        "P0 qualification matrix: Unavailable (requires Windows DX12 and Vulkan backends)")
endif()
option(RVX_ENABLE_P1_QUALIFICATION_MATRIX
    "Register the strict DX12/Vulkan P1 physics sample qualification matrix"
    ${RVX_ENABLE_P0_QUALIFICATION_MATRIX})
if(RVX_ENABLE_P1_QUALIFICATION_MATRIX)
    if(NOT WIN32)
        message(FATAL_ERROR
            "RVX_ENABLE_P1_QUALIFICATION_MATRIX requires Windows DX12 and Vulkan backends")
    endif()
    if(NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN)
        message(FATAL_ERROR
            "RVX_ENABLE_P1_QUALIFICATION_MATRIX requires both RVX_ENABLE_DX12 and RVX_ENABLE_VULKAN")
    endif()
    if(NOT TARGET RenderVerseSamples)
        message(FATAL_ERROR
            "RVX_ENABLE_P1_QUALIFICATION_MATRIX requires RVX_BUILD_SAMPLES with RenderVerseSamples")
    endif()
elseif(NOT WIN32)
    message(STATUS
        "P1 qualification matrix: Unavailable (requires Windows DX12 and Vulkan backends)")
endif()
set(RVX_EXTERNAL_TEST_ASSET_ROOT "" CACHE PATH
    "Asset library root used by enabled external asset test suites")

# Collect all enabled backend libraries
set(RVX_ALL_BACKENDS "")
if(RVX_ENABLE_DX11)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_Vulkan)
endif()
if(RVX_ENABLE_METAL)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_Metal)
endif()
if(RVX_ENABLE_OPENGL)
    list(APPEND RVX_ALL_BACKENDS RVX::RHI_OpenGL)
endif()

# Shared test utilities
add_library(RVX_TestCommon STATIC
    Common/ImageCompare.cpp
    Common/ImageFile.cpp
)

target_include_directories(RVX_TestCommon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(RVX_TestCommon PUBLIC
    RVX::Core
)

add_library(RVX_RenderTestSupport STATIC
    Common/RenderRuntimeTestSupport.cpp
)

target_include_directories(RVX_RenderTestSupport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(RVX_RenderTestSupport PRIVATE
    ${CMAKE_SOURCE_DIR}/Render/Private
)

target_link_libraries(RVX_RenderTestSupport PUBLIC
    RVX::RHI
    RVX_RenderRuntimeCore
)

function(rvx_add_gtest)
    set(options "")
    set(oneValueArgs NAME TIMEOUT LABELS WORKING_DIRECTORY)
    set(multiValueArgs SOURCES LIBS INCLUDES DEFINES)
    cmake_parse_arguments(RVX_GTEST
        "${options}"
        "${oneValueArgs}"
        "${multiValueArgs}"
        ${ARGN}
    )

    if(NOT RVX_GTEST_NAME)
        message(FATAL_ERROR "rvx_add_gtest requires NAME")
    endif()

    if(NOT RVX_GTEST_SOURCES)
        message(FATAL_ERROR "rvx_add_gtest(${RVX_GTEST_NAME}) requires SOURCES")
    endif()

    if(NOT RVX_GTEST_TIMEOUT)
        set(RVX_GTEST_TIMEOUT 120)
    endif()

    add_executable(${RVX_GTEST_NAME}
        ${RVX_GTEST_SOURCES}
    )
    set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
        ${RVX_GTEST_NAME}
    )
    target_link_libraries(${RVX_GTEST_NAME} PRIVATE
        GTest::gtest_main
        ${RVX_GTEST_LIBS}
    )
    target_compile_features(${RVX_GTEST_NAME} PRIVATE cxx_std_20)

    if(RVX_GTEST_INCLUDES)
        target_include_directories(${RVX_GTEST_NAME} PRIVATE
            ${RVX_GTEST_INCLUDES}
        )
    endif()

    if(RVX_GTEST_DEFINES)
        target_compile_definitions(${RVX_GTEST_NAME} PRIVATE
            ${RVX_GTEST_DEFINES}
        )
    endif()

    if(RVX_GTEST_WORKING_DIRECTORY)
        gtest_discover_tests(${RVX_GTEST_NAME}
            WORKING_DIRECTORY "${RVX_GTEST_WORKING_DIRECTORY}"
            DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "${RVX_GTEST_LABELS}"
                TIMEOUT ${RVX_GTEST_TIMEOUT}
        )
    else()
        gtest_discover_tests(${RVX_GTEST_NAME}
            DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 30
            PROPERTIES
                LABELS "${RVX_GTEST_LABELS}"
                TIMEOUT ${RVX_GTEST_TIMEOUT}
        )
    endif()
endfunction()

# Pure Runtime ECS kernel invariants. These remain small deterministic gates;
# product integration is qualified separately through RenderVerseSamples.
add_subdirectory(EcsParticleFeatureBridgeValidation)
add_subdirectory(EcsWaterFeatureBridgeValidation)
add_subdirectory(EcsTerrainFeatureBridgeValidation)
add_subdirectory(ParticleEcsRuntimeGatewayValidation)
add_subdirectory(WaterEcsRuntimeGatewayValidation)
add_subdirectory(TerrainEcsRuntimeGatewayValidation)

rvx_add_gtest(
    NAME EcsEntityValidation
    SOURCES EcsEntityValidation/main.cpp
    LIBS RVX::ECS
    LABELS "unit;ecs;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsFragmentQueryValidation
    SOURCES EcsFragmentQueryValidation/main.cpp
    LIBS RVX::ECS
    LABELS "unit;ecs;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsCommandTransactionValidation
    SOURCES EcsCommandTransactionValidation/main.cpp
    LIBS RVX::ECS
    LABELS "unit;ecs;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSchedulerValidation
    SOURCES EcsSchedulerValidation/main.cpp
    LIBS RVX::ECS
    LABELS "unit;ecs;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsTransformHierarchyValidation
    SOURCES EcsTransformHierarchyValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsCleanupValidation
    SOURCES EcsCleanupValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsChangeTrackingValidation
    SOURCES EcsChangeTrackingValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSpatialIndexValidation
    SOURCES EcsSpatialIndexValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;ecs;spatial"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsPhysicsValidation
    SOURCES EcsPhysicsValidation/main.cpp
    LIBS RVX::PhysicsSceneAdapters
    LABELS "unit;scene;ecs;physics"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsAnimationValidation
    SOURCES EcsAnimationValidation/main.cpp
    LIBS RVX::AnimationSceneAdapters
    LABELS "unit;scene;ecs;animation"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsResourceAnimationEvaluatorValidation
    SOURCES EcsResourceAnimationEvaluatorValidation/main.cpp
    LIBS RVX::AnimationSceneAdapters
    LABELS "unit;resource;scene;ecs;animation"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsAnimationAssetServiceValidation
    SOURCES EcsAnimationAssetServiceValidation/main.cpp
    LIBS
        RVX::AnimationSceneAdapters
        RVX::Audio
        RVX::Engine
        RVX::PhysicsSceneAdapters
        RVX::ResourceSceneAdapters
        RVX::Scripting
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    LABELS "unit;resource;scene;ecs;animation"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsAudioValidation
    SOURCES EcsAudioValidation/main.cpp
    LIBS RVX::Audio
    LABELS "unit;scene;ecs;audio"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsScriptingValidation
    SOURCES EcsScriptingValidation/main.cpp
    LIBS RVX::Scripting
    LABELS "unit;scripting;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneSnapshotValidation
    SOURCES EcsSceneSnapshotValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSkinningSnapshotValidation
    SOURCES EcsSkinningSnapshotValidation/main.cpp
    LIBS
        RVX::AnimationSceneAdapters
        RVX::RenderExtraction
    LABELS "unit;render;scene;ecs;animation;skinning"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneOrchestrationValidation
    SOURCES EcsSceneOrchestrationValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneSpawnTransactionValidation
    SOURCES EcsSceneSpawnTransactionValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsCameraServiceValidation
    SOURCES EcsCameraServiceValidation/main.cpp
    LIBS RVX::World
    LABELS "unit;world;scene;ecs;camera"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME WorldEcsAuthorityValidation
    SOURCES WorldEcsAuthorityValidation/main.cpp
    LIBS RVX::World
    LABELS "unit;world;scene;ecs;authority"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EngineEcsWorldOwnershipValidation
    SOURCES EngineEcsWorldOwnershipValidation/main.cpp
    LIBS RVX::Engine
    LABELS "unit;engine;world;scene;ecs;authority"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsFrozenSceneBridgeValidation
    SOURCES EcsFrozenSceneBridgeValidation/main.cpp
    LIBS RVX::RenderExtraction
    LABELS "unit;render;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsFrameExtractionValidation
    SOURCES EcsFrameExtractionValidation/main.cpp
    LIBS RVX::RenderExtraction
    LABELS "unit;render;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsFeatureSnapshotValidation
    SOURCES EcsFeatureSnapshotValidation/main.cpp
    LIBS RVX::RenderExtraction
    LABELS "unit;render;scene;ecs;feature"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneRenderProofGatewayValidation
    SOURCES EcsSceneRenderProofGatewayValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::RenderExtraction
        RVX::ResourceSceneAdapters
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    LABELS "unit;engine;render;resource;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsRenderFramePipelineValidation
    SOURCES EcsRenderFramePipelineValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::RenderExtraction
        RVX::ResourceSceneAdapters
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    LABELS "unit;engine;render;resource;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsRenderRuntimeCompositionValidation
    SOURCES EcsRenderRuntimeCompositionValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::RenderExtraction
        RVX::ResourceSceneAdapters
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    LABELS "unit;engine;render;resource;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsWorldRuntimeCompositionValidation
    SOURCES EcsWorldRuntimeCompositionValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::AnimationSceneAdapters
        RVX::Audio
        RVX::PhysicsSceneAdapters
        RVX::ResourceSceneAdapters
        RVX::Scripting
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    LABELS "unit;engine;world;render;resource;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsRenderEntityRetirementCoordinatorValidation
    SOURCES EcsRenderEntityRetirementCoordinatorValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::ResourceSceneAdapters
        RVX::Scene
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    LABELS "unit;engine;render;resource;scene;ecs;retirement"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsModelBatchValidation
    SOURCES EcsModelBatchValidation/main.cpp
    LIBS RVX::ResourceSceneAdapters
    LABELS "unit;resource;scene;ecs"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneAssetLoadCoordinatorValidation
    SOURCES EcsSceneAssetLoadCoordinatorValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::Scene
    LABELS "unit;resource;scene;ecs"
    TIMEOUT 30
)

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME EcsSampleModelLoaderValidation
        SOURCES EcsSampleModelLoaderValidation/main.cpp
        LIBS
            RVX::SampleCommon
            RVX::Scene
        LABELS "unit;samples;resource;scene;ecs"
        TIMEOUT 30
    )
endif()

rvx_add_gtest(
    NAME EcsEnvironmentLoadCoordinatorValidation
    SOURCES EcsEnvironmentLoadCoordinatorValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::Scene
    LABELS "unit;resource;scene;ecs;environment"
    TIMEOUT 30
)

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME EcsSampleEnvironmentLoaderValidation
        SOURCES EcsSampleEnvironmentLoaderValidation/main.cpp
        LIBS
            RVX::Core
            RVX::SampleCommon
        LABELS "unit;samples;resource;scene;ecs;environment"
        TIMEOUT 30
    )
endif()

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME EcsSampleAnimationLoaderValidation
        SOURCES EcsSampleAnimationLoaderValidation/main.cpp
        LIBS
            RVX::Core
            RVX::SampleCommon
        LABELS "unit;samples;resource;scene;ecs;animation"
        TIMEOUT 30
    )
endif()

rvx_add_gtest(
    NAME EcsDynamicBufferValidation
    SOURCES EcsDynamicBufferValidation/main.cpp
    LIBS RVX::ECS
    LABELS "unit;ecs;storage"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSharedDataValidation
    SOURCES EcsSharedDataValidation/main.cpp
    LIBS RVX::ECS
    LABELS "unit;ecs;storage"
    TIMEOUT 30
)

add_executable(RHIBackendFactoryLinkClosureValidation
    RHIBackendFactoryLinkClosureValidation/main.cpp
)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    RHIBackendFactoryLinkClosureValidation
)
target_link_libraries(RHIBackendFactoryLinkClosureValidation PRIVATE
    RVX::RHI_BackendFactory
)
target_compile_features(RHIBackendFactoryLinkClosureValidation PRIVATE cxx_std_20)
add_test(
    NAME Architecture.RHIBackendFactoryLinkClosure
    COMMAND RHIBackendFactoryLinkClosureValidation
)
set_tests_properties(Architecture.RHIBackendFactoryLinkClosure PROPERTIES
    LABELS "unit;architecture"
    TIMEOUT 30
)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
    NAME Architecture.GateInputsFailClosed
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Scripts/test_architecture_gate_inputs.py
        -v
)
set_tests_properties(Architecture.GateInputsFailClosed PROPERTIES
    LABELS "unit;architecture"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_test(
    NAME Architecture.RenderContractFieldChecker
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Scripts/test_render_contract_field_checker.py
        -v
)
set_tests_properties(Architecture.RenderContractFieldChecker PROPERTIES
    LABELS "unit;architecture"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_test(
    NAME Architecture.ModuleBoundaryManifestChecker
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Scripts/test_module_boundary_manifest.py
        -v
)
set_tests_properties(Architecture.ModuleBoundaryManifestChecker PROPERTIES
    LABELS "unit;architecture"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_test(
    NAME Architecture.RHIOwnershipInventoryChecker
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Scripts/test_rhi_ownership_inventory_checker.py
        -v
)
set_tests_properties(Architecture.RHIOwnershipInventoryChecker PROPERTIES
    LABELS "unit;architecture"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_test(
    NAME Architecture.RHIOwnershipInventory
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/Scripts/check_rhi_ownership_inventory.py
        --root ${CMAKE_SOURCE_DIR}
        --phase final
)
set_tests_properties(Architecture.RHIOwnershipInventory PROPERTIES
    LABELS "unit;architecture"
    TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

    add_test(
        NAME Architecture.ModuleBoundaries
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_module_boundaries.py
            --root ${CMAKE_SOURCE_DIR}
            --config ${CMAKE_SOURCE_DIR}/Docs/module-boundaries.json
    )
    set_tests_properties(Architecture.ModuleBoundaries PROPERTIES
        LABELS "unit;architecture"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME Architecture.ModuleBoundaryManifest
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_module_boundary_manifest.py
            --root ${CMAKE_SOURCE_DIR}
            --config ${CMAKE_SOURCE_DIR}/Docs/module-boundaries.json
    )
    set_tests_properties(Architecture.ModuleBoundaryManifest PROPERTIES
        LABELS "unit;architecture"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME Architecture.CMakeModuleVisibility
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_cmake_module_visibility.py
            --root ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(Architecture.CMakeModuleVisibility PROPERTIES
        LABELS "unit;architecture"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME Architecture.CMakeModuleIncludeEdges
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_cmake_module_include_edges.py
            --root ${CMAKE_SOURCE_DIR}
            --config ${CMAKE_SOURCE_DIR}/Docs/module-boundaries.json
    )
    set_tests_properties(Architecture.CMakeModuleIncludeEdges PROPERTIES
        LABELS "unit;architecture"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME Architecture.CMakeModuleLinks
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_cmake_module_links.py
            --root ${CMAKE_SOURCE_DIR}
            --config ${CMAKE_SOURCE_DIR}/Docs/module-boundaries.json
    )
    set_tests_properties(Architecture.CMakeModuleLinks PROPERTIES
        LABELS "unit;architecture"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME Architecture.PublicHeaderLinkage
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_public_header_linkage.py
            --root ${CMAKE_SOURCE_DIR}
            --config ${CMAKE_SOURCE_DIR}/Docs/module-boundaries.json
    )
    set_tests_properties(Architecture.PublicHeaderLinkage PROPERTIES
        LABELS "unit;architecture"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME Architecture.EditorRuntimeBoundary
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_editor_runtime_boundary.py
            --root ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(Architecture.EditorRuntimeBoundary PROPERTIES
        LABELS "unit;architecture"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME Architecture.PureEcsRuntime
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_pure_ecs_runtime.py
            --root ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(Architecture.PureEcsRuntime PROPERTIES
        LABELS "unit;architecture;ecs"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

# Core debug configuration validation tests
rvx_add_gtest(
    NAME CoreDebugConfigValidation
    SOURCES CoreDebugConfigValidation/main.cpp
    LIBS RVX::Core
    DEFINES $<$<CONFIG:Debug>:RVX_EXPECT_DEBUG_CONFIG=1>
    LABELS unit
    TIMEOUT 30
)

rvx_add_gtest(
    NAME CorePathValidation
    SOURCES CorePathValidation/main.cpp
    LIBS RVX::Core
    LABELS unit
    TIMEOUT 30
)

rvx_add_gtest(
    NAME StartupTimelineValidation
    SOURCES StartupTimelineValidation/main.cpp
    LIBS RVX::Core
    LABELS "unit;diagnostics;startup"
    TIMEOUT 30
)

# Keep ECS/runtime validation independently buildable while the product samples are
# physically migrated during the single-authority cutover.
if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME ModelCameraFramingValidation
        SOURCES ModelCameraFramingValidation/main.cpp
        LIBS RVX::SampleCommon
        LABELS "unit;samples;assets"
        TIMEOUT 30
    )
endif()

rvx_add_gtest(
    NAME RenderContractsValidation
    SOURCES RenderContractsValidation/main.cpp
    LIBS RVX::Core RVX::RenderContracts RVX::RenderExtraction
    INCLUDES ${CMAKE_SOURCE_DIR}/RenderContracts/Private
    LABELS "unit;architecture"
    TIMEOUT 60
)

rvx_add_gtest(
    NAME RenderPolicyValidation
    SOURCES RenderPolicyValidation/main.cpp
    LIBS RVX::Core RVX::RenderContracts RVX::RHI RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Render/Include
    LABELS "unit;architecture;render-policy"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderVisibilityValidation
    SOURCES RenderVisibilityValidation/main.cpp
    LIBS RVX::Core RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Render/Include
    LABELS "unit;architecture;visibility"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME GPUSceneValidation
    SOURCES GPUSceneValidation/main.cpp
    LIBS RVX::Render RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
    LABELS "unit;architecture;gpu-scene"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME GPUSceneSchemaValidation
    SOURCES GPUSceneSchemaValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Render/Include
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS "unit;architecture;gpu-scene"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME GPUSceneUploadValidation
    SOURCES GPUSceneUploadValidation/main.cpp
    LIBS RVX::Render RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
    LABELS "unit;architecture;gpu-scene"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderDrawPacketValidation
    SOURCES RenderDrawPacketValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Render/Include
    LABELS "unit;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderDrawPacketCacheValidation
    SOURCES RenderDrawPacketCacheValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Render/Include
    LABELS "unit;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderConcurrencyValidation
    SOURCES RenderConcurrencyValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX_RenderRuntimeCore
    INCLUDES ${CMAKE_SOURCE_DIR}/Render/Private
    LABELS "unit;architecture"
    TIMEOUT 60
)

if(RVX_ENABLE_TSAN)
    rvx_add_gtest(
        NAME RenderConcurrencyTSAN
        SOURCES RenderConcurrencyTSAN/main.cpp
        LIBS
            RVX::Core
            RVX::RenderContracts
            RVX_RenderRuntimeCore
        INCLUDES ${CMAKE_SOURCE_DIR}/Render/Private
        LABELS "tsan;architecture"
        TIMEOUT 180
    )
endif()

rvx_add_gtest(
    NAME RenderExecutorValidation
    SOURCES RenderExecutorValidation/main.cpp
    LIBS
        RVX_RenderTestSupport
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Render/Private
    LABELS "unit;architecture"
    TIMEOUT 60
)

rvx_add_gtest(
    NAME RenderThreadRuntimeValidation
    SOURCES RenderThreadRuntimeValidation/main.cpp
    LIBS
        RVX::Render
        RVX::RenderExtraction
        RVX_RenderTestSupport
        RVX_RenderRuntimeCore
        RVX::RHI_BackendFactory
        ${RVX_ALL_BACKENDS}
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Render/Private
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS "unit;architecture"
    TIMEOUT 60
)

rvx_add_gtest(
    NAME JobGraphValidation
    SOURCES JobGraphValidation/main.cpp
    LIBS RVX::Core
    LABELS unit
    TIMEOUT 30
)

# RHI public contract validation tests
rvx_add_gtest(
    NAME RHIContractValidation
    SOURCES RHIContractValidation/main.cpp
    LIBS RVX::RHI
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS unit
    TIMEOUT 30
)

rvx_add_gtest(
    NAME MeshPassProcessorValidation
    SOURCES MeshPassProcessorValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Render/Include
    LABELS "unit;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderInstanceBatchPlanValidation
    SOURCES RenderInstanceBatchPlanValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Render/Include
    LABELS "unit;architecture"
    TIMEOUT 60
)

rvx_add_gtest(
    NAME RHIConformanceValidation
    SOURCES RHIConformanceValidation/main.cpp
    LIBS RVX::RHI
    LABELS unit
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RHIPipelineValidation
    SOURCES RHIPipelineValidation/main.cpp
    LIBS RVX::RHI
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS unit
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderSubmissionValidation
    SOURCES RenderSubmissionValidation/main.cpp
    LIBS
        RVX::Render
        RVX::RHI
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Private
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS "unit;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderLifetimeCutoverValidation
    SOURCES RenderLifetimeCutoverValidation/main.cpp
    LIBS
        RVX::Render
        RVX::RHI
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Private
    LABELS "unit;architecture"
    TIMEOUT 30
)

# Samples CLI/report contract validation tests
if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME SampleCLIValidation
        SOURCES SampleCLIValidation/main.cpp
        LIBS RVX::SampleCommon
        LABELS unit
        TIMEOUT 30
    )
    rvx_add_gtest(
        NAME SampleRenderPathPolicyValidation
        SOURCES SampleRenderPathPolicyValidation/main.cpp
        LIBS RVX::SampleCommon
        LABELS "unit;samples;render-policy"
        TIMEOUT 30
    )
    rvx_add_gtest(
        NAME SampleRegistryValidation
        SOURCES SampleRegistryValidation/main.cpp
        LIBS RVX::SampleCommon
        LABELS "unit;samples"
        TIMEOUT 30
    )
    rvx_add_gtest(
        NAME SampleAssetCatalogValidation
        SOURCES SampleAssetCatalogValidation/main.cpp
        LIBS RVX::SampleCommon
        LABELS "unit;samples;assets"
        TIMEOUT 30
    )
    target_compile_definitions(SampleAssetCatalogValidation PRIVATE
        RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
endif()

# Production callers must remain on immutable frame/control/diagnostic values.
rvx_add_gtest(
    NAME RenderCallerBoundaryValidation
    SOURCES RenderCallerBoundaryValidation/main.cpp
    LABELS "unit;architecture"
    TIMEOUT 30
)
target_compile_definitions(RenderCallerBoundaryValidation PRIVATE
    RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set(RVX_NATIVE_LIFECYCLE_BACKEND "")
set(RVX_NATIVE_LIFECYCLE_DEFINE "")
if(WIN32 AND RVX_ENABLE_DX12)
    set(RVX_NATIVE_LIFECYCLE_BACKEND "DX12")
    set(RVX_NATIVE_LIFECYCLE_DEFINE RVX_NATIVE_BACKEND_DX12=1)
elseif(APPLE AND RVX_ENABLE_METAL)
    set(RVX_NATIVE_LIFECYCLE_BACKEND "Metal")
    set(RVX_NATIVE_LIFECYCLE_DEFINE RVX_NATIVE_BACKEND_METAL=1)
elseif(UNIX AND RVX_ENABLE_VULKAN)
    set(RVX_NATIVE_LIFECYCLE_BACKEND "Vulkan")
    set(RVX_NATIVE_LIFECYCLE_DEFINE RVX_NATIVE_BACKEND_VULKAN=1)
endif()

if(RVX_NATIVE_LIFECYCLE_BACKEND)
    rvx_add_gtest(
        NAME NativeRenderLifecycleValidation
        SOURCES NativeRenderLifecycleValidation/main.cpp
        LIBS
            RVX::HAL
            RVX::Render
            RVX::RenderExtraction
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/HAL/Private
        DEFINES
            ${RVX_NATIVE_LIFECYCLE_DEFINE}
            RVX_NATIVE_REQUIRED_BACKEND="${RVX_NATIVE_LIFECYCLE_BACKEND}"
        LABELS "native;gpu;m1"
        TIMEOUT 180
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()

# DX12 Validation Tests
if(RVX_ENABLE_DX12)
    rvx_add_gtest(
        NAME DX12Validation
        SOURCES DX12Validation/main.cpp
        LIBS
            RVX::Core
            RVX::Render
            RVX::RenderGraph
            RVX::RHI
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/RHI_DX12/Private
            ${CMAKE_SOURCE_DIR}/Render/Private
        LABELS gpu
        TIMEOUT 120
    )
endif()

# Vulkan Validation Tests
if(RVX_ENABLE_VULKAN)
    rvx_add_gtest(
        NAME VulkanValidation
        SOURCES VulkanValidation/main.cpp
        LIBS
            RVX::Core
            RVX::Render
            RVX::RenderGraph
            RVX::RHI
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/RHI_Vulkan/Private
        LABELS gpu
        TIMEOUT 120
    )
endif()

# DX11 Validation Tests
if(RVX_ENABLE_DX11)
    rvx_add_gtest(
        NAME DX11Validation
        SOURCES DX11Validation/main.cpp
        LIBS
            RVX::Core
            RVX::RenderGraph
            RVX::RHI
            RVX::ShaderCompiler
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/RHI_DX11/Private
        LABELS gpu
        TIMEOUT 120
    )
endif()

# RenderGraph Validation Tests
rvx_add_gtest(
    NAME RenderGraphValidation
    SOURCES
        RenderGraphValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraph.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraphCompiler.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraphExecutor.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraphExecution.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/TransientResourcePool.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
    LABELS unit
    TIMEOUT 120
)

# Render resource ownership and two-phase upload validation
rvx_add_gtest(
    NAME RenderResourceRuntimeValidation
    SOURCES RenderResourceRuntimeValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::RenderContracts
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/RenderContracts/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

# Image comparison utility validation tests
rvx_add_gtest(
    NAME ImageCompareValidation
    SOURCES ImageCompareValidation/main.cpp
    LIBS
        RVX_TestCommon
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    LABELS unit
    TIMEOUT 30
)

# HDR texture loader CPU IBL validation tests
rvx_add_gtest(
    NAME HDRTextureLoaderValidation
    SOURCES HDRTextureLoaderValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Resource/Include
    LABELS unit
    TIMEOUT 120
)

# Render honesty validation tests
rvx_add_gtest(
    NAME RenderHonestyValidation
    SOURCES RenderHonestyValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::Resource
        RVX::Tools
        RVX::RHI
        RVX::Terrain
        Particle
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Particle/Private
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Resource/Include
        ${CMAKE_SOURCE_DIR}/Terrain/Private
        ${CMAKE_SOURCE_DIR}/Tools/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

if(TARGET RVXCook)
    add_dependencies(RenderHonestyValidation RVXCook)
    target_compile_definitions(RenderHonestyValidation PRIVATE
        RVX_COOK_EXECUTABLE_PATH="$<TARGET_FILE:RVXCook>"
    )
endif()

# Shader compiler and reflection validation tests
rvx_add_gtest(
    NAME ShaderCompilerValidation
    SOURCES ShaderCompilerValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ShaderCompiler
        RVX::RHI
    INCLUDES
        ${CMAKE_SOURCE_DIR}/ShaderCompiler/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

# Pipeline cache and PSO validation tests
rvx_add_gtest(
    NAME PipelineCacheValidation
    SOURCES PipelineCacheValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::RHI
        RVX::ShaderCompiler
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Tests
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

# GPU Upload Service Validation Tests
rvx_add_gtest(
    NAME GPUUploadServiceValidation
    SOURCES
        GPUUploadServiceValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/GPUUploadService.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::RenderContracts
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/RenderContracts/Include
    LABELS unit
    TIMEOUT 120
)

# GPU-driven rendering validation tests
rvx_add_gtest(
    NAME GPUDrivenValidation
    SOURCES
        GPUDrivenValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/GPUDriven/GPUCulling.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Passes/MeshPassProcessor.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderDrawPacket.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Submission/RasterInstanceStream.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Submission/DirectRasterReadbackQualification.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Submission/RenderSubmissionStrategy.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Visibility/RenderVisibility.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::RenderContracts
        RVX::ShaderCompiler
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/RenderContracts/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
        ${CMAKE_SOURCE_DIR}/ShaderCompiler/Include
    LABELS unit
    TIMEOUT 120
)

# glTF skeleton/clip import and ModelResource publication validation.
rvx_add_gtest(
    NAME SkeletalAssetPipelineValidation
    SOURCES SkeletalAssetPipelineValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
        RVX::Tools
    LABELS "unit;resource;animation;gltf"
    TIMEOUT 120
)

rvx_add_gtest(
    NAME AnimationResourceValidation
    SOURCES AnimationResourceValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS "unit;resource;animation;cooked"
    TIMEOUT 120
)

rvx_add_gtest(
    NAME AnimationRootMotionDerivationValidation
    SOURCES AnimationRootMotionDerivationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
        RVX::Tools
    DEFINES
        RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS "unit;tools;resource;animation;root-motion"
    TIMEOUT 120
)

# Render Scene Validation Tests
rvx_add_gtest(
    NAME RenderSceneValidation
    SOURCES
        RenderSceneValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Material/MaterialClassification.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderDrawItem.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderDrawPacket.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderDrawPacketCache.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderScene.cpp
    LIBS
        RVX::Core
        RVX::Runtime
        RVX::World
        RVX::Resource
        RVX::RenderExtraction
        RVX::Scene
        RVX::Render
        RVX::Animation
        RVX::RHI
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/RenderExtraction/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer
        ${CMAKE_SOURCE_DIR}/Resource/Include
        ${CMAKE_SOURCE_DIR}/Scene/Include
        ${CMAKE_SOURCE_DIR}/World/Include
    LABELS unit
    TIMEOUT 120
)

# Render resource content replacement contract validation
rvx_add_gtest(
    NAME RenderResourceReplacementValidation
    SOURCES RenderResourceReplacementValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::RenderContracts
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/RenderContracts/Include
    LABELS "unit;render;resource"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderIncrementalSceneScaleValidation
    SOURCES RenderIncrementalSceneScaleValidation/main.cpp
    LIBS RVX::Render RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
    LABELS "unit;architecture;performance"
    TIMEOUT 180
)

rvx_add_gtest(
    NAME RenderSceneUpdateQueueValidation
    SOURCES RenderSceneUpdateQueueValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/RenderContracts/Include
    LABELS "unit;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderSceneRevisionValidation
    SOURCES RenderSceneRevisionValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX::Render
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/RenderContracts/Include
    LABELS "unit;architecture"
    TIMEOUT 30
)

# Material system validation tests
rvx_add_gtest(
    NAME MaterialSystemValidation
    SOURCES
        MaterialSystemValidation/main.cpp
        Common/RenderRuntimeTestHarness.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::Resource
        RVX::RHI
        RVX::Scene
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Tests
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/Resource/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

# Clustered lighting validation tests
rvx_add_gtest(
    NAME ClusteredLightingValidation
    SOURCES ClusteredLightingValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::RHI
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

# Render pass validation tests
rvx_add_gtest(
    NAME RenderPassValidation
    SOURCES
        RenderPassValidation/main.cpp
        Common/RenderRuntimeTestHarness.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::Resource
        RVX::RHI
        RVX::RHI_BackendFactory
        ${RVX_ALL_BACKENDS}
        RVX::Scene
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Tests
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/Resource/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
        ${CMAKE_SOURCE_DIR}/Scene/Include
    LABELS unit
    TIMEOUT 120
)

if(MSVC)
    # This validation intentionally exercises the complete pass matrix in one
    # translation unit and exceeds the legacy COFF section limit in Debug.
    target_compile_options(RenderPassValidation PRIVATE /bigobj)
endif()

# Editor context validation tests
if(TARGET RVX::Editor)
    rvx_add_gtest(
        NAME EditorContextValidation
        SOURCES EditorContextValidation/main.cpp
        LIBS
            RVX::Core
            RVX::Editor
            RVX::ResourceSceneAdapters
            RVX::Scene
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
        INCLUDES
            ${CMAKE_SOURCE_DIR}/Editor/Include
        LABELS "unit;editor"
        TIMEOUT 120
    )
    target_compile_options(EditorContextValidation PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
    )
endif()

# App mode and editor/runtime shared-core boundary validation tests
rvx_add_gtest(
    NAME AppModeBoundaryValidation
    SOURCES AppModeBoundaryValidation/main.cpp
    LIBS
        RVX::Core
    LABELS unit
    TIMEOUT 120
)

# Runtime UI command stream validation tests
rvx_add_gtest(
    NAME UIValidation
    SOURCES UIValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::UI
    LABELS unit
    TIMEOUT 120
)

# Audio resource honesty validation tests
rvx_add_gtest(
    NAME AudioResourceValidation
    SOURCES AudioResourceValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS unit
    TIMEOUT 120
)

if(TARGET RVX_SampleCommon)
rvx_add_gtest(
    NAME SampleWorldRequirementsValidation
    SOURCES SampleWorldRequirementsValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::SampleCommon
        RVX::World
    LABELS "unit;samples;world;architecture"
    TIMEOUT 60
)

rvx_add_gtest(
    NAME FrameworkAssessmentValidation
    SOURCES FrameworkAssessmentValidation/main.cpp
    LIBS RVX::SampleCommon
    LABELS "unit;samples;diagnostics;architecture"
    TIMEOUT 30
)

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME SampleSceneLifetimeScopeValidation
        SOURCES SampleSceneLifetimeScopeValidation/main.cpp
        LIBS RVX::SampleCommon
        LABELS "unit;sample;scene;lifetime"
        TIMEOUT 30
    )
endif()

rvx_add_gtest(
    NAME SceneLifecycleSampleValidation
    SOURCES
        SceneLifecycleSampleValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples/Scenes/SceneLifecycleSample.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::SampleCommon
        RVX::Scene
        RVX::World
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples
    LABELS "unit;samples;scene;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME PhysicsSandboxSampleValidation
    SOURCES
        PhysicsSandboxSampleValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples/Scenes/PhysicsSandboxSample.cpp
    LIBS
        RVX::Core
        RVX::Physics
        RVX::RenderContracts
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::SampleCommon
        RVX::Scene
        RVX::World
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples
    LABELS "unit;samples;physics;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME AnimationCharacterSampleValidation
    SOURCES
        AnimationCharacterSampleValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples/Scenes/AnimationCharacterSample.cpp
    LIBS
        RVX::Animation
        RVX::AnimationSceneAdapters
        RVX::Core
        RVX::Physics
        RVX::RenderContracts
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::SampleCommon
        RVX::Scene
        RVX::World
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples
    LABELS "unit;samples;animation;physics;architecture"
    TIMEOUT 60
)

rvx_add_gtest(
    NAME AssetStreamingSampleValidation
    SOURCES
        AssetStreamingSampleValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples/Scenes/AssetStreamingSample.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::SampleCommon
        RVX::Scene
        RVX::World
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS "unit;samples;resource;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME InteriorRenderingSampleValidation
    SOURCES
        InteriorRenderingSampleValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples/Scenes/InteriorRenderingSample.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::SampleCommon
        RVX::Scene
        RVX::World
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS "unit;samples;render;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderingStressSampleValidation
    SOURCES
        RenderingStressSampleValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples/Scenes/RenderingStressSample.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::SampleCommon
        RVX::Scene
        RVX::World
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Samples/RenderVerseSamples
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS "unit;samples;render;architecture;stress"
    TIMEOUT 30
)
endif()

# Resource runtime policy validation tests
rvx_add_gtest(
    NAME ResourceDiagnosticsViewValidation
    SOURCES ResourceDiagnosticsViewValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS "unit;resource;diagnostics"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME ResourceRuntimePolicyValidation
    SOURCES ResourceRuntimePolicyValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS unit
    TIMEOUT 120
)

# Coalesced resource load operation and subscriber lifecycle validation
rvx_add_gtest(
    NAME ResourceLoadOperationValidation
    SOURCES ResourceLoadOperationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS "unit;resource;async"
    TIMEOUT 30
)

# Atomic strong acquisition of already-published loaded resources
rvx_add_gtest(
    NAME ResourceLoadedAcquireValidation
    SOURCES ResourceLoadedAcquireValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS "unit;resource;concurrency"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME CookManifestValidation
    SOURCES CookManifestValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS "unit;resource;cook"
    TIMEOUT 30
)

# Prepared worker load and owner-thread transactional publish validation
rvx_add_gtest(
    NAME ResourcePreparedLoadingValidation
    SOURCES ResourcePreparedLoadingValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS "unit;resource;async"
    TIMEOUT 60
)

rvx_add_gtest(
    NAME ResourceRetirementLedgerValidation
    SOURCES ResourceRetirementLedgerValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS "unit;resource;retirement"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME MaterialInstanceResourceValidation
    SOURCES MaterialInstanceResourceValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
        RVX::Scene
    LABELS "unit;resource;material;architecture"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME SceneAssetStreamingValidation
    SOURCES SceneAssetStreamingValidation/main.cpp
    LIBS RVX::Core RVX::Resource
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS "unit;resource;streaming"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME CookedModelProductValidation
    SOURCES CookedModelProductValidation/main.cpp
    LIBS RVX::Core RVX::Resource RVX::Tools
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LABELS "unit;resource;cook;model"
    TIMEOUT 120
)

# Prepared HDR/IBL environment bundle and immutable admission validation
rvx_add_gtest(
    NAME EnvironmentPreparedLoadingValidation
    SOURCES EnvironmentPreparedLoadingValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS "unit;resource;environment;async"
    TIMEOUT 120
)

# Resource view cache validation tests
rvx_add_gtest(
    NAME ResourceViewCacheValidation
    SOURCES
        ResourceViewCacheValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/ResourceViewCache.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

# Visual golden validation utility
add_executable(VisualGoldenValidation
    VisualGoldenValidation/main.cpp
)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    VisualGoldenValidation
)

target_link_libraries(VisualGoldenValidation PRIVATE
    RVX_TestCommon
)

target_compile_features(VisualGoldenValidation PRIVATE cxx_std_20)
target_include_directories(VisualGoldenValidation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(RVX_VISUAL_GOLDEN_REPORT_SELFTEST_IMAGE
    "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/R7_DX11_320x180.ppm")
if(EXISTS "${RVX_VISUAL_GOLDEN_REPORT_SELFTEST_IMAGE}")
    add_test(
        NAME VisualGoldenValidationReportSelfTest
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_VISUAL_GOLDEN_REPORT_SELFTEST_IMAGE}"
            --actual "${RVX_VISUAL_GOLDEN_REPORT_SELFTEST_IMAGE}"
            --report "${CMAKE_CURRENT_BINARY_DIR}/VisualGoldenValidationReportSelfTest.json"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(VisualGoldenValidationReportSelfTest PROPERTIES
        LABELS unit
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()

add_executable(ImageContentValidation
    ImageContentValidation/main.cpp
)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    ImageContentValidation
)

target_link_libraries(ImageContentValidation PRIVATE
    RVX_TestCommon
)

target_compile_features(ImageContentValidation PRIVATE cxx_std_20)
target_include_directories(ImageContentValidation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(SampleReportContentValidation
    SampleReportContentValidation/main.cpp
)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    SampleReportContentValidation
)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(SampleReportContentValidation PRIVATE
    nlohmann_json::nlohmann_json
)
target_compile_features(SampleReportContentValidation PRIVATE cxx_std_20)

if((RVX_ENABLE_P0_QUALIFICATION_MATRIX OR
    RVX_ENABLE_P1_QUALIFICATION_MATRIX) AND
    (NOT TARGET VisualGoldenValidation OR
    NOT TARGET ImageContentValidation OR
    NOT TARGET SampleReportContentValidation))
    message(FATAL_ERROR
        "Enabled P0/P1 qualification matrices require visual and SampleReport validators")
endif()

add_executable(SampleQualificationReportValidation
    SampleQualificationReportValidation/main.cpp
)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    SampleQualificationReportValidation
)
target_compile_features(SampleQualificationReportValidation PRIVATE cxx_std_20)

function(rvx_add_renderverse_primary_smoke SAMPLE_ID SAMPLE_LABEL ASSET_ID BACKEND_NAME BACKEND_LABEL)
    cmake_parse_arguments(
        RVX_SAMPLE
        "WAIT_READY;ASSESSMENT"
        "READY_TIMEOUT_MS;READY_MAX_FRAMES;TIMEOUT;ASSESSMENT_PROFILE;ASSET_LICENSE"
        "SAMPLE_ARGS;REPORT_ARGS"
        ${ARGN})
    if(RVX_SAMPLE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "rvx_add_renderverse_primary_smoke received unparsed arguments: ${RVX_SAMPLE_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        return()
    endif()

    set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
        RenderVerseSamples
    )
    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/RenderVerseSamples")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/${config_name}/RenderVerseSamples")
    endforeach()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}${SAMPLE_LABEL}")
    set(smoke_actual
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.ppm")
    set(smoke_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.json")
    set(smoke_startup_report
        "${CMAKE_CURRENT_BINARY_DIR}/StartupTimelines/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.json")
    set(smoke_readiness_args)
    set(smoke_assessment_args)
    set(smoke_assessment_report_args)
    # Every primary smoke captures a screenshot, which implicitly enables the
    # readiness-bounded runner path and may require a carrying publication frame.
    set(smoke_frame_report_args --min-frame-count 8)
    if(RVX_SAMPLE_WAIT_READY)
        if(NOT RVX_SAMPLE_READY_TIMEOUT_MS)
            set(RVX_SAMPLE_READY_TIMEOUT_MS 30000)
        endif()
        if(NOT RVX_SAMPLE_READY_MAX_FRAMES)
            set(RVX_SAMPLE_READY_MAX_FRAMES 64)
        endif()
        list(APPEND smoke_readiness_args
            --wait-ready
            --ready-timeout-ms ${RVX_SAMPLE_READY_TIMEOUT_MS}
            --ready-max-frames ${RVX_SAMPLE_READY_MAX_FRAMES})
    endif()
    if(RVX_SAMPLE_ASSESSMENT)
        if(NOT RVX_SAMPLE_ASSESSMENT_PROFILE)
            set(RVX_SAMPLE_ASSESSMENT_PROFILE smoke)
        endif()
        foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
            file(MAKE_DIRECTORY
                "${CMAKE_CURRENT_BINARY_DIR}/FrameworkAssessments/${config_name}/RenderVerseSamples")
        endforeach()
        set(smoke_assessment_report
            "${CMAKE_CURRENT_BINARY_DIR}/FrameworkAssessments/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.json")
        list(APPEND smoke_assessment_args
            --assessment-report "${smoke_assessment_report}"
            --assessment-profile ${RVX_SAMPLE_ASSESSMENT_PROFILE})
        list(APPEND smoke_assessment_report_args
            --require-assessment-summary
            --assessment-report-path "${smoke_assessment_report}"
            --assessment-blocking-count 0
            --assessment-dropped-event-count 0
            --require-assessment-pass)
    endif()
    if(NOT RVX_SAMPLE_TIMEOUT)
        set(RVX_SAMPLE_TIMEOUT 120)
    endif()
    if(NOT RVX_SAMPLE_ASSET_LICENSE)
        set(RVX_SAMPLE_ASSET_LICENSE
            LicenseRef-RenderVerseX-Generated-Fixture)
    endif()

    add_test(
        NAME ${test_prefix}Smoke
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample ${SAMPLE_ID}
            --backend ${BACKEND_NAME}
            --smoke
            --frames 8
            --width 320
            --height 180
            --quality low
            --screenshot "${smoke_actual}"
            --report "${smoke_report}"
            --startup-report "${smoke_startup_report}"
            --diagnostics
            --validation
            ${smoke_readiness_args}
            ${smoke_assessment_args}
            ${RVX_SAMPLE_SAMPLE_ARGS}
    )
    set_tests_properties(${test_prefix}Smoke PROPERTIES
        LABELS "visual;gpu;sample;primary;${BACKEND_NAME}"
        TIMEOUT ${RVX_SAMPLE_TIMEOUT}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    add_test(
        NAME ${test_prefix}ImageContentValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${smoke_actual}"
            --min-width 320
            --min-height 180
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
    )
    set_tests_properties(${test_prefix}ImageContentValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Smoke
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    add_test(
        NAME ${test_prefix}ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${smoke_report}"
            --sample-name ${SAMPLE_ID}
            --category visual
            --backend ${BACKEND_NAME}
            --quality low
            ${smoke_frame_report_args}
            --asset-id ${ASSET_ID}
            --asset-license-spdx ${RVX_SAMPLE_ASSET_LICENSE}
            --require-redistributable
            --min-visible-objects 1
            --require-enabled-feature ModelResourceLoad
            --require-enabled-feature SceneInstantiation
            --require-enabled-feature RenderGraph
            --require-enabled-feature SceneRendering
            --require-ready
            ${smoke_assessment_report_args}
            ${RVX_SAMPLE_REPORT_ARGS}
    )
    set_tests_properties(${test_prefix}ReportValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Smoke
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

function(rvx_add_renderverse_p0_cross_path_parity SAMPLE_LABEL BACKEND_NAME BACKEND_LABEL)
    if(NOT TARGET VisualGoldenValidation)
        return()
    endif()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}${SAMPLE_LABEL}")
    set(direct_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}Direct_320x180.ppm")
    set(gpu_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}GPUDriven_320x180.ppm")
    set(parity_diff
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_DirectVsGPUDriven_diff.ppm")
    set(parity_report
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_DirectVsGPUDriven.json")
    add_test(
        NAME ${test_prefix}CrossPathParityValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${gpu_capture}"
            --actual "${direct_capture}"
            --diff "${parity_diff}"
            --report "${parity_report}"
            --tolerance 0.0
            --max-different-pixels 0)
    set_tests_properties(${test_prefix}CrossPathParityValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;gpu-driven;parity;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS "${test_prefix}GPUDrivenSmoke;${test_prefix}DirectSmoke"
        FIXTURES_REQUIRED
            "${test_prefix}GPUDrivenFreshCapture;${test_prefix}DirectFreshCapture")
endfunction()

function(rvx_add_renderverse_p0_matrix SAMPLE_ID SAMPLE_LABEL ASSET_ID ASSET_LICENSE
         BACKEND_NAME BACKEND_LABEL)
    cmake_parse_arguments(
        RVX_P0
        ""
        "ENVIRONMENT_ID;ENVIRONMENT_LICENSE;ENVIRONMENT_SOURCE_URI;READY_MAX_FRAMES"
        "CONTENT_IDS;REPORT_ARGS"
        ${ARGN})
    if(RVX_P0_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "rvx_add_renderverse_p0_matrix received unparsed arguments: ${RVX_P0_UNPARSED_ARGUMENTS}")
    endif()

    if(NOT RVX_P0_READY_MAX_FRAMES)
        set(RVX_P0_READY_MAX_FRAMES 600)
    endif()

    foreach(path_label Direct GPUDriven)
        if(path_label STREQUAL "Direct")
            set(render_path direct)
            set(execution_gate --require-direct-execution)
        else()
            set(render_path gpu-driven)
            set(execution_gate --require-gpu-driven-execution)
        endif()

        set(report_args
            --render-path ${render_path}
            --require-requested-render-path ${render_path}
            --require-actual-render-path ${render_path}
            --require-requested-physics-backend built-in
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            ${execution_gate}
            --asset-license-spdx ${ASSET_LICENSE}
            --require-phase stable)
        foreach(content_id IN LISTS RVX_P0_CONTENT_IDS)
            list(APPEND report_args --require-asset-content-id ${content_id})
        endforeach()
        if(RVX_P0_ENVIRONMENT_ID)
            list(APPEND report_args
                --environment-asset-id ${RVX_P0_ENVIRONMENT_ID}
                --environment-license-spdx ${RVX_P0_ENVIRONMENT_LICENSE}
                --environment-source-uri ${RVX_P0_ENVIRONMENT_SOURCE_URI}
                --require-environment-redistributable)
        endif()

        rvx_add_renderverse_primary_smoke(
            ${SAMPLE_ID} ${SAMPLE_LABEL}${path_label}
            ${ASSET_ID} ${BACKEND_NAME} ${BACKEND_LABEL}
            WAIT_READY
            ASSESSMENT
            ASSESSMENT_PROFILE qualification
            READY_TIMEOUT_MS 120000
            READY_MAX_FRAMES ${RVX_P0_READY_MAX_FRAMES}
            SAMPLE_ARGS
                --render-path ${render_path}
            REPORT_ARGS
                ${report_args}
                ${RVX_P0_REPORT_ARGS})
    endforeach()

    rvx_add_renderverse_p0_cross_path_parity(
        ${SAMPLE_LABEL} ${BACKEND_NAME} ${BACKEND_LABEL})
endfunction()

function(rvx_add_renderverse_base_render_path_matrices BACKEND_NAME BACKEND_LABEL)
    set(generated_fixture_license LicenseRef-RenderVerseX-Generated-Fixture)

    rvx_add_renderverse_p0_matrix(
        model-rendering ModelRendering shadow-plane-caster
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL})

    rvx_add_renderverse_p0_matrix(
        asset-gallery AssetGallery r7-triangle
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        REPORT_ARGS
            --min-visible-objects 3
            --require-enabled-feature ResourceSceneAdapters
            --require-enabled-feature CatalogStaticGallery
            --require-enabled-feature BoundsCameraFraming
            --require-unsupported-feature RuntimeAssetSwitching
            --require-asset-entry r7-triangle
            --require-asset-entry shadow-plane-caster
            --require-asset-entry pbr-material-grid
            --require-resource-diagnostic "gallery assets=3")

    rvx_add_renderverse_p0_matrix(
        lighting-shadows LightingShadows shadow-plane-caster
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        REPORT_ARGS
            --require-enabled-feature ShadowCasterReceiverFixture
            --require-enabled-feature CascadedDirectionalShadowRequested
            --require-enabled-feature DirectionalShadow
            --require-directional-shadow
            --require-resource-diagnostic "shadow atlas resolution=1024"
            --require-resource-diagnostic "shadow cascade count=1")

    rvx_add_renderverse_p0_matrix(
        render-pipeline RenderPipeline shadow-plane-caster
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        REPORT_ARGS
            --require-enabled-feature EngineRenderPipelineConfiguration
            --require-enabled-feature DirectionalShadow
            --require-enabled-feature PostProcessRenderGraph
            --require-directional-shadow
            --min-render-graph-passes 8
            --min-enabled-post-process-effects 2
            --min-post-process-graph-passes 2
            --forbid-unsupported-post-process
            --require-resource-diagnostic
                "pipeline ownership=Render/RenderGraph/PostProcessStack")

    rvx_add_renderverse_p0_matrix(
        model-viewer ModelViewer r7-triangle
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        REPORT_ARGS
            --require-enabled-feature BoundsCameraFraming
            --require-enabled-feature OrbitCamera
            --require-enabled-feature ModelRenderReady
            --require-enabled-feature ProceduralSkyScene
            --forbid-fallback-reason PacketEnvironmentResourcesUnavailable)

    rvx_add_renderverse_p0_matrix(
        pbr-materials PBRMaterials pbr-material-grid
        ${generated_fixture_license} ${BACKEND_NAME} ${BACKEND_LABEL}
        READY_MAX_FRAMES 192
        ENVIRONMENT_ID pbr-reference-environment
        ENVIRONMENT_LICENSE ${generated_fixture_license}
        ENVIRONMENT_SOURCE_URI
            repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        REPORT_ARGS
            --require-enabled-feature PBRMetallicRoughnessMaterials
            --require-enabled-feature PBRMaterialParameterCube
            --require-enabled-feature PBRBaseColorFamilyAxis
            --require-enabled-feature MetallicRoughnessFactorCube
            --require-enabled-feature SharedMeshSceneInstances
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-texture-ibl
            --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
            --require-enabled-feature EnvironmentResourceLoad
            --require-enabled-feature EnvironmentIBL
            --require-enabled-feature DiffuseIBL
            --require-enabled-feature SpecularIBL
            --require-enabled-feature TextureIBL
            --require-enabled-feature ControlledStudioLighting
            --require-enabled-feature BoundsCameraFraming
            --require-resource-diagnostic "pbr materials=125"
            --require-resource-diagnostic "matrix shape=5x5x5"
            --require-resource-diagnostic "scene mesh instances=125"
            --require-resource-diagnostic "opaque draw packets=125")
endfunction()

function(rvx_add_renderverse_scene_lifecycle_matrix BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_p0_matrix(
        scene-lifecycle SceneLifecycle water-bottle
        CC0-1.0 ${BACKEND_NAME} ${BACKEND_LABEL}
        CONTENT_IDS
            b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
            4df540b802067f192a7aa38aed524dda141dc23d9cb7a7e6272fc021694198ed
        REPORT_ARGS
            --require-asset-entry water-bottle
            --require-action Spawn
            --require-action ReparentKeepLocal
            --require-action ReparentKeepWorld
            --require-action RecycleSecondaryCamera
            --require-action PresentSecondaryCamera
            --require-action RestorePrimaryCamera
            --require-action MutateLight
            --require-action MutateMaterialSlots
            --require-action DestroyAndReuseEntitySlot
            --require-invariant EntitySlotReuse
            --require-invariant CameraEntityReuse
            --require-invariant KeepLocal
            --require-invariant KeepWorld
            --require-invariant CameraTemporalReset
            --require-invariant LightWriteVersion
            --require-invariant MaterialSlotWriteVersion
            --require-invariant RetirementQueuesDrained
            --require-metric secondary-camera-presentation
            --require-metric primary-camera-presentation
            --require-metric-equals final-owned-entity-count=5
            --require-metric-equals expected-owned-entity-count=5
            --require-metric light-write-version
            --require-metric material-slot-write-version
            --require-metric-equals terminal-pending-destroy=0
            --require-metric-equals terminal-cleanup-required=0
            --require-metric-equals terminal-retiring=0
            --require-metric-equals terminal-recyclable=0
            --require-metric-equals terminal-render-pending-upload=0
            --require-metric-equals terminal-render-retirement=0
            --require-supported-feature SceneLifecycleProbe
            --require-supported-feature DeferredEcsCommands
            --require-supported-feature GenerationSafeEntityIdentity
            --require-supported-feature TransformHierarchyAuthorityProbe
            --require-supported-feature EcsCameraLifecycleProbe
            --require-supported-feature RuntimeLightFragmentMutationProbe
            --require-supported-feature RuntimeMaterialSlotMutationProbe)
endfunction()

function(rvx_add_renderverse_asset_streaming_matrix BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_p0_matrix(
        asset-streaming AssetStreaming water-bottle CC0-1.0
        ${BACKEND_NAME} ${BACKEND_LABEL}
        READY_MAX_FRAMES 2000
        CONTENT_IDS
            b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
            4df540b802067f192a7aa38aed524dda141dc23d9cb7a7e6272fc021694198ed
            9582c0dc0dee813be77f60e6ddf7213987c7e11497bf3cc66fd7b18957ae0d26
            d9649a867305e7152ea1dcc5c30f4624fcb17fe7d4f7468da00e9b1997f1bb6c
            c7e93451e25a1ee35247d265dbd06c760de904ba339890a97c248648c3170431
            1d7d24ff3eb0f54e30834bf14717c3a303cb179e41fca4829545768e0999d46a
        REPORT_ARGS
            --require-asset-entry water-bottle
            --require-asset-entry corset
            --require-asset-entry crytek-sponza
            --require-action QueueThreeModelsAndDuplicateSubscriber
            --require-action PresentFallbackFrame
            --require-action VerifyRequestCoalescing
            --require-action CancelDuplicateSubscriber
            --require-action RequestUnknownAssetAndRollback
            --require-action InstantiateMinimumResidentModels
            --require-action CommitRealTextures
            --require-action CancelPublishedLargeModel
            --require-action AwaitExactRetirement
            --require-action ReloadSameAssetId
            --require-action PresentReloadedRevision
            --require-action DrainAllQueues
            --require-invariant FirstFrameBeforeFullyResident
            --require-invariant RequestCoalescing
            --require-invariant UnknownAssetRollback
            --require-invariant IncrementalTexturePublication
            --require-invariant ExactRetirement
            --require-invariant ReloadContentIdentityAndGeneration
            --require-invariant DrainedQueues
            --require-metric-equals pendingOperations=0
            --require-metric-equals activeSubscribers=0
            --require-metric-equals pendingAsyncJobs=0
            --require-metric-equals pendingAsyncCompletions=0
            --require-metric-equals decodeQueued=0
            --require-metric-equals decodeActive=0
            --require-metric-equals pendingPublications=0
            --require-metric-equals pendingUploads=0
            --require-metric-equals pendingReplacements=0
            --require-metric-equals pendingRollbacks=0
            --require-metric-equals pendingRetirements=0
            --require-metric-equals queuedLeaseUnloads=0
            --require-metric closureUnloadRequests
            --require-supported-feature AsynchronousModelRequests
            --require-supported-feature CatalogAssetIdRequests
            --require-supported-feature CoalescedSubscriberCancellation
            --require-supported-feature UnknownAssetNotFoundRollback
            --require-supported-feature FallbackMinimumResidency
            --require-supported-feature TextureContentReplacement
            --require-supported-feature CompletionOwnedRetirement
            --require-supported-feature RetiredAssetIdReload
            --require-supported-feature DescriptorRevisionEvidence)
endfunction()

function(rvx_add_renderverse_interior_matrix BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_p0_matrix(
        interior-rendering InteriorRendering crytek-sponza CC-BY-3.0
        ${BACKEND_NAME} ${BACKEND_LABEL}
        ENVIRONMENT_ID kloofendal-48d-partly-cloudy-pure-sky
        ENVIRONMENT_LICENSE CC0-1.0
        ENVIRONMENT_SOURCE_URI https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/kloofendal_48d_partly_cloudy_puresky_2k.hdr
        CONTENT_IDS
            c7e93451e25a1ee35247d265dbd06c760de904ba339890a97c248648c3170431
            1d7d24ff3eb0f54e30834bf14717c3a303cb179e41fca4829545768e0999d46a
            5244534e9cf5b606f2ff513aa00ddb161b0a4826ffd88a0d3bd03ac29247d198
            5976bfa6aee89beecfd51d98b8b47f625965c1df245d0816fd708860d64ab449
            f94c219679ca4b47844ab7e7f9c3012c8195f2f1002a102aed6a1a7f9249737d
            62bad1e475a107ac0941a1b19bc6231fee6de5ae5558ed12115da03e12d4af34
        REPORT_ARGS
            --require-asset-entry crytek-sponza
            --require-asset-entry kloofendal-48d-partly-cloudy-pure-sky
            --require-asset-entry interior-rendering-p0a
            --require-directional-shadow
            --require-texture-ibl
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-action PresentQualifiedInteriorFrame
            --require-invariant VerifiedProductionAssetComposition
            --require-invariant DeterministicTransparentGlassPanels
            --require-invariant CompletedInteriorRenderContract
            --require-metric productionMeshCount
            --require-metric productionMaterialCount
            --require-metric productionTextureCount
            --require-metric-equals transparentGlassPanelCount=2
            --require-supported-feature AsynchronousModelRequest
            --require-supported-feature AsynchronousEnvironmentRequest
            --require-supported-feature ProductionSponzaAsset
            --require-supported-feature DeterministicTransparentGlassPanels
            --require-supported-feature LocalLightsWithoutLocalShadows
            --require-supported-feature InteriorRenderingDiagnosticsQualified
            --require-supported-feature EnvironmentResourceLoad
            --require-supported-feature EnvironmentIBL
            --require-supported-feature TransparentPassObserved
            --require-supported-feature ClusteredLightingObserved
            --require-supported-feature DirectionalCSMObserved
            --require-supported-feature TextureIBLEnabled
            --require-unsupported-feature PointShadow
            --require-unsupported-feature SpotShadow
            --require-unsupported-feature HZBOcclusion
            --require-resource-diagnostic "directional cascades requested=3"
            --require-resource-diagnostic "directional shadow map requested=2048"
            --require-resource-diagnostic "point lights requested=8"
            --require-resource-diagnostic "spot lights requested=4"
            --require-resource-diagnostic "point local-shadow requests=0"
            --require-resource-diagnostic "spot local-shadow requests=0"
            --require-resource-diagnostic "HZB requests=0")
endfunction()

function(rvx_add_renderverse_physics_sandbox_matrix BACKEND_NAME BACKEND_LABEL)
    foreach(path_label Direct GPUDriven)
        if(path_label STREQUAL "Direct")
            set(render_path direct)
            set(execution_gate --require-direct-execution)
        else()
            set(render_path gpu-driven)
            set(execution_gate --require-gpu-driven-execution)
        endif()

        rvx_add_renderverse_primary_smoke(
            physics-sandbox PhysicsSandbox${path_label}
            interior-rendering-p0a ${BACKEND_NAME} ${BACKEND_LABEL}
            TIMEOUT 300
            WAIT_READY
            ASSESSMENT
            ASSESSMENT_PROFILE qualification
            READY_TIMEOUT_MS 120000
            READY_MAX_FRAMES 1200
            SAMPLE_ARGS
                --asset interior-rendering-p0a
                --render-path ${render_path}
            REPORT_ARGS
                --render-path ${render_path}
                --require-requested-render-path ${render_path}
                --require-actual-render-path ${render_path}
                --require-requested-physics-backend built-in
                --require-actual-physics-backend built-in
                --forbid-physics-backend-fallback
                --require-native-validation-available
                --require-native-validation-enabled
                --require-native-validation-read-complete
                --require-native-validation-error-count 0
                --require-native-validation-corruption-count 0
                ${execution_gate}
                --require-readiness-wait
                --require-phase stable
                --require-asset-entry interior-rendering-p0a
                --require-asset-content-id f94c219679ca4b47844ab7e7f9c3012c8195f2f1002a102aed6a1a7f9249737d
                --require-asset-content-id 62bad1e475a107ac0941a1b19bc6231fee6de5ae5558ed12115da03e12d4af34
                --require-action CreateBodies
                --require-action Settle120Steps
                --require-action DriveKinematicPlatform120Steps
                --require-action VerifyDynamicContacts
                --require-action RebuildCollider
                --require-action DestroyBody
                --require-action RecreateSameSlot
                --require-action RejectStaleHandle
                --require-invariant BuiltInAuthority
                --require-invariant DynamicPhysicsToScene
                --require-invariant DynamicFloorContact
                --require-invariant ColliderFragmentRefresh
                --require-invariant ProbeCleanup
                --require-invariant StaleSceneRefRejected
                --require-invariant TerminalCensus
                --require-metric sceneRuntimeId
                --require-metric sceneFrameSequence
                --require-metric physicsFixedStepSequence
                --require-metric fixedSteps
                --require-metric requestedFixedSteps
                --require-metric executedFixedSteps
                --require-metric kinematicPoseWrites
                --require-metric kinematicLastTargetStep
                --require-metric dynamicFloorContactEvidence
                --require-metric-equals fixedDeltaNumerator=1
                --require-metric-equals fixedDeltaDenominator=60
                --require-metric-equals maxSubsteps=4
                --require-metric-equals deterministicSeed=1381390416
                --require-metric-equals staticBodies=1
                --require-metric-equals dynamicBodies=17
                --require-metric-equals kinematicBodies=1
                --require-metric-equals bodyCount=19
                --require-metric-equals colliderCount=19
                --require-metric-equals bridgeBindings=19
                --require-metric-equals pendingBridgeCleanup=0
                --require-metric-equals bridgeStructuralContinuityLoss=0
                --require-metric-equals bridgeCleanupContinuityLoss=0
                --require-metric-equals bridgeAuthoritativeReconcile=1
                --require-metric-equals modelFullyResident=1
                --require-metric-equals staleRefRejected=1
                --require-enabled-feature PureEcsPhysicsFragments
                --require-enabled-feature WorldEcsPhysicsDiagnostics
                --assessment-block-grade ADVISORY
                --require-resource-diagnostic "model path="
                --require-resource-diagnostic "model meshes=8"
                --require-resource-diagnostic "model materials=8"
                --require-resource-diagnostic "render path=${render_path}"
                --require-resource-diagnostic "requested backend=${BACKEND_NAME}"
                --require-resource-diagnostic "realized backend=${BACKEND_NAME}")
    endforeach()

    rvx_add_renderverse_p0_cross_path_parity(
        PhysicsSandbox ${BACKEND_NAME} ${BACKEND_LABEL})
endfunction()

function(rvx_add_renderverse_animation_character_matrix BACKEND_NAME BACKEND_LABEL)
    # Keep these records as one CTest argument each.  The escaped semicolons
    # are required because CMake otherwise treats the typed receipt fields as
    # a list before it writes CTestTestfile.cmake.
    string(JOIN "$<SEMICOLON>" casual_female_cook_admission
        "assetId=casual-female" "packageContentId=42dec4e0c8ed81f540f666943f7823e500f53d22c9eccc7a9750cc9c1324dee5/3595794/5" "required=true" "accepted=true"
        "manifestPath=$<SHELL_PATH:$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female/CookManifest.rvxmanifest>" "cookedRoot=$<SHELL_PATH:$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female>"
        "declaredSource=source/self-contained-artifact/87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a/2058883/1" "observedSource=source/self-contained-artifact/87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a/2058883/1"
        "declaredCooked=cooked-artifact/dependency-closure/c91215f2751eb272b6750de1a03829d2599577a13842fa76dc488e621a94ce89/1534789/3" "observedCooked=cooked-artifact/dependency-closure/c91215f2751eb272b6750de1a03829d2599577a13842fa76dc488e621a94ce89/1534789/3"
        "declaredManifest=cook-manifest/self-contained-artifact/87e1fb3666fce0a75740894ea6124e8cb27e7fa88c4142fe9debe40670132096/2122/1" "observedManifest=cook-manifest/self-contained-artifact/87e1fb3666fce0a75740894ea6124e8cb27e7fa88c4142fe9debe40670132096/2122/1"
        "cookSettingsHash=d47a3f3b166ac705cfd06128506f127d776f43ad59eaeaa5cb4d52c1946829fa" "recipeHash=815ad489b05e8595afec66fa1dadfc39cb1dea900cea4e18bad7e5092f8592c5" "toolName=RVXCook" "toolVersion=2.0.0")
    string(JOIN "$<SEMICOLON>" root_motion_cook_admission
        "assetId=casual-female-walk-root-motion" "packageContentId=3e32fb4c332021590f3841ddb7fe858e09c8240246bfbf16e67bb8b795413360/56156/2" "required=true" "accepted=true"
        "manifestPath=$<SHELL_PATH:$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female-root-motion/CookManifest.rvxmanifest>" "cookedRoot=$<SHELL_PATH:$<TARGET_FILE_DIR:RenderVerseSamples>/Assets/Samples/cooked/casual-female-root-motion>"
        "declaredSource=source/self-contained-artifact/36f4824d1d82aeaae42ce33208b3c774622e09d4547290c89ace8560eeb3ad11/927584/1" "observedSource=source/self-contained-artifact/36f4824d1d82aeaae42ce33208b3c774622e09d4547290c89ace8560eeb3ad11/927584/1"
        "declaredCooked=cooked-artifact/self-contained-artifact/b820610acdfb55f6f182d753ddeabdc0d73d635816d0b14d93b8ae899b16acbb/54462/1" "observedCooked=cooked-artifact/self-contained-artifact/b820610acdfb55f6f182d753ddeabdc0d73d635816d0b14d93b8ae899b16acbb/54462/1"
        "declaredManifest=cook-manifest/self-contained-artifact/00f59bc67067c1503f148daa77da2093e151cf3910aaed186b27bca93fd93c17/1694/1" "observedManifest=cook-manifest/self-contained-artifact/00f59bc67067c1503f148daa77da2093e151cf3910aaed186b27bca93fd93c17/1694/1"
        "cookSettingsHash=0618f669053ab678745f85251fe13a8db9966f8196a30f955ea1585bc2c81963" "recipeHash=2984a9d350b89ae24050cbcc3087c4714f66aafca654ea8cef9b7bc5a8dcd709" "toolName=RVXCook" "toolVersion=2.0.0")
    string(JOIN "$<SEMICOLON>" root_motion_asset_receipt
        "assetId=casual-female-walk-root-motion" "role=animation" "kind=animation" "packageContentId=3e32fb4c332021590f3841ddb7fe858e09c8240246bfbf16e67bb8b795413360/56156/2"
        "expected=cooked-artifact/self-contained-artifact/8a04a46e8507391e11df7b2102e5ba3260aecea14d822b33c2d5871c9909e1ca/54462/1" "observed=cooked-artifact/self-contained-artifact/8a04a46e8507391e11df7b2102e5ba3260aecea14d822b33c2d5871c9909e1ca/54462/1"
        "verificationStatus=verified" "loaded=true" "verified=true")

    foreach(path_label Direct GPUDriven)
        if(path_label STREQUAL "Direct")
            set(render_path direct)
            set(execution_gate --require-direct-execution)
            set(request_feature DirectPathRequested)
        else()
            set(render_path gpu-driven)
            set(execution_gate --require-gpu-driven-execution)
            set(request_feature GPUDrivenPathRequested)
        endif()

        rvx_add_renderverse_primary_smoke(
            animation-character AnimationCharacter${path_label}
            casual-female ${BACKEND_NAME} ${BACKEND_LABEL}
            TIMEOUT 300
            WAIT_READY
            ASSESSMENT
            ASSESSMENT_PROFILE qualification
            ASSET_LICENSE CC0-1.0
            READY_TIMEOUT_MS 120000
            READY_MAX_FRAMES 2400
            SAMPLE_ARGS
                --render-path ${render_path}
            REPORT_ARGS
                --render-path ${render_path}
                --require-requested-render-path ${render_path}
                --require-actual-render-path ${render_path}
                --require-requested-physics-backend built-in
                --require-actual-physics-backend built-in
                --forbid-physics-backend-fallback
                --require-native-validation-available
                --require-native-validation-enabled
                --require-native-validation-read-complete
                --require-native-validation-error-count 0
                --require-native-validation-corruption-count 0
                ${execution_gate}
                --require-readiness-wait
                --require-phase stable
                --min-visible-objects 7
                --require-asset-entry casual-female
                --require-asset-entry casual-female-walk-root-motion
                --require-asset-entry water-bottle
                --require-asset-content-id 87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a
                --require-asset-content-id 42dec4e0c8ed81f540f666943f7823e500f53d22c9eccc7a9750cc9c1324dee5
                --require-asset-content-id b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
                --require-asset-content-id 4df540b802067f192a7aa38aed524dda141dc23d9cb7a7e6272fc021694198ed
                --require-cook-admission "${casual_female_cook_admission}"
                --require-cook-admission "${root_motion_cook_admission}"
                --require-asset-receipt "${root_motion_asset_receipt}"
                --require-action BindEcsCharacter
                --require-action EvaluateFixed120
                --require-action ConsumeRootMotion120
                --require-action PresentSkinningPalette
                --require-invariant ExactEcsBinding
                --require-invariant FixedSchedule
                --require-invariant RootMotionEpoch
                --require-invariant PresentedPalette
                --require-metric fixedSteps
                --require-metric poseEvaluations
                --require-metric rootMotionIntents
                --require-metric rootMotionMillimetres
                --require-metric paletteHash
                --require-metric-equals fixedSteps=120
                --require-metric-equals poseEvaluations=120
                --require-metric-equals rootMotionIntents=120
                --require-metric-equals rootMotionMillimetres=2000
                --require-enabled-feature ModelResourceLoad
                --require-enabled-feature SceneInstantiation
                --require-enabled-feature EcsCharacterModel
                --require-enabled-feature EcsSkinningPalette
                --require-enabled-feature EcsAnimationAsset
                --require-enabled-feature EcsRootMotion
                --require-enabled-feature EcsAnimationPhysicsBinding
                --require-enabled-feature EcsFixedAnimation
                --require-enabled-feature EcsPresentedSkinningPalette
                --require-enabled-feature RenderPathPolicySelection
                --require-enabled-feature ${request_feature}
                --require-unsupported-feature Jolt
                --require-resource-diagnostic "fixed steps=120"
                --require-resource-diagnostic "root-motion intents=120"
                --require-resource-diagnostic "render path=${render_path}")
    endforeach()

    rvx_add_renderverse_p0_cross_path_parity(
        AnimationCharacter ${BACKEND_NAME} ${BACKEND_LABEL})

endfunction()

function(rvx_add_renderverse_pbr_response_validation BACKEND_NAME BACKEND_LABEL SAMPLE_LABEL)
    if(NOT TARGET ImageContentValidation)
        return()
    endif()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}${SAMPLE_LABEL}")
    set(smoke_actual
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.ppm")
    add_test(
        NAME ${test_prefix}ResponseValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${smoke_actual}"
            --min-width 320
            --min-height 180
            --require-pbr-grid-response
            --min-pbr-metallic-luma-delta 40
            --min-pbr-roughness-contrast-delta 2
            --sample-step 2
    )
    set_tests_properties(${test_prefix}ResponseValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Smoke
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

function(rvx_add_renderverse_pbr_texture_smoke BACKEND_NAME BACKEND_LABEL)
    rvx_add_renderverse_primary_smoke(
        pbr-materials PBRMaterialsTexture pbr-material-texture-cube
        ${BACKEND_NAME} ${BACKEND_LABEL}
        WAIT_READY
        SAMPLE_ARGS
            --asset pbr-material-texture-cube
        REPORT_ARGS
        --require-enabled-feature PBRMetallicRoughnessMaterials
        --require-enabled-feature PBRMetallicRoughnessTexture
        --require-enabled-feature PBRMaterialParameterCube
        --require-enabled-feature PBRBaseColorFamilyAxis
        --require-enabled-feature MetallicRoughnessTextureCube
        --require-opaque-execution
        --require-opaque-materials-ready
        --require-opaque-material-texture-flags 4
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature ControlledStudioLighting
        --require-enabled-feature BoundsCameraFraming
        --require-resource-diagnostic "pbr materials=5"
        --require-resource-diagnostic "metallic-roughness texture=5x5"
        --require-resource-diagnostic "metallic levels=5"
        --require-resource-diagnostic "roughness levels=5"
        --require-resource-diagnostic "base-color families=5"
        --require-resource-diagnostic "matrix shape=5x5x5"
        --require-resource-diagnostic "total material spheres=125"
        --require-resource-diagnostic "matrix x-axis=metallic 0.00,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix y-axis=roughness 0.05,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix z-axis=base-color terracotta,gold,green,blue,neutral"
        --require-resource-diagnostic "material workflow=metallic-roughness texture"
        --require-resource-diagnostic "texture cube materials=5"
        --require-resource-diagnostic "texture cube combinations=125"
    )
    rvx_add_renderverse_pbr_response_validation(
        ${BACKEND_NAME} ${BACKEND_LABEL} PBRMaterialsTexture)
endfunction()

function(rvx_add_renderverse_pbr_workflow_parity BACKEND_NAME BACKEND_LABEL)
    if(NOT TARGET VisualGoldenValidation)
        return()
    endif()

    set(factor_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamples${BACKEND_LABEL}PBRMaterials_320x180.ppm")
    set(texture_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamples${BACKEND_LABEL}PBRMaterialsTexture_320x180.ppm")
    set(diff_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamples${BACKEND_LABEL}PBRMaterialsWorkflowDiff.ppm")
    set(parity_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/RenderVerseSamples${BACKEND_LABEL}PBRMaterialsWorkflowParity.json")
    add_test(
        NAME RenderVerseSamples${BACKEND_LABEL}PBRMaterialsWorkflowParity
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${factor_capture}"
            --actual "${texture_capture}"
            --diff "${diff_capture}"
            --report "${parity_report}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(
        RenderVerseSamples${BACKEND_LABEL}PBRMaterialsWorkflowParity
        PROPERTIES
        LABELS "visual;gpu;sample;primary;pbr;parity;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS "RenderVerseSamples${BACKEND_LABEL}PBRMaterialsSmoke;RenderVerseSamples${BACKEND_LABEL}PBRMaterialsTextureSmoke"
        FIXTURES_REQUIRED
            "RenderVerseSamples${BACKEND_LABEL}PBRMaterialsFreshCapture;RenderVerseSamples${BACKEND_LABEL}PBRMaterialsTextureFreshCapture"
    )
endfunction()

function(rvx_add_renderverse_external_model_viewer_validation BACKEND_NAME BACKEND_LABEL)
    if(NOT RVX_ENABLE_EXTERNAL_ASSET_TESTS OR
       NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation OR
       NOT TARGET SampleQualificationReportValidation)
        return()
    endif()
    if(NOT RVX_EXTERNAL_TEST_ASSET_ROOT)
        message(FATAL_ERROR
            "RVX_ENABLE_EXTERNAL_ASSET_TESTS requires RVX_EXTERNAL_TEST_ASSET_ROOT")
    endif()

    cmake_path(ABSOLUTE_PATH RVX_EXTERNAL_TEST_ASSET_ROOT
        NORMALIZE OUTPUT_VARIABLE RVX_EXTERNAL_MODEL_ROOT)
    set(external_catalog "${RVX_EXTERNAL_MODEL_ROOT}/catalog.json")
    set(external_model
        "${RVX_EXTERNAL_MODEL_ROOT}/models/free_1975_porsche_911_930_turbo/scene.gltf")
    set(external_license
        "${RVX_EXTERNAL_MODEL_ROOT}/models/free_1975_porsche_911_930_turbo/license.txt")
    foreach(required_file
            "${external_catalog}"
            "${external_model}"
            "${external_license}")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR
                "External Model Viewer gate requires ${required_file}")
        endif()
    endforeach()

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/ExternalAssets")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/ExternalAssets")
    endforeach()

    set(test_prefix
        "RenderVerseSamples${BACKEND_LABEL}ExternalPorscheModelViewer")
    set(capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ExternalAssets/${test_prefix}_320x180.ppm")
    set(report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/ExternalAssets/${test_prefix}_320x180.json")

    add_test(
        NAME ${test_prefix}Validation
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample model-viewer
            --asset free-1975-porsche-911-930-turbo
            --catalog "${external_catalog}"
            --asset-root "${RVX_EXTERNAL_MODEL_ROOT}"
            --backend ${BACKEND_NAME}
            --smoke
            --frames 1
            --wait-ready
            --ready-timeout-ms 180000
            --ready-max-frames 4096
            --width 320
            --height 180
            --quality low
            --screenshot "${capture}"
            --report "${report}"
            --diagnostics
            --validation
    )
    set_tests_properties(${test_prefix}Validation PROPERTIES
        LABELS "visual;gpu;sample;model-viewer;external-assets;qualification-candidate;${BACKEND_NAME}"
        TIMEOUT 300
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
    )

    add_test(
        NAME ${test_prefix}ImageContentValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${capture}"
            --min-width 320
            --min-height 180
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
    )
    set_tests_properties(${test_prefix}ImageContentValidation PROPERTIES
        LABELS "visual;gpu;sample;model-viewer;external-assets;qualification-candidate;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Validation
    )

    add_test(
        NAME ${test_prefix}ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${report}"
            --sample-name model-viewer
            --category visual
            --backend ${BACKEND_NAME}
            --quality low
            --min-frame-count 1
            --min-visible-objects 50
            --asset-id free-1975-porsche-911-930-turbo
            --asset-license-spdx CC-BY-4.0
            --require-redistributable
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-ready
            --require-readiness-wait
            --require-enabled-feature ModelResourceLoad
            --require-enabled-feature SceneInstantiation
            --require-enabled-feature ModelInspection
            --require-enabled-feature ModelRenderReady
            --require-enabled-feature BoundsCameraFraming
            --require-enabled-feature OrbitCamera
            --require-resource-diagnostic "model meshes=75"
            --require-resource-diagnostic "model materials=14"
            --require-resource-diagnostic "model nodes=141"
    )
    set_tests_properties(${test_prefix}ReportValidation PROPERTIES
        LABELS "visual;gpu;sample;model-viewer;external-assets;qualification-candidate;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Validation
    )

    if(TARGET RVXCook)
        set(cooked_output_root
            "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerCookedPorsche")
        set(cooked_model "${cooked_output_root}/scene.rva")
        set(cooked_fixture RenderVerseSamplesExternalPorscheCookedModelFixture)
    endif()
    if(TARGET RVXCook AND NOT TEST ${cooked_fixture})
        add_test(
            NAME ${cooked_fixture}
            COMMAND $<TARGET_FILE:RVXCook>
                --source "${RVX_EXTERNAL_MODEL_ROOT}/models/free_1975_porsche_911_930_turbo"
                --output "${cooked_output_root}"
                --manifest "${cooked_output_root}/CookManifest.rvxmanifest"
                --model-only
                --non-recursive
                --fail-on-errors
        )
        set_tests_properties(${cooked_fixture} PROPERTIES
            LABELS "resource;cook;model;external-assets;qualification-fixture"
            TIMEOUT 600
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
    endif()

    foreach(asset_mode source cooked)
        if(asset_mode STREQUAL "cooked" AND NOT TARGET RVXCook)
            continue()
        endif()
        if(asset_mode STREQUAL "source")
            set(mode_label Source)
            set(model_arguments
                --asset free-1975-porsche-911-930-turbo
                --catalog "${external_catalog}"
                --asset-root "${RVX_EXTERNAL_MODEL_ROOT}")
            set(mode_run_arguments --smoke)
            set(mode_dependency "")
        else()
            set(mode_label Cooked)
            set(model_arguments --model "${cooked_model}")
            # The generated model product is intentionally outside the source
            # catalog. It is still validation-enabled, but cannot claim the
            # catalog-backed redistributable provenance required by --smoke.
            set(mode_run_arguments "")
            set(mode_dependency ${cooked_fixture})
        endif()

        set(direct_capture "")
        set(gpu_capture "")
        set(direct_lifetime_test "")
        set(gpu_lifetime_test "")
        foreach(render_path direct gpu-driven)
            if(render_path STREQUAL "direct")
                set(path_label Direct)
            else()
                set(path_label GPUDriven)
            endif()
            if(asset_mode STREQUAL "source")
                # Preserve the established source qualification test names.
                set(lifetime_prefix "${test_prefix}${path_label}")
            else()
                set(lifetime_prefix "${test_prefix}${mode_label}${path_label}")
            endif()
            set(lifetime_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_600.json")
            set(lifetime_sample_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_sample.json")
            set(startup_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerStartup/${lifetime_prefix}.json")
            set(lifetime_capture
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_320x180.ppm")
            add_test(
                NAME ${lifetime_prefix}Lifetime600
                COMMAND $<TARGET_FILE:RenderVerseSamples>
                    --sample model-viewer
                    ${model_arguments}
                    --backend ${BACKEND_NAME}
                    --render-path ${render_path}
                    ${mode_run_arguments}
                    --frames 1
                    --wait-ready
                    --ready-timeout-ms 180000
                    --ready-max-frames 4096
                    --width 320
                    --height 180
                    --quality low
                    --deterministic-orbit
                    --startup-report "${startup_report}"
                    --lifetime-warmup-frames 120
                    --lifetime-observation-frames 480
                    --lifetime-resize-interval 240
                    --lifetime-resize-settle-frames 30
                    --lifetime-report "${lifetime_report}"
                    --screenshot "${lifetime_capture}"
                    --report "${lifetime_sample_report}"
                    --diagnostics
                    --validation
            )
            set_tests_properties(${lifetime_prefix}Lifetime600 PROPERTIES
                LABELS "visual;gpu;sample;model-viewer;lifetime;external-assets;qualification;${asset_mode};${BACKEND_NAME};${render_path}"
                TIMEOUT 1200
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
            )
            if(mode_dependency)
                set_property(TEST ${lifetime_prefix}Lifetime600 PROPERTY
                    DEPENDS ${mode_dependency})
            endif()

            set(qualification_test ${lifetime_prefix}QualificationValidation)
            add_test(
                NAME ${qualification_test}
                COMMAND $<TARGET_FILE:SampleQualificationReportValidation>
                    --startup-report "${startup_report}"
                    --lifetime-report "${lifetime_report}"
                    --sample-report "${lifetime_sample_report}"
                    --backend ${BACKEND_NAME}
                    --render-path ${render_path}
                    --asset-mode ${asset_mode}
                    --expected-texture-count 27
                    --expected-visible-objects 75
                    --max-first-present-ms 5000
                    --require-resize
            )
            set_tests_properties(${qualification_test} PROPERTIES
                LABELS "diagnostics;sample;model-viewer;startup;residency;lifetime;qualification;${asset_mode};${BACKEND_NAME};${render_path}"
                TIMEOUT 30
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                DEPENDS ${lifetime_prefix}Lifetime600
            )

            if(render_path STREQUAL "direct")
                set(direct_capture "${lifetime_capture}")
                set(direct_lifetime_test ${lifetime_prefix}Lifetime600)
            else()
                set(gpu_capture "${lifetime_capture}")
                set(gpu_lifetime_test ${lifetime_prefix}Lifetime600)
            endif()

            if(RVX_ENABLE_LONG_RUNNING_QUALIFICATION_TESTS)
                set(soak_report
                    "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_10000.json")
                set(soak_sample_report
                    "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_10000_sample.json")
                set(soak_startup_report
                    "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerStartup/${lifetime_prefix}_10000.json")
                add_test(
                    NAME ${lifetime_prefix}Lifetime10000
                    COMMAND $<TARGET_FILE:RenderVerseSamples>
                        --sample model-viewer
                        ${model_arguments}
                        --backend ${BACKEND_NAME}
                        --render-path ${render_path}
                        ${mode_run_arguments}
                        --frames 1
                        --wait-ready
                        --ready-timeout-ms 180000
                        --ready-max-frames 4096
                        --width 320
                        --height 180
                        --quality low
                        --deterministic-orbit
                        --startup-report "${soak_startup_report}"
                        --lifetime-warmup-frames 120
                        --lifetime-observation-frames 9880
                        --lifetime-min-duration-ms 300000
                        --lifetime-resize-interval 2400
                        --lifetime-resize-settle-frames 60
                        --lifetime-report "${soak_report}"
                        --report "${soak_sample_report}"
                        --diagnostics
                        --validation
                )
                set_tests_properties(${lifetime_prefix}Lifetime10000 PROPERTIES
                    LABELS "visual;gpu;sample;model-viewer;lifetime;soak;external-assets;qualification;${asset_mode};${BACKEND_NAME};${render_path}"
                    TIMEOUT 3600
                    WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
                )
                if(mode_dependency)
                    set_property(TEST ${lifetime_prefix}Lifetime10000 PROPERTY
                        DEPENDS ${mode_dependency})
                endif()
            endif()
        endforeach()

        if(TARGET VisualGoldenValidation)
            set(path_parity_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${test_prefix}${mode_label}DirectGPUParity.json")
            set(path_parity_diff
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${test_prefix}${mode_label}DirectGPUParity.diff.ppm")
            add_test(
                NAME ${test_prefix}${mode_label}DirectGPUParity
                COMMAND $<TARGET_FILE:VisualGoldenValidation>
                    --expected "${direct_capture}"
                    --actual "${gpu_capture}"
                    --diff "${path_parity_diff}"
                    --report "${path_parity_report}"
                    --tolerance 0.0
                    --max-different-pixels 0
            )
            set_tests_properties(${test_prefix}${mode_label}DirectGPUParity PROPERTIES
                LABELS "visual;gpu;sample;model-viewer;lifetime;parity;external-assets;qualification;${asset_mode};${BACKEND_NAME}"
                TIMEOUT 30
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                DEPENDS "${direct_lifetime_test};${gpu_lifetime_test}"
            )
        endif()
    endforeach()
endfunction()

function(rvx_add_renderverse_external_environment_validation BACKEND_NAME BACKEND_LABEL)
    if(NOT RVX_ENABLE_EXTERNAL_ENVIRONMENT_TESTS OR
       NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        return()
    endif()
    if(NOT RVX_ENABLE_EXTERNAL_ASSET_TESTS)
        message(FATAL_ERROR
            "RVX_ENABLE_EXTERNAL_ENVIRONMENT_TESTS requires RVX_ENABLE_EXTERNAL_ASSET_TESTS")
    endif()
    if(NOT RVX_EXTERNAL_TEST_ASSET_ROOT)
        message(FATAL_ERROR
            "RVX_ENABLE_EXTERNAL_ASSET_TESTS requires RVX_EXTERNAL_TEST_ASSET_ROOT")
    endif()

    cmake_path(ABSOLUTE_PATH RVX_EXTERNAL_TEST_ASSET_ROOT
        NORMALIZE OUTPUT_VARIABLE RVX_EXTERNAL_ENVIRONMENT_ROOT)
    set(external_catalog "${RVX_EXTERNAL_ENVIRONMENT_ROOT}/catalog.json")
    set(external_environment
        "${RVX_EXTERNAL_ENVIRONMENT_ROOT}/textures/valley_of_desolation_4k.exr")
    set(external_catalog_companion
        "${RVX_EXTERNAL_ENVIRONMENT_ROOT}/textures/cowboy_town_hall_8k.exr")
    set(external_license
        "${RVX_EXTERNAL_ENVIRONMENT_ROOT}/LICENSE.polyhaven-cc0.txt")
    foreach(required_file
            "${external_catalog}"
            "${external_environment}"
            "${external_catalog_companion}"
            "${external_license}")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR
                "External environment gate requires ${required_file}")
        endif()
    endforeach()

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/ExternalAssets")
        file(MAKE_DIRECTORY
            "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/ExternalAssets")
    endforeach()

    set(test_prefix
        "RenderVerseSamples${BACKEND_LABEL}ExternalPolyHavenPBR")
    set(capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ExternalAssets/${test_prefix}_320x180.ppm")
    set(report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/ExternalAssets/${test_prefix}_320x180.json")

    add_test(
        NAME ${test_prefix}Validation
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample pbr-materials
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/Samples/PBRMaterialGrid.gltf"
            --environment valley-of-desolation-4k
            --catalog "${external_catalog}"
            --asset-root "${RVX_EXTERNAL_ENVIRONMENT_ROOT}"
            --backend ${BACKEND_NAME}
            --frames 8
            --wait-ready
            --ready-timeout-ms 120000
            --ready-max-frames 120
            --width 320
            --height 180
            --quality low
            --screenshot "${capture}"
            --report "${report}"
            --diagnostics
            --validation
    )
    set_tests_properties(${test_prefix}Validation PROPERTIES
        LABELS "visual;gpu;sample;pbr;external-assets;qualification-candidate;${BACKEND_NAME}"
        TIMEOUT 300
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
    )

    add_test(
        NAME ${test_prefix}ImageContentValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${capture}"
            --min-width 320
            --min-height 180
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
    )
    set_tests_properties(${test_prefix}ImageContentValidation PROPERTIES
        LABELS "visual;gpu;sample;pbr;external-assets;qualification-candidate;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Validation
    )

    add_test(
        NAME ${test_prefix}ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${report}"
            --sample-name pbr-materials
            --category visual
            --backend ${BACKEND_NAME}
            --quality low
            --min-frame-count 8
            --min-visible-objects 1
            --environment-asset-id valley-of-desolation-4k
            --environment-license-spdx CC0-1.0
            --environment-source-uri https://polyhaven.com/a/valley_of_desolation
            --require-environment-redistributable
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-texture-ibl
            --require-ready
            --require-readiness-wait
            --require-enabled-feature PBRMetallicRoughnessMaterials
            --require-enabled-feature EnvironmentResourceLoad
            --require-enabled-feature EnvironmentIBL
            --require-enabled-feature DiffuseIBL
            --require-enabled-feature SpecularIBL
    )
    set_tests_properties(${test_prefix}ReportValidation PROPERTIES
        LABELS "visual;gpu;sample;pbr;external-assets;qualification-candidate;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Validation
    )
endfunction()

function(rvx_add_renderverse_readiness_smoke BACKEND_NAME BACKEND_LABEL)
    if(NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
        return()
    endif()

    set(test_prefix
        "RenderVerseSamples${BACKEND_LABEL}PBRMaterialsReadiness")
    set(smoke_actual
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.ppm")
    set(smoke_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.json")

    add_test(
        NAME ${test_prefix}Smoke
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample pbr-materials
            --backend ${BACKEND_NAME}
            --smoke
            --frames 1
            --wait-ready
            --ready-timeout-ms 60000
            --ready-max-frames 192
            --width 320
            --height 180
            --quality low
            --screenshot "${smoke_actual}"
            --report "${smoke_report}"
            --diagnostics
            --validation
    )
    set_tests_properties(${test_prefix}Smoke PROPERTIES
        LABELS "visual;gpu;sample;primary;readiness;${BACKEND_NAME}"
        TIMEOUT 120
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        FIXTURES_SETUP "${test_prefix}FreshCapture"
    )

    add_test(
        NAME ${test_prefix}ImageContentValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${smoke_actual}"
            --min-width 320
            --min-height 180
            --min-unique-colors 16
            --min-luma-range 8
            --near-black-threshold 8
            --max-near-black-ratio 0.95
            --sample-step 2
    )
    set_tests_properties(${test_prefix}ImageContentValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;readiness;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Smoke
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )

    add_test(
        NAME ${test_prefix}ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${smoke_report}"
            --sample-name pbr-materials
            --category visual
            --backend ${BACKEND_NAME}
            --quality low
            --min-frame-count 2
            --min-visible-objects 1
            --environment-asset-id pbr-reference-environment
            --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
            --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
            --require-environment-redistributable
            --require-opaque-execution
            --require-opaque-materials-ready
            --require-texture-ibl
            --require-ready
            --require-readiness-wait
    )
    set_tests_properties(${test_prefix}ReportValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;readiness;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Smoke
        FIXTURES_REQUIRED "${test_prefix}FreshCapture"
    )
endfunction()

function(rvx_add_renderverse_gpu_driven_parity BACKEND_NAME BACKEND_LABEL)
    if(NOT RVX_ENABLE_P0_QUALIFICATION_MATRIX)
        return()
    endif()
    foreach(required_target
            RenderVerseSamples
            ImageContentValidation
            SampleReportContentValidation
            VisualGoldenValidation)
        if(NOT TARGET ${required_target})
            message(FATAL_ERROR
                "RVX_ENABLE_P0_QUALIFICATION_MATRIX requires ${required_target} for the gpu-driven sample matrix")
        endif()
    endforeach()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}GPUDriven")
    set(gpu_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_GPU_320x180.ppm")
    set(direct_capture
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_Direct_320x180.ppm")
    set(parity_diff
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_Parity_320x180.diff.ppm")
    set(parity_report
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_Parity_320x180.json")
    set(gpu_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_GPU_320x180.json")
    set(direct_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_Direct_320x180.json")

    foreach(render_path gpu-driven direct)
        if(render_path STREQUAL "gpu-driven")
            set(path_label GPU)
            set(capture "${gpu_capture}")
            set(report "${gpu_report}")
            set(execution_gate --require-gpu-driven-execution)
            set(request_feature GPUDrivenPathRequested)
            set(render_path_args --render-path gpu-driven)
        else()
            set(path_label Direct)
            set(capture "${direct_capture}")
            set(report "${direct_report}")
            set(execution_gate --require-direct-execution)
            set(request_feature DirectPathRequested)
            set(render_path_args --render-path direct)
        endif()

        add_test(
            NAME ${test_prefix}${path_label}Smoke
            COMMAND $<TARGET_FILE:RenderVerseSamples>
                --sample gpu-driven
                ${render_path_args}
                --backend ${BACKEND_NAME}
                --smoke
                --frames 1
                --wait-ready
                --ready-timeout-ms 30000
                --ready-max-frames 64
                --width 320
                --height 180
                --quality low
                --screenshot "${capture}"
                --report "${report}"
                --diagnostics
                --validation
        )
        set_tests_properties(${test_prefix}${path_label}Smoke PROPERTIES
            LABELS "visual;gpu;sample;primary;gpu-driven;parity;${BACKEND_NAME}"
            TIMEOUT 120
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
            FIXTURES_SETUP "${test_prefix}${path_label}FreshCapture"
        )

        add_test(
            NAME ${test_prefix}${path_label}ImageContentValidation
            COMMAND $<TARGET_FILE:ImageContentValidation>
                --image "${capture}"
                --min-width 320
                --min-height 180
                --min-unique-colors 16
                --min-luma-range 8
                --near-black-threshold 8
                --max-near-black-ratio 0.95
                --sample-step 2
        )
        set_tests_properties(
            ${test_prefix}${path_label}ImageContentValidation PROPERTIES
            LABELS "visual;gpu;sample;primary;gpu-driven;parity;${BACKEND_NAME}"
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            DEPENDS ${test_prefix}${path_label}Smoke
            FIXTURES_REQUIRED "${test_prefix}${path_label}FreshCapture"
        )

        add_test(
            NAME ${test_prefix}${path_label}ReportValidation
            COMMAND $<TARGET_FILE:SampleReportContentValidation>
                --report "${report}"
                --sample-name gpu-driven
                --category visual
                --backend ${BACKEND_NAME}
                --quality low
                --render-path ${render_path}
                --require-requested-render-path ${render_path}
                --require-actual-render-path ${render_path}
                --min-frame-count 2
                --asset-id water-bottle
                --asset-license-spdx CC0-1.0
                --require-redistributable
                --require-asset-content-id b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
                --require-asset-content-id 4df540b802067f192a7aa38aed524dda141dc23d9cb7a7e6272fc021694198ed
                --min-visible-objects 25
                --require-ready
                --require-readiness-wait
                --require-enabled-feature ModelResourceLoad
                --require-enabled-feature SceneInstantiation
                --require-enabled-feature SharedSceneExtractionInput
                --require-enabled-feature SharedModelResourceInstancing
                --require-enabled-feature RenderPathPolicySelection
                --require-enabled-feature ${request_feature}
                --require-resource-diagnostic "scene model instances=25"
                --require-resource-diagnostic "unique model resources=1"
                ${execution_gate}
        )
        set_tests_properties(
            ${test_prefix}${path_label}ReportValidation PROPERTIES
            LABELS "visual;gpu;sample;primary;gpu-driven;parity;${BACKEND_NAME}"
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            DEPENDS ${test_prefix}${path_label}Smoke
            FIXTURES_REQUIRED "${test_prefix}${path_label}FreshCapture"
        )
    endforeach()

    add_test(
        NAME ${test_prefix}CrossPathParityValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${gpu_capture}"
            --actual "${direct_capture}"
            --diff "${parity_diff}"
            --report "${parity_report}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(${test_prefix}CrossPathParityValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;gpu-driven;parity;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS "${test_prefix}GPUSmoke;${test_prefix}DirectSmoke"
        FIXTURES_REQUIRED
            "${test_prefix}GPUFreshCapture;${test_prefix}DirectFreshCapture"
    )
endfunction()

if(WIN32 AND RVX_ENABLE_DX12)
    if(RVX_ENABLE_P0_QUALIFICATION_MATRIX)
    rvx_add_renderverse_scene_lifecycle_matrix(dx12 DX12)
    rvx_add_renderverse_asset_streaming_matrix(dx12 DX12)
    rvx_add_renderverse_interior_matrix(dx12 DX12)
    rvx_add_renderverse_primary_smoke(
        rendering-stress RenderingStressDirect interior-rendering-p0a dx12 DX12
        TIMEOUT 300
        WAIT_READY
        ASSESSMENT
        ASSESSMENT_PROFILE qualification
        READY_TIMEOUT_MS 120000
        READY_MAX_FRAMES 600
        SAMPLE_ARGS
            --render-path direct
            --workload-scale pr
        REPORT_ARGS
            --render-path direct
            --require-requested-render-path direct
            --require-actual-render-path direct
            --require-requested-physics-backend auto
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-direct-execution
            --require-enabled-feature DeterministicRenderingStressStateMachine
            --require-enabled-feature SharedModelResourceProceduralActors
            --require-enabled-feature RetainedSceneDeltaDiagnostics
            --require-enabled-feature GPUSceneDeltaDiagnostics
            --require-resource-diagnostic "workload scale=pr"
            --require-resource-diagnostic "workload objects=1000"
            --require-resource-diagnostic "workload dirty=10"
            --require-resource-diagnostic "workload churn=100"
            --require-resource-diagnostic "phase=Complete")
    rvx_add_renderverse_primary_smoke(
        rendering-stress RenderingStressGPUDriven interior-rendering-p0a dx12 DX12
        TIMEOUT 300
        WAIT_READY
        ASSESSMENT
        ASSESSMENT_PROFILE qualification
        READY_TIMEOUT_MS 120000
        READY_MAX_FRAMES 600
        SAMPLE_ARGS
            --render-path gpu-driven
            --workload-scale pr
        REPORT_ARGS
            --render-path gpu-driven
            --require-requested-render-path gpu-driven
            --require-actual-render-path gpu-driven
            --require-requested-physics-backend auto
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-gpu-driven-execution
            --require-enabled-feature DeterministicRenderingStressStateMachine
            --require-enabled-feature SharedModelResourceProceduralActors
            --require-enabled-feature RetainedSceneDeltaDiagnostics
            --require-enabled-feature GPUSceneDeltaDiagnostics
            --require-resource-diagnostic "workload scale=pr"
            --require-resource-diagnostic "workload objects=1000"
            --require-resource-diagnostic "workload dirty=10"
            --require-resource-diagnostic "workload churn=100"
            --require-resource-diagnostic "phase=Complete")
    rvx_add_renderverse_p0_cross_path_parity(RenderingStress dx12 DX12)
    endif()
    if(RVX_ENABLE_P1_QUALIFICATION_MATRIX)
    rvx_add_renderverse_physics_sandbox_matrix(dx12 DX12)
    rvx_add_renderverse_animation_character_matrix(dx12 DX12)
    endif()
    rvx_add_renderverse_primary_smoke(
        model-rendering ModelRendering shadow-plane-caster dx12 DX12)
    rvx_add_renderverse_primary_smoke(
        asset-gallery AssetGallery r7-triangle dx12 DX12
        REPORT_ARGS
        --min-visible-objects 3
        --require-enabled-feature ResourceSceneAdapters
        --require-enabled-feature CatalogStaticGallery
        --require-enabled-feature BoundsCameraFraming
        --require-unsupported-feature RuntimeAssetSwitching
        --require-asset-entry r7-triangle
        --require-asset-entry shadow-plane-caster
        --require-asset-entry pbr-material-grid
        --require-resource-diagnostic "gallery assets=3"
    )
    rvx_add_renderverse_primary_smoke(
        lighting-shadows LightingShadows shadow-plane-caster dx12 DX12
        REPORT_ARGS
        --require-enabled-feature ShadowCasterReceiverFixture
        --require-enabled-feature CascadedDirectionalShadowRequested
        --require-enabled-feature DirectionalShadow
        --require-directional-shadow
        --require-resource-diagnostic "shadow atlas resolution=1024"
        --require-resource-diagnostic "shadow cascade count=1"
    )
    rvx_add_renderverse_primary_smoke(
        render-pipeline RenderPipeline shadow-plane-caster dx12 DX12
        REPORT_ARGS
        --require-enabled-feature EngineRenderPipelineConfiguration
        --require-enabled-feature DirectionalShadow
        --require-enabled-feature PostProcessRenderGraph
        --require-directional-shadow
        --min-render-graph-passes 8
        --min-enabled-post-process-effects 2
        --min-post-process-graph-passes 2
        --forbid-unsupported-post-process
        --require-resource-diagnostic "pipeline ownership=Render/RenderGraph/PostProcessStack"
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewer r7-triangle dx12 DX12
        SAMPLE_ARGS
            --environment pbr-reference-environment
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature TextureEnvironmentScene
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewerProcedural r7-triangle dx12 DX12
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --require-enabled-feature ModelRenderReady
        --require-enabled-feature ProceduralSkyScene
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
    )
    rvx_add_renderverse_primary_smoke(
        pbr-materials PBRMaterials pbr-material-grid dx12 DX12
        WAIT_READY
        READY_TIMEOUT_MS 60000
        READY_MAX_FRAMES 192
        REPORT_ARGS
        --require-enabled-feature PBRMetallicRoughnessMaterials
        --require-enabled-feature PBRMaterialParameterCube
        --require-enabled-feature PBRBaseColorFamilyAxis
        --require-enabled-feature MetallicRoughnessFactorCube
        --require-enabled-feature SharedMeshSceneInstances
        --require-opaque-execution
        --require-opaque-materials-ready
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature ControlledStudioLighting
        --require-enabled-feature BoundsCameraFraming
        --require-resource-diagnostic "pbr materials=125"
        --require-resource-diagnostic "metallic levels=5"
        --require-resource-diagnostic "roughness levels=5"
        --require-resource-diagnostic "base-color families=5"
        --require-resource-diagnostic "matrix shape=5x5x5"
        --require-resource-diagnostic "total material spheres=125"
        --require-resource-diagnostic "matrix x-axis=metallic 0.00,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix y-axis=roughness 0.05,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix z-axis=base-color terracotta,gold,green,blue,neutral"
        --require-resource-diagnostic "material workflow=factors"
        --require-resource-diagnostic "factor cube materials=125"
        --require-resource-diagnostic "factor cube combinations=125"
        --require-resource-diagnostic "model meshes=1"
        --require-resource-diagnostic "scene mesh instances=125"
        --require-resource-diagnostic "unique mesh resources=1"
        --require-resource-diagnostic "material overrides=125"
        --require-resource-diagnostic "opaque draw packets=125"
    )
    rvx_add_renderverse_base_render_path_matrices(dx12 DX12)
    rvx_add_renderverse_pbr_response_validation(dx12 DX12 PBRMaterials)
    rvx_add_renderverse_pbr_texture_smoke(dx12 DX12)
    rvx_add_renderverse_pbr_workflow_parity(dx12 DX12)
    rvx_add_renderverse_external_model_viewer_validation(dx12 DX12)
    rvx_add_renderverse_external_environment_validation(dx12 DX12)
    rvx_add_renderverse_readiness_smoke(dx12 DX12)
    rvx_add_renderverse_gpu_driven_parity(dx12 DX12)
endif()
if(RVX_ENABLE_VULKAN)
    if(RVX_ENABLE_P0_QUALIFICATION_MATRIX)
    rvx_add_renderverse_scene_lifecycle_matrix(vulkan Vulkan)
    rvx_add_renderverse_asset_streaming_matrix(vulkan Vulkan)
    rvx_add_renderverse_interior_matrix(vulkan Vulkan)
    rvx_add_renderverse_primary_smoke(
        rendering-stress RenderingStressDirect interior-rendering-p0a vulkan Vulkan
        TIMEOUT 300
        WAIT_READY
        ASSESSMENT
        ASSESSMENT_PROFILE qualification
        READY_TIMEOUT_MS 120000
        READY_MAX_FRAMES 600
        SAMPLE_ARGS
            --render-path direct
            --workload-scale pr
        REPORT_ARGS
            --render-path direct
            --require-requested-render-path direct
            --require-actual-render-path direct
            --require-requested-physics-backend auto
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-direct-execution
            --require-enabled-feature DeterministicRenderingStressStateMachine
            --require-enabled-feature SharedModelResourceProceduralActors
            --require-enabled-feature RetainedSceneDeltaDiagnostics
            --require-enabled-feature GPUSceneDeltaDiagnostics
            --require-resource-diagnostic "workload scale=pr"
            --require-resource-diagnostic "workload objects=1000"
            --require-resource-diagnostic "workload dirty=10"
            --require-resource-diagnostic "workload churn=100"
            --require-resource-diagnostic "phase=Complete")
    rvx_add_renderverse_primary_smoke(
        rendering-stress RenderingStressGPUDriven interior-rendering-p0a vulkan Vulkan
        TIMEOUT 300
        WAIT_READY
        ASSESSMENT
        ASSESSMENT_PROFILE qualification
        READY_TIMEOUT_MS 120000
        READY_MAX_FRAMES 600
        SAMPLE_ARGS
            --render-path gpu-driven
            --workload-scale pr
        REPORT_ARGS
            --render-path gpu-driven
            --require-requested-render-path gpu-driven
            --require-actual-render-path gpu-driven
            --require-requested-physics-backend auto
            --require-actual-physics-backend built-in
            --forbid-physics-backend-fallback
            --require-native-validation-available
            --require-native-validation-enabled
            --require-native-validation-read-complete
            --require-native-validation-error-count 0
            --require-native-validation-corruption-count 0
            --require-gpu-driven-execution
            --require-enabled-feature DeterministicRenderingStressStateMachine
            --require-enabled-feature SharedModelResourceProceduralActors
            --require-enabled-feature RetainedSceneDeltaDiagnostics
            --require-enabled-feature GPUSceneDeltaDiagnostics
            --require-resource-diagnostic "workload scale=pr"
            --require-resource-diagnostic "workload objects=1000"
            --require-resource-diagnostic "workload dirty=10"
            --require-resource-diagnostic "workload churn=100"
            --require-resource-diagnostic "phase=Complete")
    rvx_add_renderverse_p0_cross_path_parity(RenderingStress vulkan Vulkan)
    endif()
    if(RVX_ENABLE_P1_QUALIFICATION_MATRIX)
    rvx_add_renderverse_physics_sandbox_matrix(vulkan Vulkan)
    rvx_add_renderverse_animation_character_matrix(vulkan Vulkan)
    endif()
    rvx_add_renderverse_primary_smoke(
        model-rendering ModelRendering shadow-plane-caster vulkan Vulkan)
    rvx_add_renderverse_primary_smoke(
        asset-gallery AssetGallery r7-triangle vulkan Vulkan
        REPORT_ARGS
        --min-visible-objects 3
        --require-enabled-feature ResourceSceneAdapters
        --require-enabled-feature CatalogStaticGallery
        --require-enabled-feature BoundsCameraFraming
        --require-unsupported-feature RuntimeAssetSwitching
        --require-asset-entry r7-triangle
        --require-asset-entry shadow-plane-caster
        --require-asset-entry pbr-material-grid
        --require-resource-diagnostic "gallery assets=3"
    )
    rvx_add_renderverse_primary_smoke(
        lighting-shadows LightingShadows shadow-plane-caster vulkan Vulkan
        REPORT_ARGS
        --require-enabled-feature ShadowCasterReceiverFixture
        --require-enabled-feature CascadedDirectionalShadowRequested
        --require-enabled-feature DirectionalShadow
        --require-directional-shadow
        --require-resource-diagnostic "shadow atlas resolution=1024"
        --require-resource-diagnostic "shadow cascade count=1"
    )
    rvx_add_renderverse_primary_smoke(
        render-pipeline RenderPipeline shadow-plane-caster vulkan Vulkan
        REPORT_ARGS
        --require-enabled-feature EngineRenderPipelineConfiguration
        --require-enabled-feature DirectionalShadow
        --require-enabled-feature PostProcessRenderGraph
        --require-directional-shadow
        --min-render-graph-passes 8
        --min-enabled-post-process-effects 2
        --min-post-process-graph-passes 2
        --forbid-unsupported-post-process
        --require-resource-diagnostic "pipeline ownership=Render/RenderGraph/PostProcessStack"
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewer r7-triangle vulkan Vulkan
        SAMPLE_ARGS
            --environment pbr-reference-environment
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature TextureEnvironmentScene
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewerProcedural r7-triangle vulkan Vulkan
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --require-enabled-feature ModelRenderReady
        --require-enabled-feature ProceduralSkyScene
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
    )
    rvx_add_renderverse_primary_smoke(
        pbr-materials PBRMaterials pbr-material-grid vulkan Vulkan
        WAIT_READY
        READY_TIMEOUT_MS 60000
        READY_MAX_FRAMES 192
        REPORT_ARGS
        --require-enabled-feature PBRMetallicRoughnessMaterials
        --require-enabled-feature PBRMaterialParameterCube
        --require-enabled-feature PBRBaseColorFamilyAxis
        --require-enabled-feature MetallicRoughnessFactorCube
        --require-enabled-feature SharedMeshSceneInstances
        --require-opaque-execution
        --require-opaque-materials-ready
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature ControlledStudioLighting
        --require-enabled-feature BoundsCameraFraming
        --require-resource-diagnostic "pbr materials=125"
        --require-resource-diagnostic "metallic levels=5"
        --require-resource-diagnostic "roughness levels=5"
        --require-resource-diagnostic "base-color families=5"
        --require-resource-diagnostic "matrix shape=5x5x5"
        --require-resource-diagnostic "total material spheres=125"
        --require-resource-diagnostic "matrix x-axis=metallic 0.00,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix y-axis=roughness 0.05,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix z-axis=base-color terracotta,gold,green,blue,neutral"
        --require-resource-diagnostic "material workflow=factors"
        --require-resource-diagnostic "factor cube materials=125"
        --require-resource-diagnostic "factor cube combinations=125"
        --require-resource-diagnostic "model meshes=1"
        --require-resource-diagnostic "scene mesh instances=125"
        --require-resource-diagnostic "unique mesh resources=1"
        --require-resource-diagnostic "material overrides=125"
        --require-resource-diagnostic "opaque draw packets=125"
    )
    rvx_add_renderverse_base_render_path_matrices(vulkan Vulkan)
    rvx_add_renderverse_pbr_response_validation(vulkan Vulkan PBRMaterials)
    rvx_add_renderverse_pbr_texture_smoke(vulkan Vulkan)
    rvx_add_renderverse_pbr_workflow_parity(vulkan Vulkan)
    rvx_add_renderverse_external_model_viewer_validation(vulkan Vulkan)
    rvx_add_renderverse_external_environment_validation(vulkan Vulkan)
    rvx_add_renderverse_readiness_smoke(vulkan Vulkan)
    rvx_add_renderverse_gpu_driven_parity(vulkan Vulkan)
endif()
if(APPLE AND RVX_ENABLE_METAL)
    rvx_add_renderverse_primary_smoke(
        model-rendering ModelRendering shadow-plane-caster metal Metal)
    rvx_add_renderverse_primary_smoke(
        asset-gallery AssetGallery r7-triangle metal Metal
        REPORT_ARGS
        --min-visible-objects 3
        --require-enabled-feature ResourceSceneAdapters
        --require-enabled-feature CatalogStaticGallery
        --require-enabled-feature BoundsCameraFraming
        --require-unsupported-feature RuntimeAssetSwitching
        --require-asset-entry r7-triangle
        --require-asset-entry shadow-plane-caster
        --require-asset-entry pbr-material-grid
        --require-resource-diagnostic "gallery assets=3"
    )
    rvx_add_renderverse_primary_smoke(
        lighting-shadows LightingShadows shadow-plane-caster metal Metal
        REPORT_ARGS
        --require-enabled-feature ShadowCasterReceiverFixture
        --require-enabled-feature CascadedDirectionalShadowRequested
        --require-enabled-feature DirectionalShadow
        --require-directional-shadow
        --require-resource-diagnostic "shadow atlas resolution=1024"
        --require-resource-diagnostic "shadow cascade count=1"
    )
    rvx_add_renderverse_primary_smoke(
        render-pipeline RenderPipeline shadow-plane-caster metal Metal
        REPORT_ARGS
        --require-enabled-feature EngineRenderPipelineConfiguration
        --require-enabled-feature DirectionalShadow
        --require-enabled-feature PostProcessRenderGraph
        --require-directional-shadow
        --min-render-graph-passes 8
        --min-enabled-post-process-effects 2
        --min-post-process-graph-passes 2
        --forbid-unsupported-post-process
        --require-resource-diagnostic "pipeline ownership=Render/RenderGraph/PostProcessStack"
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewer r7-triangle metal Metal
        SAMPLE_ARGS
            --environment pbr-reference-environment
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature TextureEnvironmentScene
    )
    rvx_add_renderverse_primary_smoke(
        model-viewer ModelViewerProcedural r7-triangle metal Metal
        REPORT_ARGS
        --require-enabled-feature BoundsCameraFraming
        --require-enabled-feature OrbitCamera
        --require-enabled-feature ModelRenderReady
        --require-enabled-feature ProceduralSkyScene
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
    )
    rvx_add_renderverse_primary_smoke(
        pbr-materials PBRMaterials pbr-material-grid metal Metal
        WAIT_READY
        READY_TIMEOUT_MS 60000
        READY_MAX_FRAMES 192
        REPORT_ARGS
        --require-enabled-feature PBRMetallicRoughnessMaterials
        --require-enabled-feature PBRMaterialParameterCube
        --require-enabled-feature PBRBaseColorFamilyAxis
        --require-enabled-feature MetallicRoughnessFactorCube
        --require-enabled-feature SharedMeshSceneInstances
        --require-opaque-execution
        --require-opaque-materials-ready
        --environment-asset-id pbr-reference-environment
        --environment-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
        --environment-source-uri repo://Samples/Common/Tools/SampleHDRFixtureWriter.cpp
        --require-environment-redistributable
        --require-texture-ibl
        --forbid-fallback-reason PacketEnvironmentResourcesUnavailable
        --require-enabled-feature EnvironmentResourceLoad
        --require-enabled-feature EnvironmentIBL
        --require-enabled-feature DiffuseIBL
        --require-enabled-feature SpecularIBL
        --require-enabled-feature TextureIBL
        --require-enabled-feature ControlledStudioLighting
        --require-enabled-feature BoundsCameraFraming
        --require-resource-diagnostic "pbr materials=125"
        --require-resource-diagnostic "metallic levels=5"
        --require-resource-diagnostic "roughness levels=5"
        --require-resource-diagnostic "base-color families=5"
        --require-resource-diagnostic "matrix shape=5x5x5"
        --require-resource-diagnostic "total material spheres=125"
        --require-resource-diagnostic "matrix x-axis=metallic 0.00,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix y-axis=roughness 0.05,0.25,0.50,0.75,1.00"
        --require-resource-diagnostic "matrix z-axis=base-color terracotta,gold,green,blue,neutral"
        --require-resource-diagnostic "material workflow=factors"
        --require-resource-diagnostic "factor cube materials=125"
        --require-resource-diagnostic "factor cube combinations=125"
        --require-resource-diagnostic "model meshes=1"
        --require-resource-diagnostic "scene mesh instances=125"
        --require-resource-diagnostic "unique mesh resources=1"
        --require-resource-diagnostic "material overrides=125"
        --require-resource-diagnostic "opaque draw packets=125"
    )
    rvx_add_renderverse_pbr_response_validation(metal Metal PBRMaterials)
    rvx_add_renderverse_pbr_texture_smoke(metal Metal)
    rvx_add_renderverse_pbr_workflow_parity(metal Metal)
    rvx_add_renderverse_external_environment_validation(metal Metal)
    rvx_add_renderverse_readiness_smoke(metal Metal)
    rvx_add_renderverse_gpu_driven_parity(metal Metal)
endif()

if(TARGET RenderVerseSamples)
    add_test(
        NAME RenderVerseSamplesUnknownAssetFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample model-rendering
            --asset definitely-unknown
            --backend auto
            --frames 1
    )
    set_tests_properties(RenderVerseSamplesUnknownAssetFailsClosed PROPERTIES
        LABELS "unit;samples;assets"
        TIMEOUT 30
        WILL_FAIL TRUE
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
    )

    add_test(
        NAME RenderVerseSamplesUnsupportedRenderPathFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample model-rendering
            --render-path gpu-driven
            --backend auto
            --frames 1
    )
    set_tests_properties(
        RenderVerseSamplesUnsupportedRenderPathFailsClosed PROPERTIES
        LABELS "unit;samples;policy"
        TIMEOUT 30
        WILL_FAIL TRUE
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
    )

    add_test(
        NAME RenderVerseSamplesMissingModelFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample model-rendering
            --model "${CMAKE_CURRENT_BINARY_DIR}/DefinitelyMissing/model.gltf"
            --backend auto
            --frames 1
    )
    set_tests_properties(RenderVerseSamplesMissingModelFailsClosed PROPERTIES
        LABELS "unit;samples;assets"
        TIMEOUT 30
        WILL_FAIL TRUE
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
    )

    add_test(
        NAME RenderVerseSamplesUnknownEnvironmentFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample pbr-materials
            --environment definitely-unknown
            --backend auto
            --frames 1
    )
    set_tests_properties(RenderVerseSamplesUnknownEnvironmentFailsClosed PROPERTIES
        LABELS "unit;samples;assets"
        TIMEOUT 30
        WILL_FAIL TRUE
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
    )

    add_test(
        NAME RenderVerseSamplesEnvironmentKindMismatchFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample pbr-materials
            --environment pbr-material-grid
            --backend auto
            --frames 1
    )
    set_tests_properties(RenderVerseSamplesEnvironmentKindMismatchFailsClosed PROPERTIES
        LABELS "unit;samples;assets"
        TIMEOUT 30
        WILL_FAIL TRUE
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
    )

    add_test(
        NAME RenderVerseSamplesUnsupportedEnvironmentFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample model-rendering
            --environment pbr-reference-environment
            --backend auto
            --frames 1
    )
    set_tests_properties(RenderVerseSamplesUnsupportedEnvironmentFailsClosed PROPERTIES
        LABELS "unit;samples;assets"
        TIMEOUT 30
        WILL_FAIL TRUE
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
    )

    add_test(
        NAME RenderVerseSamplesMissingEnvironmentFileFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples>
            --sample pbr-materials
            --environment-file
                "${CMAKE_CURRENT_BINARY_DIR}/DefinitelyMissing/environment.exr"
            --backend auto
            --frames 1
    )
    set_tests_properties(RenderVerseSamplesMissingEnvironmentFileFailsClosed PROPERTIES
        LABELS "unit;samples;assets"
        TIMEOUT 30
        WILL_FAIL TRUE
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
    )
endif()

if(RVX_ENABLE_OPENGL AND TARGET RVXEditor)
    set(RVX_EDITOR_SMOKE_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/Editor/EditorViewportSmoke.ppm")
    set(RVX_EDITOR_WORKSPACE_PATH_SMOKE_CWD
        "${CMAKE_CURRENT_BINARY_DIR}/Fixtures/Editor/WorkspacePathSmokeCwd")
    set(RVX_EDITOR_WORKSPACE_PATH_SMOKE_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/Editor/EditorWorkspacePathSmoke.ppm")
    file(MAKE_DIRECTORY "${RVX_EDITOR_WORKSPACE_PATH_SMOKE_CWD}")

    add_test(
        NAME RVXEditorSmoke
        COMMAND $<TARGET_FILE:RVXEditor>
            --smoke
            --frames 20
            --screenshot "${RVX_EDITOR_SMOKE_ACTUAL}"
    )
    set_tests_properties(RVXEditorSmoke PROPERTIES
        LABELS "visual;gpu;editor;opengl"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME RVXEditorSmokeImageValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${RVX_EDITOR_SMOKE_ACTUAL}"
            --min-width 640
            --min-height 360
            --min-unique-colors 32
            --min-luma-range 32
            --near-black-threshold 8
            --max-near-black-ratio 0.98
            --sample-step 4
    )
    set_tests_properties(RVXEditorSmokeImageValidation PROPERTIES
        LABELS "visual;gpu;editor;opengl"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST RVXEditorSmokeImageValidation PROPERTY DEPENDS RVXEditorSmoke)

    add_test(
        NAME RVXEditorWorkspacePathSmoke
        COMMAND $<TARGET_FILE:RVXEditor>
            --smoke
            --frames 8
            --windowed
            --screenshot "${RVX_EDITOR_WORKSPACE_PATH_SMOKE_ACTUAL}"
    )
    set_tests_properties(RVXEditorWorkspacePathSmoke PROPERTIES
        LABELS "visual;gpu;editor;opengl"
        TIMEOUT 120
        WORKING_DIRECTORY "${RVX_EDITOR_WORKSPACE_PATH_SMOKE_CWD}"
    )

    add_test(
        NAME RVXEditorWorkspacePathSmokeImageValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${RVX_EDITOR_WORKSPACE_PATH_SMOKE_ACTUAL}"
            --min-width 640
            --min-height 360
            --min-unique-colors 32
            --min-luma-range 32
            --near-black-threshold 8
            --max-near-black-ratio 0.98
            --sample-step 4
    )
    set_tests_properties(RVXEditorWorkspacePathSmokeImageValidation PROPERTIES
        LABELS "visual;gpu;editor;opengl"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST RVXEditorWorkspacePathSmokeImageValidation
        PROPERTY DEPENDS RVXEditorWorkspacePathSmoke)
endif()

if(TARGET RVXEditor AND TARGET ImageContentValidation)
    function(rvx_add_editor_native_panel_smoke_test test_prefix panel_id artifact_name)
        set(smoke_test "${test_prefix}Smoke")
        set(image_test "${test_prefix}ImageValidation")
        set(smoke_actual "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/Editor/${artifact_name}")

        add_test(
            NAME ${smoke_test}
            COMMAND $<TARGET_FILE:RVXEditor>
                --smoke
                --frames 40
                --ui-backend native
                --automation-scenario open-native-panel
                --automation-target ${panel_id}
                --screenshot "${smoke_actual}"
        )
        set_tests_properties(${smoke_test} PROPERTIES
            LABELS "visual;gpu;editor;native-ui"
            TIMEOUT 120
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME ${image_test}
            COMMAND $<TARGET_FILE:ImageContentValidation>
                --image "${smoke_actual}"
                --min-width 640
                --min-height 360
                --min-unique-colors 8
                --min-luma-range 16
                --near-black-threshold 8
                --max-near-black-ratio 0.99
                --sample-step 4
        )
        set_tests_properties(${image_test} PROPERTIES
            LABELS "visual;gpu;editor;native-ui"
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_property(TEST ${image_test} PROPERTY DEPENDS ${smoke_test})
    endfunction()

    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeViewport
        native.viewport
        NativeViewportSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeInspector
        native.inspector
        NativeInspectorSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeCommandHistory
        native.commandHistory
        NativeCommandHistorySmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeConsole
        native.console
        NativeConsoleSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeMaterialEditor
        native.materialEditor
        NativeMaterialEditorSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeAnimationEditor
        native.animationEditor
        NativeAnimationEditorSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativePanelCatalog
        native.panelCatalog
        NativePanelCatalogSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativeAssetBrowser
        native.assetBrowser
        NativeAssetBrowserSmoke.ppm
    )
    rvx_add_editor_native_panel_smoke_test(
        RVXEditorNativePreferences
        native.preferences
        NativePreferencesSmoke.ppm
    )

    function(rvx_add_editor_native_automation_smoke_test test_prefix scenario artifact_name)
        set(options CONFIRM_OVERWRITE)
        set(oneValueArgs TARGET SETTINGS_PATH SHORTCUT_PROFILE_PATH)
        set(multiValueArgs DEPENDS)
        cmake_parse_arguments(RVX_AUTOMATION
            "${options}"
            "${oneValueArgs}"
            "${multiValueArgs}"
            ${ARGN}
        )

        set(smoke_test "${test_prefix}Smoke")
        set(image_test "${test_prefix}ImageValidation")
        set(smoke_actual "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/Editor/${artifact_name}")
        set(extra_args "")

        if(RVX_AUTOMATION_TARGET)
            list(APPEND extra_args
                --automation-target "${RVX_AUTOMATION_TARGET}"
            )
        endif()
        if(RVX_AUTOMATION_SETTINGS_PATH)
            list(APPEND extra_args
                --settings-path "${RVX_AUTOMATION_SETTINGS_PATH}"
            )
        endif()
        if(RVX_AUTOMATION_SHORTCUT_PROFILE_PATH)
            list(APPEND extra_args
                --auto-shortcut-profile-path "${RVX_AUTOMATION_SHORTCUT_PROFILE_PATH}"
            )
        endif()
        if(RVX_AUTOMATION_CONFIRM_OVERWRITE)
            list(APPEND extra_args
                --automation-confirm-overwrite
            )
        endif()

        add_test(
            NAME ${smoke_test}
            COMMAND $<TARGET_FILE:RVXEditor>
                --smoke
                --frames 40
                --ui-backend native
                --automation-scenario ${scenario}
                ${extra_args}
                --screenshot "${smoke_actual}"
        )
        set_tests_properties(${smoke_test} PROPERTIES
            LABELS "visual;gpu;editor;native-ui;automation"
            TIMEOUT 120
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        if(RVX_AUTOMATION_DEPENDS)
            set_property(TEST ${smoke_test} APPEND PROPERTY
                DEPENDS ${RVX_AUTOMATION_DEPENDS}
            )
        endif()

        add_test(
            NAME ${image_test}
            COMMAND $<TARGET_FILE:ImageContentValidation>
                --image "${smoke_actual}"
                --min-width 640
                --min-height 360
                --min-unique-colors 8
                --min-luma-range 16
                --near-black-threshold 8
                --max-near-black-ratio 0.99
                --sample-step 4
        )
        set_tests_properties(${image_test} PROPERTIES
            LABELS "visual;gpu;editor;native-ui;automation"
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_property(TEST ${image_test} PROPERTY DEPENDS ${smoke_test})
    endfunction()

    set(RVX_EDITOR_NATIVE_AUTOMATION_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/EditorAutomation/$<CONFIG>")
    set(RVX_EDITOR_NATIVE_FILE_PICKER_SCENE
        "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/NativeFilePickerScene.rvxscene")
    set(RVX_EDITOR_NATIVE_SHORTCUT_SETTINGS
        "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/ShortcutAutosave/EditorSettings.json")
    set(RVX_EDITOR_NATIVE_SHORTCUT_PROFILE
        "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/ShortcutAutosave/EditorShortcuts.shortcuts")
    set(RVX_EDITOR_NATIVE_UI_SCALE_SETTINGS
        "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/UIScale/EditorSettings.json")

    add_test(
        NAME RVXEditorNativeAutomationDirectoryPrepare
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}"
            "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/ShortcutAutosave"
            "${RVX_EDITOR_NATIVE_AUTOMATION_DIR}/UIScale"
    )
    set_tests_properties(RVXEditorNativeAutomationDirectoryPrepare PROPERTIES
        LABELS "editor;native-ui;automation"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME RVXEditorNativeFilePickerOverwriteFixture
        COMMAND ${CMAKE_COMMAND} -E touch
            "${RVX_EDITOR_NATIVE_FILE_PICKER_SCENE}"
    )
    set_tests_properties(RVXEditorNativeFilePickerOverwriteFixture PROPERTIES
        LABELS "editor;native-ui;automation"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST RVXEditorNativeFilePickerOverwriteFixture PROPERTY
        DEPENDS RVXEditorNativeAutomationDirectoryPrepare
    )

    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeCommandSurfaceClick
        command-surface-click
        NativeCommandSurfaceClickSmoke.ppm
        TARGET "file:file.newScene"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeGameObjectCreateEmptyClick
        command-surface-click
        NativeGameObjectCreateEmptyClickSmoke.ppm
        TARGET "gameObject:gameObject.createEmpty"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeCommandMenuOpen
        command-menu-open
        NativeCommandMenuOpenSmoke.ppm
        TARGET "file"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeViewMenuOpen
        command-menu-open
        NativeViewMenuOpenSmoke.ppm
        TARGET "view"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeCommandPaletteOpen
        command-palette-open
        NativeCommandPaletteOpenSmoke.ppm
        TARGET "command palette"
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeSaveSceneFilePicker
        save-scene-as-file-picker
        NativeSaveSceneFilePickerSmoke.ppm
        TARGET "${RVX_EDITOR_NATIVE_FILE_PICKER_SCENE}"
        CONFIRM_OVERWRITE
        DEPENDS RVXEditorNativeFilePickerOverwriteFixture
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeOpenSceneFilePicker
        open-scene-file-picker
        NativeOpenSceneFilePickerSmoke.ppm
        TARGET "${RVX_EDITOR_NATIVE_FILE_PICKER_SCENE}"
        DEPENDS RVXEditorNativeSaveSceneFilePickerSmoke
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeShortcutAutosave
        shortcut-autosave-write
        NativeShortcutAutosaveSmoke.ppm
        SETTINGS_PATH "${RVX_EDITOR_NATIVE_SHORTCUT_SETTINGS}"
        SHORTCUT_PROFILE_PATH "${RVX_EDITOR_NATIVE_SHORTCUT_PROFILE}"
        DEPENDS RVXEditorNativeAutomationDirectoryPrepare
    )
    rvx_add_editor_native_automation_smoke_test(
        RVXEditorNativeUIScaleAdjust
        ui-scale-adjust
        NativeUIScaleAdjustSmoke.ppm
        SETTINGS_PATH "${RVX_EDITOR_NATIVE_UI_SCALE_SETTINGS}"
        DEPENDS RVXEditorNativeAutomationDirectoryPrepare
    )
endif()

# Cross-Backend Validation Tests (requires both DX12 and Vulkan)
if(RVX_ENABLE_DX12 AND RVX_ENABLE_VULKAN)
    rvx_add_gtest(
        NAME CrossBackendValidation
        SOURCES CrossBackendValidation/main.cpp
        LIBS
            RVX::Core
            RVX::RenderGraph
            RVX::RHI
            RVX::RHI_BackendFactory
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
        LABELS gpu
        TIMEOUT 180
    )
endif()

function(rvx_add_renderverse_p0_cross_backend_parity SAMPLE_LABEL)
    if(NOT WIN32 OR
       NOT RVX_ENABLE_DX12 OR
       NOT RVX_ENABLE_VULKAN OR
       NOT TARGET VisualGoldenValidation OR
       NOT TARGET RenderVerseSamples)
        return()
    endif()

    foreach(path_label Direct GPUDriven)
        set(dx12_capture
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesDX12${SAMPLE_LABEL}${path_label}_320x180.ppm")
        set(vulkan_capture
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesVulkan${SAMPLE_LABEL}${path_label}_320x180.ppm")
        set(parity_prefix
            "RenderVerseSamplesCrossBackend${SAMPLE_LABEL}${path_label}ParityValidation")
        add_test(
            NAME ${parity_prefix}
            COMMAND $<TARGET_FILE:VisualGoldenValidation>
                --expected "${dx12_capture}"
                --actual "${vulkan_capture}"
                --diff "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${parity_prefix}_DX12_Vulkan_diff.ppm"
                --report "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${parity_prefix}_DX12_Vulkan.json"
                --tolerance 0.0
                --max-different-pixels 0)
        set_tests_properties(${parity_prefix} PROPERTIES
            LABELS "visual;gpu;sample;primary;parity;cross-backend;dx12;vulkan"
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            DEPENDS "RenderVerseSamplesDX12${SAMPLE_LABEL}${path_label}Smoke;RenderVerseSamplesVulkan${SAMPLE_LABEL}${path_label}Smoke"
            FIXTURES_REQUIRED
                "RenderVerseSamplesDX12${SAMPLE_LABEL}${path_label}FreshCapture;RenderVerseSamplesVulkan${SAMPLE_LABEL}${path_label}FreshCapture")
    endforeach()
endfunction()

function(rvx_add_renderverse_gpu_driven_cross_backend_parity)
    if(NOT RVX_ENABLE_P0_QUALIFICATION_MATRIX)
        return()
    endif()
    if(NOT WIN32 OR NOT RVX_ENABLE_DX12 OR NOT RVX_ENABLE_VULKAN)
        message(FATAL_ERROR
            "The gpu-driven cross-backend qualification requires Windows DX12 and Vulkan")
    endif()
    foreach(required_target VisualGoldenValidation RenderVerseSamples)
        if(NOT TARGET ${required_target})
            message(FATAL_ERROR
                "RVX_ENABLE_P0_QUALIFICATION_MATRIX requires ${required_target} for gpu-driven cross-backend parity")
        endif()
    endforeach()

    foreach(path_label GPU Direct)
        set(dx12_capture
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesDX12GPUDriven_${path_label}_320x180.ppm")
        set(vulkan_capture
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesVulkanGPUDriven_${path_label}_320x180.ppm")
        set(parity_prefix
            "RenderVerseSamplesCrossBackendGPUDriven${path_label}ParityValidation")
        add_test(
            NAME ${parity_prefix}
            COMMAND $<TARGET_FILE:VisualGoldenValidation>
                --expected "${dx12_capture}"
                --actual "${vulkan_capture}"
                --diff "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${parity_prefix}_DX12_Vulkan_diff.ppm"
                --report "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${parity_prefix}_DX12_Vulkan.json"
                --tolerance 0.0
                --max-different-pixels 0)
        set_tests_properties(${parity_prefix} PROPERTIES
            LABELS "visual;gpu;sample;primary;gpu-driven;parity;cross-backend;dx12;vulkan"
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            DEPENDS "RenderVerseSamplesDX12GPUDriven${path_label}Smoke;RenderVerseSamplesVulkanGPUDriven${path_label}Smoke"
            FIXTURES_REQUIRED
                "RenderVerseSamplesDX12GPUDriven${path_label}FreshCapture;RenderVerseSamplesVulkanGPUDriven${path_label}FreshCapture")
    endforeach()
endfunction()

if(WIN32
    AND RVX_ENABLE_DX12
    AND RVX_ENABLE_VULKAN
    AND RVX_ENABLE_P0_QUALIFICATION_MATRIX
    AND TARGET VisualGoldenValidation
    AND TARGET RenderVerseSamples)
    rvx_add_renderverse_p0_cross_backend_parity(SceneLifecycle)
    rvx_add_renderverse_p0_cross_backend_parity(AssetStreaming)
    rvx_add_renderverse_p0_cross_backend_parity(InteriorRendering)
    rvx_add_renderverse_p0_cross_backend_parity(ModelRendering)
    rvx_add_renderverse_p0_cross_backend_parity(AssetGallery)
    rvx_add_renderverse_p0_cross_backend_parity(LightingShadows)
    rvx_add_renderverse_p0_cross_backend_parity(RenderPipeline)
    rvx_add_renderverse_p0_cross_backend_parity(ModelViewer)
    rvx_add_renderverse_p0_cross_backend_parity(PBRMaterials)
    if(RVX_ENABLE_P1_QUALIFICATION_MATRIX)
        rvx_add_renderverse_p0_cross_backend_parity(AnimationCharacter)
    endif()
    set(RVX_RENDERING_STRESS_DX12_DIRECT_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesDX12RenderingStressDirect_320x180.ppm")
    set(RVX_RENDERING_STRESS_VULKAN_DIRECT_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesVulkanRenderingStressDirect_320x180.ppm")
    set(RVX_RENDERING_STRESS_DX12_GPU_DRIVEN_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesDX12RenderingStressGPUDriven_320x180.ppm")
    set(RVX_RENDERING_STRESS_VULKAN_GPU_DRIVEN_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesVulkanRenderingStressGPUDriven_320x180.ppm")

    add_test(
        NAME RenderVerseSamplesCrossBackendRenderingStressDirectParityValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_RENDERING_STRESS_DX12_DIRECT_ACTUAL}"
            --actual "${RVX_RENDERING_STRESS_VULKAN_DIRECT_ACTUAL}"
            --diff "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesRenderingStressDirect_DX12_Vulkan_320x180.diff.ppm"
            --report "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesRenderingStressDirect_DX12_Vulkan_320x180.json"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(RenderVerseSamplesCrossBackendRenderingStressDirectParityValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;parity;cross-backend;dx12;vulkan"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS "RenderVerseSamplesDX12RenderingStressDirectSmoke;RenderVerseSamplesVulkanRenderingStressDirectSmoke"
        FIXTURES_REQUIRED
            "RenderVerseSamplesDX12RenderingStressDirectFreshCapture;RenderVerseSamplesVulkanRenderingStressDirectFreshCapture"
    )

    add_test(
        NAME RenderVerseSamplesCrossBackendRenderingStressGPUDrivenParityValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_RENDERING_STRESS_DX12_GPU_DRIVEN_ACTUAL}"
            --actual "${RVX_RENDERING_STRESS_VULKAN_GPU_DRIVEN_ACTUAL}"
            --diff "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesRenderingStressGPUDriven_DX12_Vulkan_320x180.diff.ppm"
            --report "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/RenderVerseSamplesRenderingStressGPUDriven_DX12_Vulkan_320x180.json"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(RenderVerseSamplesCrossBackendRenderingStressGPUDrivenParityValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;parity;cross-backend;dx12;vulkan"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS "RenderVerseSamplesDX12RenderingStressGPUDrivenSmoke;RenderVerseSamplesVulkanRenderingStressGPUDrivenSmoke"
        FIXTURES_REQUIRED
            "RenderVerseSamplesDX12RenderingStressGPUDrivenFreshCapture;RenderVerseSamplesVulkanRenderingStressGPUDrivenFreshCapture"
    )
endif()

rvx_add_renderverse_gpu_driven_cross_backend_parity()

if(WIN32
    AND RVX_ENABLE_DX12
    AND RVX_ENABLE_VULKAN
    AND RVX_ENABLE_P1_QUALIFICATION_MATRIX
    AND TARGET VisualGoldenValidation
    AND TARGET RenderVerseSamples)
    rvx_add_renderverse_p0_cross_backend_parity(PhysicsSandbox)
endif()

# Copy test shaders
file(GLOB TEST_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsl")
foreach(SHADER ${TEST_SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    configure_file(${SHADER} ${CMAKE_CURRENT_BINARY_DIR}/Shaders/${SHADER_NAME} COPYONLY)
endforeach()

get_property(RVX_VALIDATION_EXECUTABLE_TARGETS GLOBAL
    PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
)

foreach(RVX_EXTERNAL_TEST_TARGET IN ITEMS
    RVXCook
)
    if(TARGET ${RVX_EXTERNAL_TEST_TARGET})
        list(APPEND RVX_VALIDATION_EXECUTABLE_TARGETS
            ${RVX_EXTERNAL_TEST_TARGET}
        )
    endif()
endforeach()

list(REMOVE_DUPLICATES RVX_VALIDATION_EXECUTABLE_TARGETS)

add_custom_target(RVXValidationInventory
    DEPENDS ${RVX_VALIDATION_EXECUTABLE_TARGETS}
)
