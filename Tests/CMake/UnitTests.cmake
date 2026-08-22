# Deterministic behavioural validation of isolated runtime modules.
set(RVX_VISUAL_GOLDEN_REPORT_SELFTEST_IMAGE
    "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/R7_DX11_320x180.ppm")
if(EXISTS "${RVX_VISUAL_GOLDEN_REPORT_SELFTEST_IMAGE}")
    rvx_add_validation_test(
        LANE unit
        NAME VisualGoldenValidationReportSelfTest
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_VISUAL_GOLDEN_REPORT_SELFTEST_IMAGE}"
            --actual "${RVX_VISUAL_GOLDEN_REPORT_SELFTEST_IMAGE}"
            --report "${CMAKE_CURRENT_BINARY_DIR}/VisualGoldenValidationReportSelfTest.json"
            --tolerance 0.0
            --max-different-pixels 0
        BUILD_TARGETS VisualGoldenValidation
        TAGS subsystem.validation suite.visual-golden
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()
rvx_add_gtest(
    NAME EcsEntityValidation
    LANE unit
    SOURCES EcsEntityValidation/main.cpp
    LIBS RVX::ECS
    TAGS subsystem.ecs
    TIMEOUT 30
)

if(TARGET SampleAssetCatalogValidation)
    target_compile_definitions(SampleAssetCatalogValidation PRIVATE
        RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

rvx_add_gtest(
    NAME EcsFragmentQueryValidation
    LANE unit
    SOURCES EcsFragmentQueryValidation/main.cpp
    LIBS RVX::ECS
    TAGS subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsCommandTransactionValidation
    LANE unit
    SOURCES EcsCommandTransactionValidation/main.cpp
    LIBS RVX::ECS
    TAGS subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSchedulerValidation
    LANE unit
    SOURCES EcsSchedulerValidation/main.cpp
    LIBS RVX::ECS
    TAGS subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsTransformHierarchyValidation
    LANE unit
    SOURCES EcsTransformHierarchyValidation/main.cpp
    LIBS RVX::Scene
    TAGS subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsCleanupValidation
    LANE unit
    SOURCES EcsCleanupValidation/main.cpp
    LIBS RVX::Scene
    TAGS subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsChangeTrackingValidation
    LANE unit
    SOURCES EcsChangeTrackingValidation/main.cpp
    LIBS RVX::Scene
    TAGS subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSpatialIndexValidation
    LANE unit
    SOURCES EcsSpatialIndexValidation/main.cpp
    LIBS RVX::Scene
    TAGS subsystem.scene subsystem.ecs suite.spatial
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsPhysicsValidation
    LANE unit
    SOURCES EcsPhysicsValidation/main.cpp
    LIBS RVX::PhysicsSceneAdapters
    TAGS subsystem.scene subsystem.ecs subsystem.physics
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsAnimationValidation
    LANE unit
    SOURCES EcsAnimationValidation/main.cpp
    LIBS RVX::AnimationSceneAdapters
    TAGS subsystem.scene subsystem.ecs subsystem.animation
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsResourceAnimationEvaluatorValidation
    LANE unit
    SOURCES EcsResourceAnimationEvaluatorValidation/main.cpp
    LIBS RVX::AnimationSceneAdapters
    TAGS subsystem.resource subsystem.scene subsystem.ecs subsystem.animation
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsAnimationAssetServiceValidation
    LANE unit
    SOURCES EcsAnimationAssetServiceValidation/main.cpp
    LIBS
        RVX::AnimationSceneAdapters
        RVX::Audio
        RVX::Engine
        RVX::PhysicsSceneAdapters
        RVX::ResourceSceneAdapters
        RVX::Scripting
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Engine/Private/ECS
    TAGS subsystem.resource subsystem.scene subsystem.ecs subsystem.animation
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsAudioValidation
    LANE unit
    SOURCES EcsAudioValidation/main.cpp
    LIBS RVX::Audio
    TAGS subsystem.scene subsystem.ecs subsystem.audio
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsScriptingValidation
    LANE unit
    SOURCES EcsScriptingValidation/main.cpp
    LIBS RVX::Scripting
    TAGS subsystem.scripting subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneSnapshotValidation
    LANE unit
    SOURCES EcsSceneSnapshotValidation/main.cpp
    LIBS RVX::Scene
    TAGS subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSkinningSnapshotValidation
    LANE unit
    SOURCES EcsSkinningSnapshotValidation/main.cpp
    LIBS
        RVX::AnimationSceneAdapters
        RVX::RenderExtraction
    TAGS subsystem.render subsystem.scene subsystem.ecs subsystem.animation suite.skinning
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneOrchestrationValidation
    LANE unit
    SOURCES EcsSceneOrchestrationValidation/main.cpp
    LIBS RVX::Scene
    TAGS subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneSpawnTransactionValidation
    LANE unit
    SOURCES EcsSceneSpawnTransactionValidation/main.cpp
    LIBS RVX::Scene
    TAGS subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsCameraServiceValidation
    LANE unit
    SOURCES EcsCameraServiceValidation/main.cpp
    LIBS RVX::World
    TAGS subsystem.world subsystem.scene subsystem.ecs suite.camera
    TIMEOUT 30
)

rvx_add_gtest(
    NAME WorldEcsAuthorityValidation
    LANE unit
    SOURCES WorldEcsAuthorityValidation/main.cpp
    LIBS RVX::World
    TAGS subsystem.world subsystem.scene subsystem.ecs suite.authority
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsFrozenSceneBridgeValidation
    LANE unit
    SOURCES EcsFrozenSceneBridgeValidation/main.cpp
    LIBS RVX::RenderExtraction
    TAGS subsystem.render subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsFrameExtractionValidation
    LANE unit
    SOURCES EcsFrameExtractionValidation/main.cpp
    LIBS RVX::RenderExtraction
    TAGS subsystem.render subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsFeatureSnapshotValidation
    LANE unit
    SOURCES EcsFeatureSnapshotValidation/main.cpp
    LIBS RVX::RenderExtraction
    TAGS subsystem.render subsystem.scene subsystem.ecs suite.feature
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsModelBatchValidation
    LANE unit
    SOURCES EcsModelBatchValidation/main.cpp
    LIBS RVX::ResourceSceneAdapters
    TAGS subsystem.resource subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneAssetLoadCoordinatorValidation
    LANE unit
    SOURCES EcsSceneAssetLoadCoordinatorValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ResourceRuntime
        RVX::ResourceSceneAdapters
        RVX::Scene
    TAGS subsystem.resource subsystem.scene subsystem.ecs
    TIMEOUT 30
)

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME EcsSampleModelLoaderValidation
        LANE unit
        SOURCES EcsSampleModelLoaderValidation/main.cpp
        LIBS
            RVX::SampleCommon
            RVX::Scene
        TAGS product.samples subsystem.resource subsystem.scene subsystem.ecs
        TIMEOUT 30
    )
endif()

rvx_add_gtest(
    NAME EcsEnvironmentLoadCoordinatorValidation
    LANE unit
    SOURCES EcsEnvironmentLoadCoordinatorValidation/main.cpp
    LIBS
        RVX::AssetImport
        RVX::Core
        RVX::ResourceRuntime
        RVX::ResourceSceneAdapters
        RVX::Scene
    TAGS subsystem.resource subsystem.scene subsystem.ecs suite.environment
    TIMEOUT 30
)

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME EcsSampleEnvironmentLoaderValidation
        LANE unit
        SOURCES EcsSampleEnvironmentLoaderValidation/main.cpp
        LIBS
            RVX::AssetImport
            RVX::Core
            RVX::SampleCommon
        TAGS product.samples subsystem.resource subsystem.scene subsystem.ecs suite.environment
        TIMEOUT 30
    )
endif()

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME EcsSampleAnimationLoaderValidation
        LANE unit
        SOURCES EcsSampleAnimationLoaderValidation/main.cpp
        LIBS
            RVX::Core
            RVX::SampleCommon
        TAGS product.samples subsystem.resource subsystem.scene subsystem.ecs subsystem.animation
        TIMEOUT 30
    )
endif()

rvx_add_gtest(
    NAME EcsDynamicBufferValidation
    LANE unit
    SOURCES EcsDynamicBufferValidation/main.cpp
    LIBS RVX::ECS
    TAGS subsystem.ecs suite.storage
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSharedDataValidation
    LANE unit
    SOURCES EcsSharedDataValidation/main.cpp
    LIBS RVX::ECS
    TAGS subsystem.ecs suite.storage
    TIMEOUT 30
)

rvx_add_gtest(
    NAME CoreDebugConfigValidation
    LANE unit
    SOURCES CoreDebugConfigValidation/main.cpp
    LIBS RVX::Core
    DEFINES $<$<CONFIG:Debug>:RVX_EXPECT_DEBUG_CONFIG=1>
    TIMEOUT 30
)

rvx_add_gtest(
    NAME CorePathValidation
    LANE unit
    SOURCES CorePathValidation/main.cpp
    LIBS RVX::Core
    TIMEOUT 30
)

rvx_add_gtest(
    NAME StartupTimelineValidation
    LANE unit
    SOURCES StartupTimelineValidation/main.cpp
    LIBS RVX::Core
    TAGS trait.diagnostics suite.startup
    TIMEOUT 30
)

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME ModelCameraFramingValidation
        LANE unit
        SOURCES ModelCameraFramingValidation/main.cpp
        LIBS RVX::SampleCommon
        TAGS product.samples suite.assets
        TIMEOUT 30
    )
endif()

rvx_add_gtest(
    NAME RenderContractsValidation
    LANE unit
    SOURCES RenderContractsValidation/main.cpp
    LIBS RVX::Core RVX::RenderContracts RVX::RenderExtraction
    INCLUDES ${CMAKE_SOURCE_DIR}/Source/Runtime/RenderContracts/Private
    TIMEOUT 60
)

rvx_add_gtest(
    NAME RenderPolicyValidation
    LANE unit
    SOURCES RenderPolicyValidation/main.cpp
    LIBS RVX::Core RVX::RenderContracts RVX::RHI RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
    TAGS suite.render-policy
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderVisibilityValidation
    LANE unit
    SOURCES RenderVisibilityValidation/main.cpp
    LIBS RVX::Core RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
    TAGS suite.visibility
    TIMEOUT 30
)

rvx_add_gtest(
    NAME GPUSceneValidation
    LANE unit
    SOURCES GPUSceneValidation/main.cpp
    LIBS RVX::Render RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
    TAGS suite.gpu-scene
    TIMEOUT 30
)

rvx_add_gtest(
    NAME GPUSceneSchemaValidation
    LANE unit
    SOURCES GPUSceneSchemaValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TAGS suite.gpu-scene
    TIMEOUT 30
)

rvx_add_gtest(
    NAME GPUSceneUploadValidation
    LANE unit
    SOURCES GPUSceneUploadValidation/main.cpp
    LIBS RVX::Render RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
    TAGS suite.gpu-scene
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderDrawPacketValidation
    LANE unit
    SOURCES RenderDrawPacketValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderDrawPacketCacheValidation
    LANE unit
    SOURCES RenderDrawPacketCacheValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderConcurrencyValidation
    LANE unit
    SOURCES RenderConcurrencyValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX_RenderRuntimeCore
    INCLUDES ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
    TIMEOUT 60
)

rvx_add_gtest(
    NAME RenderExecutorValidation
    LANE unit
    SOURCES RenderExecutorValidation/main.cpp
    LIBS
        RVX_RenderTestSupport
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
    TIMEOUT 60
)

rvx_add_gtest(
    NAME JobGraphValidation
    LANE unit
    SOURCES JobGraphValidation/main.cpp
    LIBS RVX::Core
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RHIContractValidation
    LANE unit
    SOURCES RHIContractValidation/main.cpp
    LIBS RVX::RHI
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME MeshPassProcessorValidation
    LANE unit
    SOURCES MeshPassProcessorValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderInstanceBatchPlanValidation
    LANE unit
    SOURCES RenderInstanceBatchPlanValidation/main.cpp
    LIBS RVX::Render
    INCLUDES ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
    TIMEOUT 60
)

rvx_add_gtest(
    NAME RHIConformanceValidation
    LANE unit
    SOURCES RHIConformanceValidation/main.cpp
    LIBS RVX::RHI
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RHIPipelineValidation
    LANE unit
    SOURCES RHIPipelineValidation/main.cpp
    LIBS RVX::RHI
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderSubmissionValidation
    LANE unit
    SOURCES RenderSubmissionValidation/main.cpp
    LIBS
        RVX::Render
        RVX::RHI
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderLifetimeCutoverValidation
    LANE unit
    SOURCES RenderLifetimeCutoverValidation/main.cpp
    LIBS
        RVX::Render
        RVX::RHI
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
    TIMEOUT 30
)

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME SampleCLIValidation
        LANE unit
        SOURCES SampleCLIValidation/main.cpp
        LIBS RVX::SampleCommon
        TIMEOUT 30
    )
endif()

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME SampleRenderPathPolicyValidation
        LANE unit
        SOURCES SampleRenderPathPolicyValidation/main.cpp
        LIBS RVX::SampleCommon
        TAGS product.samples suite.render-policy
        TIMEOUT 30
    )
endif()

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME SampleRegistryValidation
        LANE unit
        SOURCES SampleRegistryValidation/main.cpp
        LIBS RVX::SampleCommon
        TAGS product.samples
        TIMEOUT 30
    )
endif()

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME SampleManifestValidation
        LANE unit
        SOURCES SampleManifestValidation/main.cpp
        LIBS RVX::SampleCommon
        TAGS product.samples suite.contracts
        TIMEOUT 30
    )
endif()

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME SampleAssetCatalogValidation
        LANE unit
        SOURCES SampleAssetCatalogValidation/main.cpp
        LIBS RVX::AssetImport RVX::SampleCommon
        TAGS product.samples suite.assets
        TIMEOUT 30
    )
    target_compile_definitions(SampleAssetCatalogValidation PRIVATE
        RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

rvx_add_gtest(
    NAME RenderGraphValidation
    LANE unit
    SOURCES RenderGraphValidation/main.cpp
    LIBS RVX::RenderGraph
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
    TIMEOUT 120
)

rvx_add_gtest(
    NAME RenderResourceRuntimeValidation
    LANE unit
    SOURCES RenderResourceRuntimeValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::RenderContracts
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RenderContracts/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI/Include
    TIMEOUT 120
)

rvx_add_gtest(
    NAME ImageCompareValidation
    LANE unit
    SOURCES ImageCompareValidation/main.cpp
    LIBS
        RVX_TestCommon
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    TIMEOUT 30
)

rvx_add_gtest(
    NAME HDRTextureLoaderValidation
    LANE unit
    SOURCES HDRTextureLoaderValidation/main.cpp
    LIBS
        RVX::AssetImport
        RVX::Core
        RVX::ResourceRuntime
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/ResourceRuntime/Include
    TIMEOUT 120
)

rvx_add_gtest(
    NAME ShaderCompilerValidation
    LANE unit
    SOURCES ShaderCompilerValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ShaderCompiler
        RVX::RHI
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Developer/ShaderCompiler/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI/Include
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

rvx_add_gtest(
    NAME ShaderArtifactValidation
    LANE unit
    SOURCES ShaderArtifactValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::ShaderContracts
        RVX::ShaderRuntime
    TAGS subsystem.render suite.shader-artifact
    TIMEOUT 30
)

rvx_add_gtest(
    NAME ShaderProgramCatalogValidation
    LANE unit
    SOURCES ShaderProgramCatalogValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ShaderCompiler
        RVX::ShaderContracts
        RVX::AssetPipeline
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Tests
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TAGS subsystem.render suite.shader-artifact
    TIMEOUT 60
)

rvx_add_gtest(
    NAME PipelineCacheValidation
    LANE unit
    SOURCES PipelineCacheValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::RHI
        RVX::ShaderContracts
        RVX::AssetPipeline
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Tests
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI/Include
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

rvx_add_gtest(
    NAME GPUUploadServiceValidation
    LANE unit
    SOURCES GPUUploadServiceValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::RenderContracts
        RVX::Render
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RenderContracts/Include
    TIMEOUT 120
)

rvx_add_gtest(
    NAME GPUDrivenValidation
    LANE unit
    SOURCES GPUDrivenValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::RenderContracts
        RVX::Render
        RVX::ShaderCompiler
        RVX::ShaderContracts
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Tests
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RenderContracts/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI/Include
        ${CMAKE_SOURCE_DIR}/Source/Developer/ShaderCompiler/Include
    TIMEOUT 120
)

rvx_add_gtest(
    NAME SkeletalAssetPipelineValidation
    LANE unit
    SOURCES SkeletalAssetPipelineValidation/main.cpp
    LIBS
        RVX::AssetImport
        RVX::Core
        RVX::ResourceRuntime
        RVX::AssetPipeline
    TAGS subsystem.resource subsystem.animation suite.gltf
    TIMEOUT 120
)

rvx_add_gtest(
    NAME AnimationResourceValidation
    LANE unit
    SOURCES AnimationResourceValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ResourceRuntime
    TAGS subsystem.resource subsystem.animation suite.cooked
    TIMEOUT 120
)

rvx_add_gtest(
    NAME AnimationRootMotionDerivationValidation
    LANE unit
    SOURCES AnimationRootMotionDerivationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ResourceRuntime
        RVX::AssetPipeline
    DEFINES
        RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TAGS product.tools subsystem.resource subsystem.animation suite.root-motion
    TIMEOUT 120
)

rvx_add_gtest(
    NAME RenderResourceReplacementValidation
    LANE unit
    SOURCES RenderResourceReplacementValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::RenderContracts
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RenderContracts/Include
    TAGS subsystem.render subsystem.resource
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderIncrementalSceneScaleValidation
    LANE unit
    SOURCES RenderIncrementalSceneScaleValidation/main.cpp
    LIBS RVX::Render RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
    TAGS trait.performance
    TIMEOUT 180
)

rvx_add_gtest(
    NAME RenderSceneUpdateQueueValidation
    LANE unit
    SOURCES RenderSceneUpdateQueueValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RenderContracts/Include
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderSceneRevisionValidation
    LANE unit
    SOURCES RenderSceneRevisionValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX::Render
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RenderContracts/Include
    TIMEOUT 30
)

rvx_add_gtest(
    NAME MaterialSystemValidation
    LANE unit
    SOURCES
        MaterialSystemValidation/main.cpp
        Common/RenderRuntimeTestHarness.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::ResourceRuntime
        RVX::RHI
        RVX::Scene
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Tests
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
        ${CMAKE_SOURCE_DIR}/Source/Runtime/ResourceRuntime/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI/Include
    TIMEOUT 120
)

rvx_add_gtest(
    NAME ClusteredLightingValidation
    LANE unit
    SOURCES ClusteredLightingValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::RHI
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI/Include
    TIMEOUT 120
)

rvx_add_gtest(
    NAME UIValidation
    LANE unit
    SOURCES UIValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::Render
        RVX::UI
    TIMEOUT 120
)

rvx_add_gtest(
    NAME UIRenderPacketValidation
    LANE unit
    SOURCES UIRenderPacketValidation/main.cpp
    LIBS RVX::RenderContracts
    TIMEOUT 30
)

rvx_add_gtest(
    NAME AudioResourceValidation
    LANE unit
    SOURCES AudioResourceValidation/main.cpp
    LIBS
        RVX::AssetImport
        RVX::Core
        RVX::ResourceRuntime
    TIMEOUT 120
)

rvx_add_gtest(
    NAME ResourceDiagnosticsViewValidation
    LANE unit
    SOURCES ResourceDiagnosticsViewValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ResourceRuntime
    TAGS subsystem.resource trait.diagnostics
    TIMEOUT 30
)

rvx_add_gtest(
    NAME ResourceRuntimePolicyValidation
    LANE unit
    SOURCES ResourceRuntimePolicyValidation/main.cpp
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    LIBS
        RVX::Core
        RVX::ResourceRuntime
        RVX::ShaderContracts
    TIMEOUT 120
)

rvx_add_gtest(
    NAME ResourceLoaderCatalogValidation
    LANE unit
    SOURCES ResourceLoaderCatalogValidation/main.cpp
    LIBS
        RVX::AssetContracts
        RVX::ResourceRuntime
    TAGS subsystem.resource suite.loader-catalog
    TIMEOUT 30
)

rvx_add_gtest(
    NAME ResourceSourceValidation
    LANE unit
    SOURCES ResourceSourceValidation/main.cpp
    LIBS
        RVX::AssetImport
        RVX::EngineDevelopmentComposition
        RVX::ResourceRuntime
        RVX::ResourceSource
    TAGS subsystem.resource suite.source-composition
    TIMEOUT 60
)

rvx_add_gtest(
    NAME ResourceLoadOperationValidation
    LANE unit
    SOURCES ResourceLoadOperationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ResourceRuntime
    TAGS subsystem.resource trait.async
    TIMEOUT 30
)

rvx_add_gtest(
    NAME ResourceLoadedAcquireValidation
    LANE unit
    SOURCES ResourceLoadedAcquireValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ResourceRuntime
    TAGS subsystem.resource trait.concurrency
    TIMEOUT 30
)

rvx_add_gtest(
    NAME CookManifestValidation
    LANE unit
    SOURCES CookManifestValidation/main.cpp
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    LIBS
        RVX::Core
        RVX::ResourceRuntime
    TAGS subsystem.resource suite.cook
    TIMEOUT 30
)

rvx_add_gtest(
    NAME ResourcePreparedLoadingValidation
    LANE unit
    SOURCES ResourcePreparedLoadingValidation/main.cpp
    LIBS
        RVX::AssetImport
        RVX::Core
        RVX::ResourceRuntime
    TAGS subsystem.resource trait.async
    TIMEOUT 60
)

rvx_add_gtest(
    NAME ResourceRetirementLedgerValidation
    LANE unit
    SOURCES ResourceRetirementLedgerValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ResourceRuntime
    TAGS subsystem.resource suite.retirement
    TIMEOUT 30
)

rvx_add_gtest(
    NAME MaterialInstanceResourceValidation
    LANE unit
    SOURCES MaterialInstanceResourceValidation/main.cpp
    LIBS
        RVX::Core
        RVX::ResourceRuntime
        RVX::Scene
    TAGS subsystem.resource suite.material
    TIMEOUT 30
)

rvx_add_gtest(
    NAME SceneAssetStreamingValidation
    LANE unit
    SOURCES SceneAssetStreamingValidation/main.cpp
    LIBS RVX::AssetImport RVX::Core RVX::ResourceRuntime RVX::ResourceSource
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TAGS subsystem.resource suite.streaming
    TIMEOUT 30
)

rvx_add_gtest(
    NAME CookedModelProductValidation
    LANE unit
    SOURCES CookedModelProductValidation/main.cpp
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
    LIBS RVX::AssetImport RVX::Core RVX::ResourceRuntime RVX::AssetPipeline
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TAGS subsystem.resource suite.cook suite.model
    TIMEOUT 120
)

if(RVX_ENABLE_FBX)
    rvx_add_gtest(
        NAME FBXImporterValidation
        LANE unit
        SOURCES FBXImporterValidation/main.cpp
        LIBS RVX::AssetImport RVX::AssetPipeline RVX::Core RVX::Geometry
        DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        TAGS subsystem.resource suite.fbx trait.source-import
        TIMEOUT 120
    )
endif()

rvx_add_gtest(
    NAME EnvironmentPreparedLoadingValidation
    LANE unit
    SOURCES EnvironmentPreparedLoadingValidation/main.cpp
    LIBS
        RVX::Core
        RVX::AssetImport
        RVX::ResourceRuntime
    TAGS subsystem.resource suite.environment trait.async
    TIMEOUT 120
)

rvx_add_gtest(
    NAME ResourceViewCacheValidation
    LANE unit
    SOURCES ResourceViewCacheValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::Render
        RVX_RenderRuntimeCore
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Include
        ${CMAKE_SOURCE_DIR}/Source/Runtime/Render/Private
        ${CMAKE_SOURCE_DIR}/Source/Runtime/RHI/Include
    TIMEOUT 120
)
