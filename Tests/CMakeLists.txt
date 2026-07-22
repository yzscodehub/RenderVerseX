# =============================================================================
# Tests - Validation test suites
# =============================================================================

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

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
    set(oneValueArgs NAME TIMEOUT LABELS)
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

    gtest_discover_tests(${RVX_GTEST_NAME}
        PROPERTIES
            LABELS "${RVX_GTEST_LABELS}"
            TIMEOUT ${RVX_GTEST_TIMEOUT}
    )
endfunction()

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
        NAME Architecture.PhaseGates
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_architecture_phase_gates.py
            --root ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(Architecture.PhaseGates PROPERTIES
        LABELS "unit;architecture"
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
    NAME RenderContractsValidation
    SOURCES RenderContractsValidation/main.cpp
    LIBS RVX::Core RVX::RenderContracts RVX::RenderExtraction
    INCLUDES ${CMAKE_SOURCE_DIR}/RenderContracts/Private
    LABELS "unit;architecture"
    TIMEOUT 60
)

rvx_add_gtest(
    NAME RenderFrameExtractionValidation
    SOURCES RenderFrameExtractionValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX::RenderExtraction
        RVX::Resource
        RVX::Scene
        RVX::World
    LABELS "unit;architecture"
    TIMEOUT 60
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
        ${RVX_ALL_BACKENDS}
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Render/Private
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
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/RHI_DX12/Private
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
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

# GPU Resource Manager Validation Tests
rvx_add_gtest(
    NAME GPUResourceManagerValidation
    SOURCES
        GPUResourceManagerValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/GPUResourceManager.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/GPUUploadService.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Resources/RenderResourceRegistryLegacy.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Material/MaterialClassification.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/RayTracing/RayTracingScene.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/RayTracing/RayTracingSceneManager.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderDrawItem.cpp
    LIBS
        RVX::Core
        RVX::RHI
        RVX::RenderContracts
        RVX_RenderRuntimeCore
        RVX::Resource
        RVX::Scene
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/RenderContracts/Include
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

# Animation runtime validation tests
rvx_add_gtest(
    NAME AnimationValidation
    SOURCES AnimationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Scene
        RVX::Animation
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Animation/Include
        ${CMAKE_SOURCE_DIR}/Scene/Include
    LABELS unit
    TIMEOUT 120
)

# Render Scene Validation Tests
rvx_add_gtest(
    NAME RenderSceneValidation
    SOURCES
        RenderSceneValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Material/MaterialClassification.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderDrawItem.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderScene.cpp
    LIBS
        RVX::Core
        RVX::Runtime
        RVX::World
        RVX::Resource
        RVX::RenderExtraction
        RVX::Scene
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

# Material system validation tests
rvx_add_gtest(
    NAME MaterialSystemValidation
    SOURCES MaterialSystemValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::Resource
        RVX::RHI
        RVX::Scene
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
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
    SOURCES RenderPassValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::Resource
        RVX::RHI
        ${RVX_ALL_BACKENDS}
        RVX::Scene
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/Render/Private
        ${CMAKE_SOURCE_DIR}/Resource/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
        ${CMAKE_SOURCE_DIR}/Scene/Include
    LABELS unit
    TIMEOUT 120
)

# Particle system validation tests
rvx_add_gtest(
    NAME ParticleValidation
    SOURCES ParticleValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::RHI
        RVX::Scene
        Particle
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Particle/Include
        ${CMAKE_SOURCE_DIR}/Particle/Private
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)
if(RVX_ENABLE_DX11)
    target_link_libraries(ParticleValidation PRIVATE RVX::RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    target_link_libraries(ParticleValidation PRIVATE RVX::RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    target_link_libraries(ParticleValidation PRIVATE RVX::RHI_Vulkan)
endif()
if(RVX_ENABLE_OPENGL)
    target_link_libraries(ParticleValidation PRIVATE RVX::RHI_OpenGL)
endif()

# Feature boundary validation tests
rvx_add_gtest(
    NAME FeatureBoundaryValidation
    SOURCES FeatureBoundaryValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RenderExtraction
        RVX::RenderContracts
        RVX::Scene
        RVX::Terrain
        RVX::Water
        RVX::World
    INCLUDES
        ${CMAKE_SOURCE_DIR}/RenderExtraction/Include
        ${CMAKE_SOURCE_DIR}/RenderContracts/Include
        ${CMAKE_SOURCE_DIR}/Scene/Include
        ${CMAKE_SOURCE_DIR}/Terrain/Include
        ${CMAKE_SOURCE_DIR}/Water/Include
    LABELS unit
    TIMEOUT 60
)

# Actor component validation tests
rvx_add_gtest(
    NAME ActorComponentValidation
    SOURCES ActorComponentValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Audio
        RVX::Scene
        RVX::Resource
        RVX::World
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Resource/Include
    LABELS unit
    TIMEOUT 120
)

# Spatial component validation tests
rvx_add_gtest(
    NAME SpatialComponentValidation
    SOURCES SpatialComponentValidation/main.cpp
    LIBS
        RVX::Core
        RVX::World
        RVX::Scene
        RVX::Resource
    LABELS unit
    TIMEOUT 120
)

# Editor context validation tests
if(TARGET RVX::Editor)
    rvx_add_gtest(
        NAME EditorContextValidation
        SOURCES EditorContextValidation/main.cpp
        LIBS
            RVX::Core
            RVX::Editor
            RVX::Scene
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

# Scripting subsystem validation tests
rvx_add_gtest(
    NAME ScriptingValidation
    SOURCES ScriptingValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Scene
        RVX::Scripting
    LABELS unit
    TIMEOUT 120
)

# Physics world/scene integration validation tests
rvx_add_gtest(
    NAME PhysicsWorldIntegrationValidation
    SOURCES PhysicsWorldIntegrationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::World
        RVX::Scene
        RVX::Physics
    LABELS unit
    TIMEOUT 120
)

# Audio spatial validation tests
rvx_add_gtest(
    NAME AudioSpatialValidation
    SOURCES AudioSpatialValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Audio
        RVX::Physics
        RVX::Scene
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

# Resource instantiation validation tests
rvx_add_gtest(
    NAME ResourceInstantiationValidation
    SOURCES ResourceInstantiationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::Scene
        RVX::World
    LABELS unit
    TIMEOUT 120
)

# Resource runtime policy validation tests
rvx_add_gtest(
    NAME ResourceRuntimePolicyValidation
    SOURCES ResourceRuntimePolicyValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
    LABELS unit
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

target_compile_features(SampleReportContentValidation PRIVATE cxx_std_20)

function(rvx_add_showcase_smoke_test TARGET_NAME)
    if(NOT RVX_ENABLE_DX11 OR NOT TARGET ${TARGET_NAME} OR NOT TARGET ImageContentValidation)
        return()
    endif()

    set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
        ${TARGET_NAME}
    )

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/${config_name}/Showcase")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/Showcase")
    endforeach()

    set(smoke_actual
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/Showcase/${TARGET_NAME}_DX11_320x180.ppm")
    set(smoke_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/Showcase/${TARGET_NAME}_DX11_320x180.json")

    add_test(
        NAME ${TARGET_NAME}DX11Smoke
        COMMAND $<TARGET_FILE:${TARGET_NAME}>
            --smoke
            --frames 2
            --backend dx11
            --width 320
            --height 180
            --quality low
            --screenshot "${smoke_actual}"
            --report "${smoke_report}"
            --diagnostics
            --validation
    )
    set_tests_properties(${TARGET_NAME}DX11Smoke PROPERTIES
        LABELS "visual;gpu;sample;showcase;dx11"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ${TARGET_NAME}DX11ImageContentValidation
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
    set_tests_properties(${TARGET_NAME}DX11ImageContentValidation PROPERTIES
        LABELS "visual;gpu;sample;showcase;dx11"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST ${TARGET_NAME}DX11ImageContentValidation PROPERTY DEPENDS ${TARGET_NAME}DX11Smoke)

    if(TARGET SampleReportContentValidation)
        add_test(
            NAME ${TARGET_NAME}DX11ReportSchemaValidation
            COMMAND $<TARGET_FILE:SampleReportContentValidation>
                --report "${smoke_report}"
                --sample-name ${TARGET_NAME}
                --backend dx11
                --quality low
                --frame-count 2
                ${ARGN}
        )
        set_tests_properties(${TARGET_NAME}DX11ReportSchemaValidation PROPERTIES
            LABELS "visual;gpu;sample;showcase;dx11"
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_property(TEST ${TARGET_NAME}DX11ReportSchemaValidation PROPERTY DEPENDS ${TARGET_NAME}DX11Smoke)
    endif()
endfunction()

function(rvx_add_sample_smoke_test TARGET_NAME CATEGORY)
    if(NOT TARGET ${TARGET_NAME} OR NOT TARGET SampleReportContentValidation)
        return()
    endif()

    set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
        ${TARGET_NAME}
    )

    foreach(config_name Debug Release RelWithDebInfo MinSizeRel)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/${config_name}/${CATEGORY}")
    endforeach()

    set(sample_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/${CATEGORY}/${TARGET_NAME}_ReportOnly.json")

    add_test(
        NAME ${TARGET_NAME}ReportOnlySmoke
        COMMAND $<TARGET_FILE:${TARGET_NAME}>
            --smoke
            --frames 1
            --backend auto
            --width 320
            --height 180
            --quality low
            --report "${sample_report}"
            --diagnostics
    )
    set_tests_properties(${TARGET_NAME}ReportOnlySmoke PROPERTIES
        LABELS "unit;sample;${CATEGORY}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ${TARGET_NAME}ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${sample_report}"
            --sample-name ${TARGET_NAME}
            --category ${CATEGORY}
            --quality low
            --frame-count 1
            --allow-diagnostic-only-render
            ${ARGN}
    )
    set_tests_properties(${TARGET_NAME}ReportValidation PROPERTIES
        LABELS "unit;sample;${CATEGORY}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST ${TARGET_NAME}ReportValidation PROPERTY DEPENDS ${TARGET_NAME}ReportOnlySmoke)
endfunction()

rvx_add_sample_smoke_test(BackendInfoSample basic
    --require-enabled-feature BackendSelection
    --require-enabled-feature RHICapabilityReportContract
    --require-resource-diagnostic "capability report schema"
)
rvx_add_sample_smoke_test(RenderGraphBasics basic
    --require-enabled-feature RenderGraphPassChain
    --require-enabled-feature TransientTexture
    --require-resource-diagnostic "graph contract"
)
rvx_add_sample_smoke_test(ResourcePolicySample basic
    --require-enabled-feature RuntimeResourcePolicy
    --require-resource-diagnostic "source denied"
    --require-resource-diagnostic "cooked texture resolved"
    --require-resource-diagnostic "package shader resolved"
    --require-resource-diagnostic "hash mismatch"
)
rvx_add_sample_smoke_test(TextureUploadSample basic
    --require-enabled-feature CookedTextureArtifact
    --require-enabled-feature GPUUploadService
)
rvx_add_sample_smoke_test(MeshMaterialSample basic
    --require-enabled-feature MeshResource
    --require-enabled-feature PBRMaterial
)
rvx_add_sample_smoke_test(SceneActorComponentSample basic
    --require-enabled-feature ActorComponent
    --require-enabled-feature RenderExtractionSnapshot
)
rvx_add_sample_smoke_test(PostProcessChainSample basic
    --require-enabled-feature ToneMapping
    --require-enabled-feature "SSAO low-tier"
    --require-unsupported-feature "SSR low-tier"
    --require-unsupported-feature "VolumetricLighting"
)
rvx_add_sample_smoke_test(LightingShadowSample basic
    --require-enabled-feature DirectionalLight
    --require-enabled-feature ClusteredLightingStats
    --require-enabled-feature ProceduralSkyboxPass
)
rvx_add_sample_smoke_test(GpuResidencySample basic
    --require-enabled-feature GPUResourceManager
    --require-resource-diagnostic "gpu memory budget"
    --require-resource-diagnostic "eviction reason"
)
rvx_add_sample_smoke_test(InputCameraSample basic
    --require-enabled-feature RuntimeCamera
    --require-enabled-feature InputSubsystemDiagnostics
)
rvx_add_sample_smoke_test(DebugDrawSample basic
    --require-enabled-feature DebugLineCommandBuffer
    --require-enabled-feature FrustumWireframeCPUExtraction
    --require-unsupported-feature "Debug line GPU pipeline"
    --require-resource-diagnostic "frustum wireframe vertices"
)
rvx_add_sample_smoke_test(DecalDiagnosticsSample basic
    --require-enabled-feature DecalCPUList
    --require-enabled-feature DecalRendererDiagnostics
    --require-unsupported-feature "Deferred decal projection"
    --require-resource-diagnostic "GBuffer inputs"
)
rvx_add_sample_smoke_test(SwapChainPolicySample basic
    --require-enabled-feature ExternalSwapChainInjection
    --require-enabled-feature SwapChainManagerDiagnostics
    --require-unsupported-feature "Raw window handle"
    --require-resource-diagnostic "window handle creation"
)
rvx_add_sample_smoke_test(ParticleSnapshotSample basic
    --require-enabled-feature ParticleRenderSnapshot
    --require-enabled-feature CPUParticleSimulationMetadata
    --require-unsupported-feature "GPU particle render payload"
    --require-resource-diagnostic "particle snapshot schema"
)
rvx_add_sample_smoke_test(TerrainSnapshotSample basic
    --require-enabled-feature TerrainRenderSnapshot
    --require-enabled-feature TerrainLODDiagnostic
    --require-unsupported-feature "Terrain render-owned draw pass"
    --require-resource-diagnostic "terrain heightmap diagnostic"
)
rvx_add_sample_smoke_test(WaterSnapshotSample basic
    --require-enabled-feature WaterRenderSnapshot
    --require-enabled-feature SimpleCPUWaveMetadata
    --require-unsupported-feature "Water render-owned draw pass"
    --require-resource-diagnostic "water simulation fallback"
)
rvx_add_sample_smoke_test(PhysicsQuerySample basic
    --require-enabled-feature PhysicsRaycast
    --require-enabled-feature PhysicsQueryStats
    --require-unsupported-feature "HeightField/TriangleMesh"
    --require-resource-diagnostic "raycast hit=true"
)
rvx_add_sample_smoke_test(AudioFallbackSample basic
    --require-enabled-feature AudioFullBufferFallback
    --require-enabled-feature AudioLoaderDiagnostics
    --require-unsupported-feature "Audio resource streaming"
    --require-resource-diagnostic "audio streaming status=UnsupportedStreaming"
    --require-resource-diagnostic "audio threshold fallback status=FallbackStreamingUnsupported"
)
rvx_add_showcase_smoke_test(RenderingShowcase
    --require-enabled-feature IntegratedSceneRender
    --require-enabled-feature PostProcess
)
rvx_add_showcase_smoke_test(PostProcessShowcase
    --require-enabled-feature ToneMapping
    --require-enabled-feature FilmGrain
    --require-enabled-feature "SSAO depth-only low-tier"
    --require-unsupported-feature "TAA/SSR"
)
rvx_add_showcase_smoke_test(MaterialShowcase
    --require-enabled-feature PBRMaterialSwatch
)
rvx_add_showcase_smoke_test(LightingShowcase
    --require-enabled-feature DirectionalLight
    --require-enabled-feature ClusteredLightingStats
)
rvx_add_showcase_smoke_test(SceneInteractionShowcase
    --require-enabled-feature ActorSceneSetup
    --require-unsupported-feature "Picking smoke"
    --require-fallback-reason "Picking uses runtime scene diagnostics"
)
rvx_add_showcase_smoke_test(TerrainWaterShowcase
    --require-enabled-feature TerrainWaterStagingScene
    --require-unsupported-feature "Terrain/Water Render-owned draw passes"
    --require-fallback-reason "Terrain/Water snapshot diagnostics"
)
rvx_add_showcase_smoke_test(ParticleFXShowcase
    --require-enabled-feature ParticleFXStagingScene
    --require-unsupported-feature "GPU particle pipelines"
    --require-fallback-reason "CPU particle staging path"
)
rvx_add_showcase_smoke_test(ResourceRuntimeShowcase
    --require-enabled-feature ResourceManager
    --require-enabled-feature RuntimeResourcePolicyFixture
    --require-unsupported-feature "Full package archive loading"
    --require-fallback-reason "package:// resolves through mount-table fixtures"
)
rvx_add_showcase_smoke_test(PhysicsAudioShowcase
    --require-enabled-feature PhysicsAudioStagingScene
    --require-unsupported-feature "Physics query and audio streaming"
    --require-fallback-reason "Physics advanced collider/query backend gaps"
    --require-fallback-reason "Audio streaming falls back"
)

if(RVX_ENABLE_DX11 AND TARGET ResourceRuntimeShowcase AND TARGET SampleReportContentValidation)
    set(resource_runtime_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/Showcase/ResourceRuntimeShowcase_DX11_320x180.json")
    add_test(
        NAME ResourceRuntimeShowcaseDX11ReportValidation
        COMMAND $<TARGET_FILE:SampleReportContentValidation>
            --report "${resource_runtime_report}"
            --sample-name ResourceRuntimeShowcase
            --backend dx11
            --quality low
            --frame-count 2
            --require-enabled-feature ResourceManager
            --require-enabled-feature ModelResourceLoad
            --require-resource-diagnostic "model fixture"
            --require-resource-diagnostic "source denied"
            --require-resource-diagnostic "cooked artifact resolved"
            --require-resource-diagnostic "cooked mesh artifact resolved"
            --require-resource-diagnostic "cooked material artifact resolved"
            --require-resource-diagnostic "cooked audio artifact resolved"
            --require-resource-diagnostic "package artifact resolved"
            --require-resource-diagnostic "package mesh artifact resolved"
            --require-resource-diagnostic "package material artifact resolved"
            --require-resource-diagnostic "package audio artifact resolved"
            --require-resource-diagnostic "hash mismatch"
            --require-resource-diagnostic "gpu residency memory budget"
            --require-resource-diagnostic "gpu residency used memory"
            --require-resource-diagnostic "gpu residency resident meshes"
            --require-resource-diagnostic "gpu residency resident textures"
            --require-resource-diagnostic "gpu residency pending uploads"
            --require-resource-diagnostic "gpu residency eviction reason"
    )
    set_tests_properties(ResourceRuntimeShowcaseDX11ReportValidation PROPERTIES
        LABELS "visual;gpu;sample;showcase;dx11"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST ResourceRuntimeShowcaseDX11ReportValidation PROPERTY DEPENDS ResourceRuntimeShowcaseDX11Smoke)
endif()

add_executable(ModelViewerHDRIFixtureWriter
    ModelViewerHDRIFixtureWriter/main.cpp
)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    ModelViewerHDRIFixtureWriter
)
target_compile_features(ModelViewerHDRIFixtureWriter PRIVATE cxx_std_20)

add_executable(ModelViewerCookedBCFixtureWriter
    ModelViewerCookedBCFixtureWriter/main.cpp
)
set_property(GLOBAL APPEND PROPERTY RVX_VALIDATION_EXECUTABLE_TARGETS
    ModelViewerCookedBCFixtureWriter
)
target_link_libraries(ModelViewerCookedBCFixtureWriter PRIVATE
    RVX::Tools
)
target_compile_features(ModelViewerCookedBCFixtureWriter PRIVATE cxx_std_20)
if(TARGET RVXCook)
    add_dependencies(ModelViewerCookedBCFixtureWriter RVXCook)
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

if(RVX_ENABLE_DX11 AND TARGET ModelViewer)
    set(RVX_MODELVIEWER_SMOKE_WIDTH 320)
    set(RVX_MODELVIEWER_SMOKE_HEIGHT 180)
    set(RVX_MODELVIEWER_SMOKE_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R7_DX11_320x180.ppm")
    set(RVX_MODELVIEWER_SMOKE_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R7_DX11_320x180.diff.ppm")
    set(RVX_MODELVIEWER_SMOKE_GOLDEN
        "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/R7_DX11_320x180.ppm")
    set(RVX_MODELVIEWER_SHADOW_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RQ3d_Shadow_DX11_320x180.ppm")
    set(RVX_MODELVIEWER_SHADOW_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RQ3d_Shadow_DX11_320x180.diff.ppm")
    set(RVX_MODELVIEWER_SHADOW_GOLDEN
        "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/RQ3d_Shadow_DX11_320x180.ppm")
    set(RVX_MODELVIEWER_PBR_MATERIAL_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RQ4_PBRMaterial_DX11_320x180.ppm")
    set(RVX_MODELVIEWER_PBR_MATERIAL_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RQ4_PBRMaterial_DX11_320x180.diff.ppm")
    set(RVX_MODELVIEWER_PBR_MATERIAL_GOLDEN
        "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/RQ4_PBRMaterial_DX11_320x180.ppm")
    set(RVX_MODELVIEWER_PARTICLE_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/SP9_Particles_DX11_320x180.ppm")
    set(RVX_MODELVIEWER_PARTICLE_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/SP9_Particles_DX11_320x180.diff.ppm")
    set(RVX_MODELVIEWER_PARTICLE_GOLDEN
        "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/SP9_Particles_DX11_320x180.ppm")
    set(RVX_MODELVIEWER_HDRI_FIXTURE
        "${CMAKE_CURRENT_BINARY_DIR}/Fixtures/$<CONFIG>/ModelViewer/RQ2fTinyEnvironment.hdr")

    add_test(
        NAME ModelViewerSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx11
            --width ${RVX_MODELVIEWER_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_SMOKE_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_SMOKE_ACTUAL}"
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerSmoke PROPERTIES
        LABELS "visual;gpu"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME VisualGoldenValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_SMOKE_GOLDEN}"
            --actual "${RVX_MODELVIEWER_SMOKE_ACTUAL}"
            --diff "${RVX_MODELVIEWER_SMOKE_DIFF}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(VisualGoldenValidation PROPERTIES
        LABELS "visual;gpu"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST VisualGoldenValidation PROPERTY DEPENDS ModelViewerSmoke)

    add_test(
        NAME ModelViewerShadowSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/ShadowPlaneCaster.gltf"
            --shadow-test-scene
            --expect-shadow-ready
            --backend dx11
            --width ${RVX_MODELVIEWER_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_SMOKE_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_SHADOW_ACTUAL}"
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerShadowSmoke PROPERTIES
        LABELS "visual;gpu"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ShadowVisualGoldenValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_SHADOW_GOLDEN}"
            --actual "${RVX_MODELVIEWER_SHADOW_ACTUAL}"
            --diff "${RVX_MODELVIEWER_SHADOW_DIFF}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(ShadowVisualGoldenValidation PROPERTIES
        LABELS "visual;gpu"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST ShadowVisualGoldenValidation PROPERTY DEPENDS ModelViewerShadowSmoke)

    add_test(
        NAME ModelViewerPBRMaterialSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/PBRMaterialSwatch.gltf"
            --material-test-scene
            --expect-material-ready
            --backend dx11
            --width ${RVX_MODELVIEWER_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_SMOKE_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_PBR_MATERIAL_ACTUAL}"
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerPBRMaterialSmoke PROPERTIES
        LABELS "visual;gpu"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME PBRMaterialVisualGoldenValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_PBR_MATERIAL_GOLDEN}"
            --actual "${RVX_MODELVIEWER_PBR_MATERIAL_ACTUAL}"
            --diff "${RVX_MODELVIEWER_PBR_MATERIAL_DIFF}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(PBRMaterialVisualGoldenValidation PROPERTIES
        LABELS "visual;gpu"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST PBRMaterialVisualGoldenValidation PROPERTY DEPENDS ModelViewerPBRMaterialSmoke)

    add_test(
        NAME ModelViewerParticleSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --particle-test-scene
            --expect-particles-ready
            --backend dx11
            --width ${RVX_MODELVIEWER_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_SMOKE_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_PARTICLE_ACTUAL}"
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerParticleSmoke PROPERTIES
        LABELS "visual;gpu;particle"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ParticleVisualGoldenValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_PARTICLE_GOLDEN}"
            --actual "${RVX_MODELVIEWER_PARTICLE_ACTUAL}"
            --diff "${RVX_MODELVIEWER_PARTICLE_DIFF}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(ParticleVisualGoldenValidation PROPERTIES
        LABELS "visual;gpu;particle"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST ParticleVisualGoldenValidation PROPERTY DEPENDS ModelViewerParticleSmoke)

    add_test(
        NAME ModelViewerIBLSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx11
            --width ${RVX_MODELVIEWER_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_SMOKE_HEIGHT}
            --frames 2
            --expect-ibl-ready
            --expect-procedural-ibl-quality
            --validation
    )
    set_tests_properties(ModelViewerIBLSmoke PROPERTIES
        LABELS "visual;gpu"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerHDRIFixture
        COMMAND $<TARGET_FILE:ModelViewerHDRIFixtureWriter>
            --output "${RVX_MODELVIEWER_HDRI_FIXTURE}"
    )
    set_tests_properties(ModelViewerHDRIFixture PROPERTIES
        LABELS "visual;fixture"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerHDRISmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx11
            --width ${RVX_MODELVIEWER_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_SMOKE_HEIGHT}
            --frames 2
            --hdri "${RVX_MODELVIEWER_HDRI_FIXTURE}"
            --expect-ibl-ready
            --expect-skybox-ready
            --validation
    )
    set_tests_properties(ModelViewerHDRISmoke PROPERTIES
        LABELS "visual;gpu"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST ModelViewerHDRISmoke PROPERTY DEPENDS ModelViewerHDRIFixture)
endif()

if(RVX_ENABLE_OPENGL AND TARGET ModelViewer)
    set(RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_WIDTH 320)
    set(RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_HEIGHT 180)
    set(RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RQ4_PBRMaterial_OpenGL_320x180.ppm")
    set(RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RQ4_PBRMaterial_OpenGL_320x180.diff.ppm")
    set(RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_GOLDEN
        "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/RQ4_PBRMaterial_OpenGL_320x180.ppm")
    set(RVX_MODELVIEWER_OPENGL_COOKED_BC_FIXTURE_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/Fixtures/$<CONFIG>/ModelViewer/CookedBCMaterial")
    set(RVX_MODELVIEWER_OPENGL_COOKED_BC_FIXTURE
        "${RVX_MODELVIEWER_OPENGL_COOKED_BC_FIXTURE_DIR}/CookedBCMaterial.gltf")
    set(RVX_MODELVIEWER_OPENGL_COOKED_BC_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/SP14_CookedBCMaterial_OpenGL_320x180.ppm")

    add_test(
        NAME ModelViewerOpenGLPBRMaterialSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/PBRMaterialSwatch.gltf"
            --material-test-scene
            --expect-material-ready
            --backend opengl
            --width ${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_WIDTH}
            --height ${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_ACTUAL}"
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerOpenGLPBRMaterialSmoke PROPERTIES
        LABELS "visual;gpu;opengl"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME OpenGLPBRMaterialVisualGoldenValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_GOLDEN}"
            --actual "${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_ACTUAL}"
            --diff "${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_DIFF}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(OpenGLPBRMaterialVisualGoldenValidation PROPERTIES
        LABELS "visual;gpu;opengl"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST OpenGLPBRMaterialVisualGoldenValidation PROPERTY DEPENDS ModelViewerOpenGLPBRMaterialSmoke)

    add_test(
        NAME ModelViewerCookedBCMaterialFixture
        COMMAND $<TARGET_FILE:ModelViewerCookedBCFixtureWriter>
            --output-dir "${RVX_MODELVIEWER_OPENGL_COOKED_BC_FIXTURE_DIR}"
            --rvxcook "$<TARGET_FILE:RVXCook>"
    )
    set_tests_properties(ModelViewerCookedBCMaterialFixture PROPERTIES
        LABELS "visual;fixture;opengl"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerOpenGLCookedBCMaterialSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${RVX_MODELVIEWER_OPENGL_COOKED_BC_FIXTURE}"
            --material-test-scene
            --expect-material-ready
            --backend opengl
            --width ${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_WIDTH}
            --height ${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_OPENGL_COOKED_BC_ACTUAL}"
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerOpenGLCookedBCMaterialSmoke PROPERTIES
        LABELS "visual;gpu;opengl"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST ModelViewerOpenGLCookedBCMaterialSmoke PROPERTY DEPENDS ModelViewerCookedBCMaterialFixture)

    add_test(
        NAME OpenGLCookedBCMaterialImageContentValidation
        COMMAND $<TARGET_FILE:ImageContentValidation>
            --image "${RVX_MODELVIEWER_OPENGL_COOKED_BC_ACTUAL}"
            --min-width ${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_WIDTH}
            --min-height ${RVX_MODELVIEWER_OPENGL_PBR_MATERIAL_HEIGHT}
            --min-unique-colors 16
            --min-luma-range 20
            --near-black-threshold 8
            --max-near-black-ratio 0.98
            --sample-step 4
    )
    set_tests_properties(OpenGLCookedBCMaterialImageContentValidation PROPERTIES
        LABELS "visual;gpu;opengl"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST OpenGLCookedBCMaterialImageContentValidation PROPERTY DEPENDS ModelViewerOpenGLCookedBCMaterialSmoke)
endif()

if(RVX_ENABLE_DX12 AND TARGET ModelViewer)
    set(RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_WIDTH 320)
    set(RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_HEIGHT 180)
    set(RVX_MODELVIEWER_RT_SMOKE_WIDTH 160)
    set(RVX_MODELVIEWER_RT_SMOKE_HEIGHT 90)
    set(RVX_MODELVIEWER_RT_RESIZE_WIDTH 192)
    set(RVX_MODELVIEWER_RT_RESIZE_HEIGHT 108)
    set(RVX_MODELVIEWER_GPU_DRIVEN_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R11_GPUDriven_DX12_320x180.ppm")
    set(RVX_MODELVIEWER_GPU_DRIVEN_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R11_GPUDriven_DX12_320x180.diff.ppm")
    set(RVX_MODELVIEWER_GPU_DRIVEN_DISABLED_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R11_GPUDrivenDisabled_DX12_320x180.ppm")
    set(RVX_MODELVIEWER_GPU_DRIVEN_DISABLED_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R11_GPUDrivenDisabled_DX12_320x180.diff.ppm")
    set(RVX_MODELVIEWER_GPU_DRIVEN_GOLDEN
        "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/R11_GPUDriven_DX12_320x180.ppm")
    set(RVX_MODELVIEWER_RT_SHADOW_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RTShadow_DX12_192x108.ppm")
    set(RVX_MODELVIEWER_RT_SHADOW_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RTShadow_DX12_192x108.diff.ppm")
    set(RVX_MODELVIEWER_RT_SHADOW_GOLDEN
        "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/RTShadow_DX12_192x108.ppm")
    set(RVX_MODELVIEWER_RT_REFLECTION_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RTReflection_DX12_192x108.ppm")
    set(RVX_MODELVIEWER_RT_REFLECTION_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/RTReflection_DX12_192x108.diff.ppm")
    set(RVX_MODELVIEWER_RT_REFLECTION_GOLDEN
        "${CMAKE_CURRENT_SOURCE_DIR}/Golden/ModelViewer/RTReflection_DX12_192x108.ppm")

    add_test(
        NAME ModelViewerGPUDrivenSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx12
            --width ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_GPU_DRIVEN_ACTUAL}"
            --no-ibl
            --gpu-driven-culling-test-scene
            --expect-gpu-driven-culling-ready
            --validation
    )
    set_tests_properties(ModelViewerGPUDrivenSmoke PROPERTIES
        LABELS "visual;gpu;gpu-driven"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME GPUDrivenVisualGoldenValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_GPU_DRIVEN_GOLDEN}"
            --actual "${RVX_MODELVIEWER_GPU_DRIVEN_ACTUAL}"
            --diff "${RVX_MODELVIEWER_GPU_DRIVEN_DIFF}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(GPUDrivenVisualGoldenValidation PROPERTIES
        LABELS "visual;gpu;gpu-driven"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST GPUDrivenVisualGoldenValidation PROPERTY DEPENDS ModelViewerGPUDrivenSmoke)

    add_test(
        NAME ModelViewerGPUDrivenDisabledSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx12
            --width ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_GPU_DRIVEN_DISABLED_ACTUAL}"
            --no-ibl
            --gpu-driven-culling-test-scene
            --disable-gpu-driven-culling
            --validation
    )
    set_tests_properties(ModelViewerGPUDrivenDisabledSmoke PROPERTIES
        LABELS "visual;gpu;gpu-driven"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME GPUDrivenDisabledVisualDiffValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_GPU_DRIVEN_GOLDEN}"
            --actual "${RVX_MODELVIEWER_GPU_DRIVEN_DISABLED_ACTUAL}"
            --diff "${RVX_MODELVIEWER_GPU_DRIVEN_DISABLED_DIFF}"
            --tolerance 0.0
            --max-different-pixels 57600
            --min-different-pixels 1
    )
    set_tests_properties(GPUDrivenDisabledVisualDiffValidation PROPERTIES
        LABELS "visual;gpu;gpu-driven"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST GPUDrivenDisabledVisualDiffValidation PROPERTY DEPENDS
        "ModelViewerGPUDrivenSmoke;ModelViewerGPUDrivenDisabledSmoke")

    add_test(
        NAME ModelViewerRayTracedShadowSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/ShadowPlaneCaster.gltf"
            --shadow-test-scene
            --expect-ray-traced-shadow-ready
            --expect-ray-traced-shadow-history-ready
            --expect-ray-traced-shadow-history-reset-ready
            --rt-reset-history-frame 4
            --expect-ray-traced-shadow-history-resize-ready
            --rt-resize-frame 5
            --rt-resize-width ${RVX_MODELVIEWER_RT_RESIZE_WIDTH}
            --rt-resize-height ${RVX_MODELVIEWER_RT_RESIZE_HEIGHT}
            --expect-ray-tracing-gpu-timing-ready
            --backend dx12
            --width ${RVX_MODELVIEWER_RT_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_RT_SMOKE_HEIGHT}
            --frames 6
            --screenshot "${RVX_MODELVIEWER_RT_SHADOW_ACTUAL}"
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerRayTracedShadowSmoke PROPERTIES
        LABELS "visual;gpu;raytracing"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerRayTracedShadowVisualGoldenValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_RT_SHADOW_GOLDEN}"
            --actual "${RVX_MODELVIEWER_RT_SHADOW_ACTUAL}"
            --diff "${RVX_MODELVIEWER_RT_SHADOW_DIFF}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(ModelViewerRayTracedShadowVisualGoldenValidation PROPERTIES
        LABELS "visual;gpu;raytracing"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST ModelViewerRayTracedShadowVisualGoldenValidation PROPERTY DEPENDS ModelViewerRayTracedShadowSmoke)

    add_test(
        NAME ModelViewerRayTracedShadowAlphaMaskSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/RTAlphaMaskedTriangle.gltf"
            --shadow-test-scene
            --expect-ray-traced-shadow-ready
            --expect-ray-traced-shadow-alpha-ready
            --backend dx12
            --width ${RVX_MODELVIEWER_RT_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_RT_SMOKE_HEIGHT}
            --frames 4
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerRayTracedShadowAlphaMaskSmoke PROPERTIES
        LABELS "visual;gpu;raytracing"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME ModelViewerRayTracedReflectionSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --ray-traced-reflections
            --expect-ray-traced-reflection-ready
            --expect-ray-traced-reflection-history-ready
            --expect-ray-traced-reflection-history-reset-ready
            --rt-reset-history-frame 4
            --expect-ray-traced-reflection-history-resize-ready
            --rt-resize-frame 5
            --rt-resize-width ${RVX_MODELVIEWER_RT_RESIZE_WIDTH}
            --rt-resize-height ${RVX_MODELVIEWER_RT_RESIZE_HEIGHT}
            --expect-ray-tracing-gpu-timing-ready
            --backend dx12
            --width ${RVX_MODELVIEWER_RT_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_RT_SMOKE_HEIGHT}
            --frames 6
            --screenshot "${RVX_MODELVIEWER_RT_REFLECTION_ACTUAL}"
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerRayTracedReflectionSmoke PROPERTIES
        LABELS "visual;gpu;raytracing"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerRayTracedReflectionVisualGoldenValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_RT_REFLECTION_GOLDEN}"
            --actual "${RVX_MODELVIEWER_RT_REFLECTION_ACTUAL}"
            --diff "${RVX_MODELVIEWER_RT_REFLECTION_DIFF}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(ModelViewerRayTracedReflectionVisualGoldenValidation PROPERTIES
        LABELS "visual;gpu;raytracing"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST ModelViewerRayTracedReflectionVisualGoldenValidation PROPERTY DEPENDS ModelViewerRayTracedReflectionSmoke)

    add_test(
        NAME ModelViewerRayTracedReflectionAssetSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/RTAlphaMaskedTriangle.gltf"
            --ray-traced-reflections
            --expect-ray-traced-reflection-ready
            --expect-ray-traced-reflection-asset-ready
            --backend dx12
            --width ${RVX_MODELVIEWER_RT_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_RT_SMOKE_HEIGHT}
            --frames 4
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerRayTracedReflectionAssetSmoke PROPERTIES
        LABELS "visual;gpu;raytracing"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerRayTracingBudgetSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --ray-traced-reflections
            --rt-max-rays 8000
            --rt-max-resource-bytes 1
            --expect-ray-traced-reflection-ready
            --expect-ray-tracing-budget-applied
            --expect-ray-tracing-ray-budget-respected
            --expect-ray-tracing-resource-budget-exceeded
            --backend dx12
            --width ${RVX_MODELVIEWER_RT_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_RT_SMOKE_HEIGHT}
            --frames 2
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerRayTracingBudgetSmoke PROPERTIES
        LABELS "visual;gpu;raytracing"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerRayTracingGpuBudgetSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --ray-traced-reflections
            --rt-max-reflection-gpu-ms 0.000001
            --rt-gpu-adjust-frames 1
            --expect-ray-traced-reflection-ready
            --expect-ray-tracing-gpu-timing-ready
            --expect-ray-tracing-gpu-budget-applied
            --backend dx12
            --width ${RVX_MODELVIEWER_RT_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_RT_SMOKE_HEIGHT}
            --frames 6
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerRayTracingGpuBudgetSmoke PROPERTIES
        LABELS "visual;gpu;raytracing"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerRayTracingGpuBudgetRecoverySmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --ray-traced-reflections
            --rt-max-reflection-gpu-ms 0.000001
            --rt-recovery-reflection-gpu-ms 1000.0
            --rt-gpu-adjust-frames 1
            --expect-ray-traced-reflection-ready
            --expect-ray-tracing-gpu-timing-ready
            --expect-ray-tracing-gpu-budget-recovered
            --backend dx12
            --width ${RVX_MODELVIEWER_RT_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_RT_SMOKE_HEIGHT}
            --frames 8
            --no-ibl
            --validation
    )
    set_tests_properties(ModelViewerRayTracingGpuBudgetRecoverySmoke PROPERTIES
        LABELS "visual;gpu;raytracing"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
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
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
        LABELS gpu
        TIMEOUT 180
    )
endif()

# System Integration Test (Spatial + Scene + Asset)
rvx_add_gtest(
    NAME SystemIntegrationTest
    SOURCES SystemIntegration/main.cpp
    LIBS
        RVX::Core
        Spatial
        RVX::Scene
        RVX::Resource
        RVX::Audio
    LABELS unit
    TIMEOUT 120
)

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
    ModelViewer
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
