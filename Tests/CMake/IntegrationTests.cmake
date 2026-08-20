# Multi-module and product-composition validation.
rvx_add_gtest(
    NAME EngineEcsWorldOwnershipValidation
    LANE integration
    SOURCES EngineEcsWorldOwnershipValidation/main.cpp
    LIBS RVX::Engine
    TAGS subsystem.engine subsystem.world subsystem.scene subsystem.ecs suite.authority
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsSceneRenderProofGatewayValidation
    LANE integration
    SOURCES EcsSceneRenderProofGatewayValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::RenderExtraction
        RVX::ResourceSceneAdapters
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    TAGS subsystem.engine subsystem.render subsystem.resource subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsRenderFramePipelineValidation
    LANE integration
    SOURCES EcsRenderFramePipelineValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::RenderExtraction
        RVX::ResourceSceneAdapters
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    TAGS subsystem.engine subsystem.render subsystem.resource subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsRenderRuntimeCompositionValidation
    LANE integration
    SOURCES EcsRenderRuntimeCompositionValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::RenderExtraction
        RVX::ResourceSceneAdapters
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    TAGS subsystem.engine subsystem.render subsystem.resource subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsWorldRuntimeCompositionValidation
    LANE integration
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
    TAGS subsystem.engine subsystem.world subsystem.render subsystem.resource subsystem.scene subsystem.ecs
    TIMEOUT 30
)

rvx_add_gtest(
    NAME EcsRenderEntityRetirementCoordinatorValidation
    LANE integration
    SOURCES EcsRenderEntityRetirementCoordinatorValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::ResourceSceneAdapters
        RVX::Scene
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Engine/Private/ECS
    TAGS subsystem.engine subsystem.render subsystem.resource subsystem.scene subsystem.ecs suite.retirement
    TIMEOUT 30
)

if(RVX_ENABLE_TSAN)
    rvx_add_gtest(
        NAME RenderConcurrencyTSAN
        LANE integration
        SOURCES RenderConcurrencyTSAN/main.cpp
        LIBS
            RVX::Core
            RVX::RenderContracts
            RVX_RenderRuntimeCore
        INCLUDES ${CMAKE_SOURCE_DIR}/Render/Private
        TAGS trait.tsan
        TIMEOUT 180
    )
endif()

# Product command-line failures are composition behavior, not unit behavior.
if(TARGET RenderVerseSamples)
    rvx_add_validation_test(
        LANE integration
        NAME RenderVerseSamplesUnknownAssetFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples> --sample model-rendering --asset definitely-unknown --backend auto --frames 1
        BUILD_TARGETS RenderVerseSamples
        TAGS product.samples subsystem.resource suite.cli
        TIMEOUT 30
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        WILL_FAIL
    )
    rvx_add_validation_test(
        LANE integration
        NAME RenderVerseSamplesUnsupportedRenderPathFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples> --sample model-rendering --render-path gpu-driven --backend auto --frames 1
        BUILD_TARGETS RenderVerseSamples
        TAGS product.samples subsystem.render suite.cli
        TIMEOUT 30
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        WILL_FAIL
    )
    rvx_add_validation_test(
        LANE integration
        NAME RenderVerseSamplesMissingModelFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples> --sample model-rendering --model "${CMAKE_CURRENT_BINARY_DIR}/DefinitelyMissing/model.gltf" --backend auto --frames 1
        BUILD_TARGETS RenderVerseSamples
        TAGS product.samples subsystem.resource suite.cli
        TIMEOUT 30
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        WILL_FAIL
    )
    rvx_add_validation_test(
        LANE integration
        NAME RenderVerseSamplesUnknownEnvironmentFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples> --sample pbr-materials --environment definitely-unknown --backend auto --frames 1
        BUILD_TARGETS RenderVerseSamples
        TAGS product.samples subsystem.resource suite.cli
        TIMEOUT 30
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        WILL_FAIL
    )
    rvx_add_validation_test(
        LANE integration
        NAME RenderVerseSamplesEnvironmentKindMismatchFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples> --sample pbr-materials --environment pbr-material-grid --backend auto --frames 1
        BUILD_TARGETS RenderVerseSamples
        TAGS product.samples subsystem.resource suite.cli
        TIMEOUT 30
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        WILL_FAIL
    )
    rvx_add_validation_test(
        LANE integration
        NAME RenderVerseSamplesUnsupportedEnvironmentFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples> --sample model-rendering --environment pbr-reference-environment --backend auto --frames 1
        BUILD_TARGETS RenderVerseSamples
        TAGS product.samples subsystem.resource suite.cli
        TIMEOUT 30
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        WILL_FAIL
    )
    rvx_add_validation_test(
        LANE integration
        NAME RenderVerseSamplesMissingEnvironmentFileFailsClosed
        COMMAND $<TARGET_FILE:RenderVerseSamples> --sample pbr-materials --environment-file "${CMAKE_CURRENT_BINARY_DIR}/DefinitelyMissing/environment.exr" --backend auto --frames 1
        BUILD_TARGETS RenderVerseSamples
        TAGS product.samples subsystem.resource suite.cli
        TIMEOUT 30
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        WILL_FAIL
    )
endif()

rvx_add_gtest(
    NAME RenderThreadRuntimeValidation
    LANE integration
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
    TIMEOUT 60
)

rvx_add_gtest(
    NAME RenderHonestyValidation
    LANE integration
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
    TIMEOUT 120
)

rvx_add_gtest(
    NAME RenderSceneValidation
    LANE integration
    SOURCES RenderSceneValidation/main.cpp
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
    TIMEOUT 120
)

rvx_add_gtest(
    NAME RenderPassValidation
    LANE integration
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
    TIMEOUT 120
)

if(TARGET RVX::Editor)
    rvx_add_gtest(
        NAME EditorContextValidation
        LANE integration
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
        TAGS product.editor
        TIMEOUT 120
    )
endif()

rvx_add_gtest(
    NAME AppModeBoundaryValidation
    LANE integration
    SOURCES AppModeBoundaryValidation/main.cpp
    LIBS
        RVX::Core
    TIMEOUT 120
)

if(TARGET RVX_SampleCommon)
rvx_add_gtest(
    NAME SampleWorldRequirementsValidation
    LANE integration
    SOURCES SampleWorldRequirementsValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::SampleCommon
        RVX::World
    TAGS product.samples subsystem.world
    TIMEOUT 60
)
endif()

if(TARGET RVX_SampleCommon)
rvx_add_gtest(
    NAME FrameworkAssessmentValidation
    LANE integration
    SOURCES FrameworkAssessmentValidation/main.cpp
    LIBS RVX::SampleCommon
    TAGS product.samples trait.diagnostics
    TIMEOUT 30
)
endif()

if(TARGET RVX_SampleCommon)
    rvx_add_gtest(
        NAME SampleSceneLifetimeScopeValidation
        LANE integration
        SOURCES SampleSceneLifetimeScopeValidation/main.cpp
        LIBS RVX::SampleCommon
        TAGS product.samples subsystem.scene suite.lifetime
        TIMEOUT 30
    )
endif()

if(TARGET RVX_SampleCommon)
rvx_add_gtest(
    NAME SceneLifecycleSampleValidation
    LANE integration
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
    TAGS product.samples subsystem.scene
    TIMEOUT 30
)
endif()

if(TARGET RVX_SampleCommon)
rvx_add_gtest(
    NAME PhysicsSandboxSampleValidation
    LANE integration
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
    TAGS product.samples subsystem.physics
    TIMEOUT 30
)
endif()

if(TARGET RVX_SampleCommon)
rvx_add_gtest(
    NAME AnimationCharacterSampleValidation
    LANE integration
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
    TAGS product.samples subsystem.animation subsystem.physics
    TIMEOUT 60
)
endif()

if(TARGET RVX_SampleCommon)
rvx_add_gtest(
    NAME AssetStreamingSampleValidation
    LANE integration
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
    TAGS product.samples subsystem.resource
    TIMEOUT 30
)
endif()

if(TARGET RVX_SampleCommon)
rvx_add_gtest(
    NAME InteriorRenderingSampleValidation
    LANE integration
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
    TAGS product.samples subsystem.render
    TIMEOUT 30
)
endif()

if(TARGET RVX_SampleCommon)
rvx_add_gtest(
    NAME RenderingStressSampleValidation
    LANE integration
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
    TAGS product.samples subsystem.render trait.stress
    TIMEOUT 30
)
endif()

if(TARGET RVXCook)
    add_dependencies(RenderHonestyValidation RVXCook)
    target_compile_definitions(RenderHonestyValidation PRIVATE
        RVX_COOK_EXECUTABLE_PATH="$<TARGET_FILE:RVXCook>")
endif()
if(MSVC)
    target_compile_options(RenderPassValidation PRIVATE /bigobj)
endif()
if(TARGET RVX::Editor)
    target_compile_options(EditorContextValidation PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
endif()
