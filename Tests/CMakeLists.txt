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

    add_test(
        NAME Architecture.M1ArchitectureCut
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/Scripts/check_m1_architecture.py
            --root ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(Architecture.M1ArchitectureCut PROPERTIES
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
    NAME StartupTimelineValidation
    SOURCES StartupTimelineValidation/main.cpp
    LIBS RVX::Core
    LABELS "unit;diagnostics;startup"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME ModelCameraFramingValidation
    SOURCES ModelCameraFramingValidation/main.cpp
    LIBS RVX::SampleCommon
    LABELS "unit;samples;assets"
    TIMEOUT 30
)

rvx_add_gtest(
    NAME RenderContractsValidation
    SOURCES
        RenderContractsValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/RenderContracts/Private/RenderFramePacket.cpp
        ${CMAKE_SOURCE_DIR}/RenderExtraction/Private/RenderFramePacketBuilder.cpp
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
    NAME RenderTransportAuthoritativeValidation
    SOURCES RenderTransportShadowValidation/main.cpp
    LIBS
        RVX::Core
        RVX::RenderContracts
        RVX::RenderExtraction
        RVX::Render
        RVX::Resource
        RVX::Scene
        RVX::World
    INCLUDES ${CMAKE_SOURCE_DIR}/Render/Include
    LABELS "unit;architecture;transport"
    TIMEOUT 60
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
    NAME EngineRenderCompositionValidation
    SOURCES EngineRenderCompositionValidation/main.cpp
    LIBS
        RVX::Engine
        RVX::RenderExtraction
        RVX::RHI_BackendFactory
        ${RVX_ALL_BACKENDS}
    INCLUDES ${CMAKE_SOURCE_DIR}/Engine/Private
    DEFINES RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
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

# Animation runtime validation tests
rvx_add_gtest(
    NAME AnimationValidation
    SOURCES AnimationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Scene
        RVX::Animation
        RVX::Resource
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Animation/Include
        ${CMAKE_SOURCE_DIR}/Scene/Include
    LABELS unit
    TIMEOUT 120
)

# Scene must link independently of Animation-owned component adapters.
rvx_add_gtest(
    NAME SceneLinkClosureValidation
    SOURCES SceneLinkClosureValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;architecture"
    TIMEOUT 30
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

# Particle system validation tests
rvx_add_gtest(
    NAME ParticleValidation
    SOURCES ParticleValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Render
        RVX::RHI
        RVX::RHI_BackendFactory
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

rvx_add_gtest(
    NAME SceneHandleValidation
    SOURCES SceneHandleValidation/main.cpp
    LIBS RVX::Scene RVX::World
    LABELS "unit;scene;architecture"
)

rvx_add_gtest(
    NAME SceneLifetimeValidation
    SOURCES SceneLifetimeValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;architecture"
)

rvx_add_gtest(
    NAME SceneComponentRegistryValidation
    SOURCES SceneComponentRegistryValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;architecture"
)

rvx_add_gtest(
    NAME SceneTransformHierarchyValidation
    SOURCES SceneTransformHierarchyValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;architecture"
)

rvx_add_gtest(
    NAME SceneTransformScaleValidation
    SOURCES SceneTransformScaleValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;architecture;performance"
    TIMEOUT 60
)

rvx_add_gtest(
    NAME SceneSystemScheduleValidation
    SOURCES SceneSystemScheduleValidation/main.cpp
    LIBS RVX::Scene
    LABELS "unit;scene;architecture"
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

rvx_add_gtest(
    NAME SceneAssetInstantiationValidation
    SOURCES SceneAssetInstantiationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
        RVX::ResourceSceneAdapters
        RVX::Scene
    LABELS "unit;architecture"
    TIMEOUT 30
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

target_compile_features(SampleReportContentValidation PRIVATE cxx_std_20)

function(rvx_add_renderverse_primary_smoke SAMPLE_ID SAMPLE_LABEL ASSET_ID BACKEND_NAME BACKEND_LABEL)
    cmake_parse_arguments(
        RVX_SAMPLE
        "WAIT_READY"
        "READY_TIMEOUT_MS;READY_MAX_FRAMES"
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
    endforeach()

    set(test_prefix "RenderVerseSamples${BACKEND_LABEL}${SAMPLE_LABEL}")
    set(smoke_actual
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.ppm")
    set(smoke_report
        "${CMAKE_CURRENT_BINARY_DIR}/SampleReports/$<CONFIG>/RenderVerseSamples/${test_prefix}_320x180.json")
    set(smoke_readiness_args)
    set(smoke_frame_report_args --frame-count 8)
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
        set(smoke_frame_report_args --min-frame-count 8)
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
            --diagnostics
            --validation
            ${smoke_readiness_args}
            ${RVX_SAMPLE_SAMPLE_ARGS}
    )
    set_tests_properties(${test_prefix}Smoke PROPERTIES
        LABELS "visual;gpu;sample;primary;${BACKEND_NAME}"
        TIMEOUT 120
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
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
            --asset-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
            --require-redistributable
            --min-visible-objects 1
            --require-enabled-feature ModelResourceLoad
            --require-enabled-feature SceneInstantiation
            --require-enabled-feature RenderGraph
            --require-enabled-feature SceneRendering
            --require-ready
            ${RVX_SAMPLE_REPORT_ARGS}
    )
    set_tests_properties(${test_prefix}ReportValidation PROPERTIES
        LABELS "visual;gpu;sample;primary;${BACKEND_NAME}"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ${test_prefix}Smoke
    )
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
    )
endfunction()

function(rvx_add_renderverse_external_model_viewer_validation BACKEND_NAME BACKEND_LABEL)
    if(NOT RVX_ENABLE_EXTERNAL_ASSET_TESTS OR
       NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation)
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
            --ready-max-frames 1024
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

    foreach(render_path direct gpu-driven)
        if(render_path STREQUAL "direct")
            set(path_label Direct)
        else()
            set(path_label GPUDriven)
        endif()
        set(lifetime_prefix "${test_prefix}${path_label}")
        set(lifetime_report
            "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_600.json")
        set(lifetime_sample_report
            "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_sample.json")
        add_test(
            NAME ${lifetime_prefix}Lifetime600
            COMMAND $<TARGET_FILE:RenderVerseSamples>
                --sample model-viewer
                --asset free-1975-porsche-911-930-turbo
                --catalog "${external_catalog}"
                --asset-root "${RVX_EXTERNAL_MODEL_ROOT}"
                --backend ${BACKEND_NAME}
                --render-path ${render_path}
                --smoke
                --frames 1
                --wait-ready
                --ready-timeout-ms 180000
                --ready-max-frames 1024
                --width 320
                --height 180
                --quality low
                --deterministic-orbit
                --lifetime-warmup-frames 120
                --lifetime-observation-frames 480
                --lifetime-report "${lifetime_report}"
                --report "${lifetime_sample_report}"
                --diagnostics
                --validation
        )
        set_tests_properties(${lifetime_prefix}Lifetime600 PROPERTIES
            LABELS "visual;gpu;sample;model-viewer;lifetime;external-assets;qualification;${BACKEND_NAME};${render_path}"
            TIMEOUT 1200
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
        )

        if(RVX_ENABLE_LONG_RUNNING_QUALIFICATION_TESTS)
            set(soak_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_10000.json")
            set(soak_sample_report
                "${CMAKE_BINARY_DIR}/artifacts/$<CONFIG>/ModelViewerLifetime/${lifetime_prefix}_10000_sample.json")
            add_test(
                NAME ${lifetime_prefix}Lifetime10000
                COMMAND $<TARGET_FILE:RenderVerseSamples>
                    --sample model-viewer
                    --asset free-1975-porsche-911-930-turbo
                    --catalog "${external_catalog}"
                    --asset-root "${RVX_EXTERNAL_MODEL_ROOT}"
                    --backend ${BACKEND_NAME}
                    --render-path ${render_path}
                    --smoke
                    --frames 1
                    --wait-ready
                    --ready-timeout-ms 180000
                    --ready-max-frames 1024
                    --width 320
                    --height 180
                    --quality low
                    --deterministic-orbit
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
                LABELS "visual;gpu;sample;model-viewer;lifetime;soak;external-assets;qualification;${BACKEND_NAME};${render_path}"
                TIMEOUT 3600
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:RenderVerseSamples>"
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
    )
endfunction()

function(rvx_add_renderverse_gpu_driven_parity BACKEND_NAME BACKEND_LABEL)
    if(NOT TARGET RenderVerseSamples OR
       NOT TARGET ImageContentValidation OR
       NOT TARGET SampleReportContentValidation OR
       NOT TARGET VisualGoldenValidation)
        return()
    endif()

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
            set(render_path_args)
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
                --min-frame-count 2
                --asset-id r7-triangle
                --asset-license-spdx LicenseRef-RenderVerseX-Generated-Fixture
                --require-redistributable
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
    )
endfunction()

if(WIN32 AND RVX_ENABLE_DX12)
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
    rvx_add_renderverse_pbr_response_validation(dx12 DX12 PBRMaterials)
    rvx_add_renderverse_pbr_texture_smoke(dx12 DX12)
    rvx_add_renderverse_pbr_workflow_parity(dx12 DX12)
    rvx_add_renderverse_external_model_viewer_validation(dx12 DX12)
    rvx_add_renderverse_external_environment_validation(dx12 DX12)
    rvx_add_renderverse_readiness_smoke(dx12 DX12)
    rvx_add_renderverse_gpu_driven_parity(dx12 DX12)
endif()
if(RVX_ENABLE_VULKAN)
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
    --require-enabled-feature RenderResourceRegistry
    --require-resource-diagnostic "render resource registry"
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
    ${CMAKE_SOURCE_DIR}/Samples/Common/Tools/SampleHDRFixtureWriter.cpp
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
    set(RVX_MODELVIEWER_GPU_DRIVEN_PARITY_GPU_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R11_GPUDrivenParityGPU_DX12_320x180.ppm")
    set(RVX_MODELVIEWER_GPU_DRIVEN_PARITY_DIRECT_ACTUAL
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R11_GPUDrivenParityDirect_DX12_320x180.ppm")
    set(RVX_MODELVIEWER_GPU_DRIVEN_PARITY_DIFF
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R11_GPUDrivenCrossPathParity_DX12_320x180.diff.ppm")
    set(RVX_MODELVIEWER_GPU_DRIVEN_PARITY_REPORT
        "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ModelViewer/R11_GPUDrivenCrossPathParity_DX12_320x180.json")
    set(RVX_MODELVIEWER_M2_GPU_RESIZE_REPORT
        "${CMAKE_BINARY_DIR}/M2Exit/$<CONFIG>/ModelViewerGPUDrivenResize.measurement.json")
    set(RVX_MODELVIEWER_M2_GPU_ZERO_VISIBLE_REPORT
        "${CMAKE_BINARY_DIR}/M2Exit/$<CONFIG>/ModelViewerGPUDrivenZeroVisible.measurement.json")
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
            --gpu-driven on
            --expect-gpu-driven-culling-ready
            --validation
    )
    set_tests_properties(ModelViewerGPUDrivenSmoke PROPERTIES
        LABELS "visual;gpu;gpu-driven"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerGPUDrivenAutoPolicySmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx12
            --width ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_HEIGHT}
            --frames 8
            --no-ibl
            --gpu-driven-culling-test-scene
            --gpu-driven auto
            --expect-gpu-driven-auto-policy-ready
            --validation
    )
    set_tests_properties(ModelViewerGPUDrivenAutoPolicySmoke PROPERTIES
        LABELS "gpu;gpu-driven;qualification"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerGPUDrivenResizeSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx12
            --width ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_HEIGHT}
            --frames 6
            --no-ibl
            --gpu-driven-culling-test-scene
            --gpu-driven on
            --expect-gpu-driven-culling-ready
            --resize-frame 4
            --resize-width ${RVX_MODELVIEWER_RT_RESIZE_WIDTH}
            --resize-height ${RVX_MODELVIEWER_RT_RESIZE_HEIGHT}
            --render-policy-report "${RVX_MODELVIEWER_M2_GPU_RESIZE_REPORT}"
            --validation
    )
    set_tests_properties(ModelViewerGPUDrivenResizeSmoke PROPERTIES
        LABELS "native;gpu;gpu-driven;resize;m2"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerGPUDrivenZeroVisibleSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx12
            --width ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_HEIGHT}
            --frames 8
            --no-ibl
            --gpu-driven-zero-visible-scene
            --gpu-driven on
            --expect-gpu-driven-zero-visible-ready
            --render-policy-report "${RVX_MODELVIEWER_M2_GPU_ZERO_VISIBLE_REPORT}"
            --validation
    )
    set_tests_properties(ModelViewerGPUDrivenZeroVisibleSmoke PROPERTIES
        LABELS "native;gpu;gpu-driven;zero-visible;m2"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerGPUDrivenParityGPUSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx12
            --width ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_GPU_DRIVEN_PARITY_GPU_ACTUAL}"
            --no-ibl
            --gpu-driven on
            --expect-gpu-driven-culling-ready
            --validation
    )
    set_tests_properties(ModelViewerGPUDrivenParityGPUSmoke PROPERTIES
        LABELS "visual;gpu;gpu-driven;qualification;parity"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME ModelViewerGPUDrivenParityDirectSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend dx12
            --width ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_WIDTH}
            --height ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_HEIGHT}
            --frames 8
            --screenshot "${RVX_MODELVIEWER_GPU_DRIVEN_PARITY_DIRECT_ACTUAL}"
            --no-ibl
            --gpu-driven off
            --expect-gpu-driven-direct-ready
            --validation
    )
    set_tests_properties(ModelViewerGPUDrivenParityDirectSmoke PROPERTIES
        LABELS "visual;gpu;gpu-driven;qualification;parity"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_test(
        NAME GPUDrivenCrossPathVisualParityValidation
        COMMAND $<TARGET_FILE:VisualGoldenValidation>
            --expected "${RVX_MODELVIEWER_GPU_DRIVEN_PARITY_GPU_ACTUAL}"
            --actual "${RVX_MODELVIEWER_GPU_DRIVEN_PARITY_DIRECT_ACTUAL}"
            --diff "${RVX_MODELVIEWER_GPU_DRIVEN_PARITY_DIFF}"
            --report "${RVX_MODELVIEWER_GPU_DRIVEN_PARITY_REPORT}"
            --tolerance 0.0
            --max-different-pixels 0
    )
    set_tests_properties(GPUDrivenCrossPathVisualParityValidation PROPERTIES
        LABELS "visual;gpu;gpu-driven;qualification;parity"
        TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_property(TEST GPUDrivenCrossPathVisualParityValidation PROPERTY DEPENDS
        "ModelViewerGPUDrivenParityGPUSmoke;ModelViewerGPUDrivenParityDirectSmoke")

    if(RVX_ENABLE_EXTERNAL_ASSET_TESTS)
        if(NOT RVX_EXTERNAL_TEST_ASSET_ROOT)
            message(FATAL_ERROR
                "RVX_ENABLE_EXTERNAL_ASSET_TESTS requires RVX_EXTERNAL_TEST_ASSET_ROOT")
        endif()

        cmake_path(ABSOLUTE_PATH RVX_EXTERNAL_TEST_ASSET_ROOT
            NORMALIZE OUTPUT_VARIABLE RVX_EXTERNAL_TEST_ASSET_ROOT_ABSOLUTE)
        set(RVX_PORSCHE_REAL_ASSET
            "${RVX_EXTERNAL_TEST_ASSET_ROOT_ABSOLUTE}/models/free_1975_porsche_911_930_turbo/scene.gltf")
        set(RVX_PORSCHE_REAL_ASSET_LICENSE
            "${RVX_EXTERNAL_TEST_ASSET_ROOT_ABSOLUTE}/models/free_1975_porsche_911_930_turbo/license.txt")
        if(NOT EXISTS "${RVX_PORSCHE_REAL_ASSET}" OR
           NOT EXISTS "${RVX_PORSCHE_REAL_ASSET_LICENSE}")
            message(FATAL_ERROR
                "External Porsche gate requires scene.gltf and license.txt under ${RVX_EXTERNAL_TEST_ASSET_ROOT_ABSOLUTE}")
        endif()

        set(RVX_PORSCHE_REAL_ASSET_GPU_CAPTURE
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ExternalAssets/Porsche_GPUDriven_DX12_320x180.ppm")
        set(RVX_PORSCHE_REAL_ASSET_DIRECT_CAPTURE
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ExternalAssets/Porsche_Direct_DX12_320x180.ppm")
        set(RVX_PORSCHE_REAL_ASSET_PARITY_DIFF
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ExternalAssets/Porsche_CrossPath_DX12_320x180.diff.ppm")
        set(RVX_PORSCHE_REAL_ASSET_PARITY_REPORT
            "${CMAKE_CURRENT_BINARY_DIR}/VisualArtifacts/$<CONFIG>/ExternalAssets/Porsche_CrossPath_DX12_320x180.json")

        add_test(
            NAME ModelViewerExternalPorscheDirectSmoke
            COMMAND $<TARGET_FILE:ModelViewer>
                --smoke
                --model "${RVX_PORSCHE_REAL_ASSET}"
                --backend dx12
                --width 320
                --height 180
                --frames 4
                --camera-fit auto
                --screenshot "${RVX_PORSCHE_REAL_ASSET_DIRECT_CAPTURE}"
                --gpu-driven off
                --wait-model-ready
                --model-ready-timeout-ms 120000
                --expect-model-visible
                --expect-gpu-driven-direct-ready
                --validation
        )
        set_tests_properties(ModelViewerExternalPorscheDirectSmoke PROPERTIES
            LABELS "visual;gpu;gpu-driven;external-assets;qualification-candidate"
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME ModelViewerExternalPorscheGPUDrivenSmoke
            COMMAND $<TARGET_FILE:ModelViewer>
                --smoke
                --model "${RVX_PORSCHE_REAL_ASSET}"
                --backend dx12
                --width 320
                --height 180
                --frames 4
                --camera-fit auto
                --screenshot "${RVX_PORSCHE_REAL_ASSET_GPU_CAPTURE}"
                --gpu-driven on
                --wait-model-ready
                --model-ready-timeout-ms 120000
                --expect-model-visible
                --expect-gpu-driven-culling-ready
                --expect-gpu-driven-multi-batch-ready
                --validation
        )
        set_tests_properties(ModelViewerExternalPorscheGPUDrivenSmoke PROPERTIES
            LABELS "visual;gpu;gpu-driven;external-assets;qualification-candidate"
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        add_test(
            NAME ExternalPorscheGPUDrivenCrossPathParityValidation
            COMMAND $<TARGET_FILE:VisualGoldenValidation>
                --expected "${RVX_PORSCHE_REAL_ASSET_GPU_CAPTURE}"
                --actual "${RVX_PORSCHE_REAL_ASSET_DIRECT_CAPTURE}"
                --diff "${RVX_PORSCHE_REAL_ASSET_PARITY_DIFF}"
                --report "${RVX_PORSCHE_REAL_ASSET_PARITY_REPORT}"
                --tolerance 0.0
                --max-different-pixels 0
        )
        set_tests_properties(ExternalPorscheGPUDrivenCrossPathParityValidation PROPERTIES
            LABELS "visual;gpu;gpu-driven;external-assets;qualification-candidate;parity"
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        set_property(TEST ExternalPorscheGPUDrivenCrossPathParityValidation PROPERTY DEPENDS
            "ModelViewerExternalPorscheGPUDrivenSmoke;ModelViewerExternalPorscheDirectSmoke")
    endif()

    if(RVX_ENABLE_GPU_VALIDATION_TESTS)
        add_test(
            NAME ModelViewerGPUDrivenGBVSmoke
            COMMAND $<TARGET_FILE:ModelViewer>
                --smoke
                --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
                --backend dx12
                --width ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_WIDTH}
                --height ${RVX_MODELVIEWER_GPU_DRIVEN_SMOKE_HEIGHT}
                --frames 1
                --no-ibl
                --gpu-driven-culling-test-scene
                --gpu-driven on
                --expect-gpu-driven-culling-ready
                --validation
                --gpu-validation
        )
        set_tests_properties(ModelViewerGPUDrivenGBVSmoke PROPERTIES
            LABELS "nightly;gpu;gpu-driven;gpu-validation"
            TIMEOUT 180
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

        find_program(RVX_D4C_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(RVX_D4C_POWERSHELL_EXECUTABLE)
            add_test(
                NAME DX12GPUSceneTier2Candidate
                COMMAND ${RVX_D4C_POWERSHELL_EXECUTABLE}
                    -NoProfile
                    -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/Scripts/run_d4c_dx12_tier2_candidate.ps1"
                    -ModelViewer "$<TARGET_FILE:ModelViewer>"
                    -VisualGoldenValidation "$<TARGET_FILE:VisualGoldenValidation>"
                    -DX12Validation "$<TARGET_FILE:DX12Validation>"
                    -SourceRoot "${CMAKE_SOURCE_DIR}"
                    -ArtifactRoot "${CMAKE_BINARY_DIR}/M3Exit/$<CONFIG>/Task11D-D4c"
            )
            set_tests_properties(DX12GPUSceneTier2Candidate PROPERTIES
                LABELS "nightly;native;gpu;gpu-driven;tier2;candidate;gbv;parity;resize"
                TIMEOUT 600
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            )

            add_test(
                NAME DX12GPUSceneM3EvidenceSmoke
                COMMAND ${RVX_D4C_POWERSHELL_EXECUTABLE}
                    -NoProfile
                    -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/Scripts/run_m3_gpu_scene_exit.ps1"
                    -ModelViewer "$<TARGET_FILE:ModelViewer>"
                    -Model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
                    -OutputDirectory "${CMAKE_BINARY_DIR}/M3Exit/$<CONFIG>/Task11E"
                    -Workloads 100
            )
            set_tests_properties(DX12GPUSceneM3EvidenceSmoke PROPERTIES
                LABELS "nightly;native;gpu;gpu-driven;diagnostics;m3;non-gating"
                TIMEOUT 600
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            )
        endif()
    endif()

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

# Vulkan Direct ModelViewer smoke keeps the execution route backend-native and
# exports the same public policy measurement schema used by the DX12 fixtures.
if(RVX_ENABLE_VULKAN AND TARGET ModelViewer)
    set(RVX_MODELVIEWER_M2_VULKAN_DIRECT_REPORT
        "${CMAKE_BINARY_DIR}/M2Exit/$<CONFIG>/ModelViewerVulkanDirect.measurement.json")
    add_test(
        NAME ModelViewerVulkanDirectSmoke
        COMMAND $<TARGET_FILE:ModelViewer>
            --smoke
            --model "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/R7Triangle.gltf"
            --backend vulkan
            --width 320
            --height 180
            --frames 2
            --no-ibl
            --gpu-driven off
            --expect-gpu-driven-direct-ready
            --render-policy-report "${RVX_MODELVIEWER_M2_VULKAN_DIRECT_REPORT}"
            --validation
    )
    set_tests_properties(ModelViewerVulkanDirectSmoke PROPERTIES
        LABELS "native;vulkan;direct;m2"
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    # Task12 is an independent Vulkan GPU-driven Candidate runner.  It owns
    # its hermetic output directory and validates the real ModelViewer entry
    # point (direct, forced GPU, repeat, resize, and zero-visible) rather than
    # duplicating the DX12 Tier2 qualification semantics in CTest.
    find_program(RVX_TASK12_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(RVX_TASK12_POWERSHELL_EXECUTABLE AND TARGET GPUDrivenValidation)
        add_test(
            NAME VulkanGPUDrivenCandidate
            COMMAND ${RVX_TASK12_POWERSHELL_EXECUTABLE}
                -NoProfile
                -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/Scripts/run_task12_vulkan_candidate.ps1"
                -BuildDir "${CMAKE_BINARY_DIR}"
                -Configuration "$<CONFIG>"
                -ReportDirectory "${CMAKE_BINARY_DIR}/Task12Exit/$<CONFIG>"
                -ModelViewer "$<TARGET_FILE:ModelViewer>"
                -GPUDrivenValidation "$<TARGET_FILE:GPUDrivenValidation>"
        )
        set_tests_properties(VulkanGPUDrivenCandidate PROPERTIES
            LABELS "native;vulkan;gpu-driven;candidate;task12"
            TIMEOUT 900
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
    endif()
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
