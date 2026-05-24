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

# Test Framework library
add_library(RVX_TestFramework STATIC
    TestFramework/TestRunner.cpp
    TestFramework/ImageCompare.cpp
)

target_include_directories(RVX_TestFramework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(RVX_TestFramework PUBLIC
    RVX::Core
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

# Core debug configuration validation tests
rvx_add_gtest(
    NAME CoreDebugConfigValidation
    SOURCES CoreDebugConfigValidation/main.cpp
    LIBS RVX::Core
    DEFINES $<$<CONFIG:Debug>:RVX_EXPECT_DEBUG_CONFIG=1>
    LABELS unit
    TIMEOUT 30
)

# DX12 Validation Tests
if(RVX_ENABLE_DX12)
    add_executable(DX12Validation
        DX12Validation/main.cpp
    )
    target_link_libraries(DX12Validation PRIVATE
        RVX_TestFramework
        RVX::RenderGraph
        RVX::RHI
        ${RVX_ALL_BACKENDS}
        glfw
    )
endif()

# Vulkan Validation Tests
if(RVX_ENABLE_VULKAN)
    add_executable(VulkanValidation
        VulkanValidation/main.cpp
    )
    target_link_libraries(VulkanValidation PRIVATE
        RVX_TestFramework
        RVX::RenderGraph
        RVX::RHI
        ${RVX_ALL_BACKENDS}
        glfw
    )
endif()

# DX11 Validation Tests
if(RVX_ENABLE_DX11)
    add_executable(DX11Validation
        DX11Validation/main.cpp
    )
    target_link_libraries(DX11Validation PRIVATE
        RVX_TestFramework
        RVX::RenderGraph
        RVX::RHI
        ${RVX_ALL_BACKENDS}
        glfw
    )
endif()

# RenderGraph Validation Tests
add_executable(RenderGraphValidation
    RenderGraphValidation/main.cpp
    ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraph.cpp
    ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraphCompiler.cpp
    ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraphExecutor.cpp
)
target_include_directories(RenderGraphValidation PRIVATE
    ${CMAKE_SOURCE_DIR}/Render/Include
)
target_link_libraries(RenderGraphValidation PRIVATE
    RVX_TestFramework
    RVX::RHI
)
target_compile_features(RenderGraphValidation PRIVATE cxx_std_20)

# GPU Resource Manager Validation Tests
add_executable(GPUResourceManagerValidation
    GPUResourceManagerValidation/main.cpp
    ${CMAKE_SOURCE_DIR}/Render/Private/GPUResourceManager.cpp
    ${CMAKE_SOURCE_DIR}/Render/Private/GPUUploadService.cpp
)
target_include_directories(GPUResourceManagerValidation PRIVATE
    ${CMAKE_SOURCE_DIR}/Render/Include
)
target_link_libraries(GPUResourceManagerValidation PRIVATE
    RVX_TestFramework
    RVX::RHI
    RVX::Resource
    RVX::Scene
)
target_compile_features(GPUResourceManagerValidation PRIVATE cxx_std_20)

# GPU Upload Service Validation Tests
add_executable(GPUUploadServiceValidation
    GPUUploadServiceValidation/main.cpp
    ${CMAKE_SOURCE_DIR}/Render/Private/GPUUploadService.cpp
)
target_include_directories(GPUUploadServiceValidation PRIVATE
    ${CMAKE_SOURCE_DIR}/Render/Include
)
target_link_libraries(GPUUploadServiceValidation PRIVATE
    RVX_TestFramework
    RVX::RHI
)
target_compile_features(GPUUploadServiceValidation PRIVATE cxx_std_20)

# Render Scene Validation Tests
add_executable(RenderSceneValidation
    RenderSceneValidation/main.cpp
    ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderScene.cpp
    ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderSceneCollector.cpp
)
target_include_directories(RenderSceneValidation PRIVATE
    ${CMAKE_SOURCE_DIR}/Render/Include
    ${CMAKE_SOURCE_DIR}/Render/Private/Renderer
    ${CMAKE_SOURCE_DIR}/Resource/Include
    ${CMAKE_SOURCE_DIR}/Scene/Include
    ${CMAKE_SOURCE_DIR}/World/Include
)
target_link_libraries(RenderSceneValidation PRIVATE
    RVX_TestFramework
    RVX::Runtime
    RVX::World
    RVX::Resource
    RVX::Scene
)
target_compile_features(RenderSceneValidation PRIVATE cxx_std_20)

# Material system validation tests
add_executable(MaterialSystemValidation
    MaterialSystemValidation/main.cpp
)
target_compile_definitions(MaterialSystemValidation PRIVATE
    RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_include_directories(MaterialSystemValidation PRIVATE
    ${CMAKE_SOURCE_DIR}/Render/Include
    ${CMAKE_SOURCE_DIR}/Scene/Include
)
target_link_libraries(MaterialSystemValidation PRIVATE
    RVX_TestFramework
    RVX::Render
    RVX::Scene
)
target_compile_features(MaterialSystemValidation PRIVATE cxx_std_20)

# Actor component validation tests
add_executable(ActorComponentValidation
    ActorComponentValidation/main.cpp
)
target_include_directories(ActorComponentValidation PRIVATE
    ${CMAKE_SOURCE_DIR}/Resource/Include
)
target_link_libraries(ActorComponentValidation PRIVATE
    RVX_TestFramework
    RVX::Scene
    RVX::Resource
    RVX::World
)
target_compile_features(ActorComponentValidation PRIVATE cxx_std_20)

# Spatial component validation tests
add_executable(SpatialComponentValidation
    SpatialComponentValidation/main.cpp
)
target_link_libraries(SpatialComponentValidation PRIVATE
    RVX_TestFramework
    RVX::World
    RVX::Scene
    RVX::Resource
)
target_compile_features(SpatialComponentValidation PRIVATE cxx_std_20)

# Resource instantiation validation tests
add_executable(ResourceInstantiationValidation
    ResourceInstantiationValidation/main.cpp
)
target_link_libraries(ResourceInstantiationValidation PRIVATE
    RVX_TestFramework
    RVX::Resource
    RVX::Scene
    RVX::World
)
target_compile_features(ResourceInstantiationValidation PRIVATE cxx_std_20)

# Resource view cache validation tests
add_executable(ResourceViewCacheValidation
    ResourceViewCacheValidation/main.cpp
    ${CMAKE_SOURCE_DIR}/Render/Private/Graph/ResourceViewCache.cpp
)
target_compile_definitions(ResourceViewCacheValidation PRIVATE
    RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_include_directories(ResourceViewCacheValidation PRIVATE
    ${CMAKE_SOURCE_DIR}/Render/Include
    ${CMAKE_SOURCE_DIR}/RHI/Include
)
target_link_libraries(ResourceViewCacheValidation PRIVATE
    RVX_TestFramework
    RVX::RHI
)
target_compile_features(ResourceViewCacheValidation PRIVATE cxx_std_20)

# Cross-Backend Validation Tests (requires both DX12 and Vulkan)
if(RVX_ENABLE_DX12 AND RVX_ENABLE_VULKAN)
    add_executable(CrossBackendValidation
        CrossBackendValidation/main.cpp
    )
    target_link_libraries(CrossBackendValidation PRIVATE
        RVX_TestFramework
        RVX::RenderGraph
        RVX::RHI
        ${RVX_ALL_BACKENDS}
        glfw
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

# =============================================================================
# CTest registration (labels: unit | gpu | lint)
#   unit -> CPU-only, the CI gate set      (ctest -L unit)
#   gpu  -> needs a real device, headless-skip
#   lint -> source-pattern checks, not behavioral (see Task 0.3)
# =============================================================================

# --- unit (CPU-only) ---
add_test(NAME RenderGraphValidation         COMMAND RenderGraphValidation)
add_test(NAME GPUResourceManagerValidation  COMMAND GPUResourceManagerValidation)
add_test(NAME GPUUploadServiceValidation    COMMAND GPUUploadServiceValidation)
add_test(NAME RenderSceneValidation         COMMAND RenderSceneValidation)
add_test(NAME MaterialSystemValidation      COMMAND MaterialSystemValidation)
add_test(NAME ActorComponentValidation      COMMAND ActorComponentValidation)
add_test(NAME SpatialComponentValidation    COMMAND SpatialComponentValidation)
add_test(NAME ResourceInstantiationValidation COMMAND ResourceInstantiationValidation)
add_test(NAME ResourceViewCacheValidation   COMMAND ResourceViewCacheValidation)
set_tests_properties(
    RenderGraphValidation GPUResourceManagerValidation GPUUploadServiceValidation
    RenderSceneValidation MaterialSystemValidation
    ActorComponentValidation SpatialComponentValidation ResourceInstantiationValidation
    ResourceViewCacheValidation
    PROPERTIES LABELS "unit" TIMEOUT 120)

# --- gpu (needs a real device; headless CI skips these) ---
if(RVX_ENABLE_DX12)
    add_test(NAME DX12Validation COMMAND DX12Validation)
    set_tests_properties(DX12Validation PROPERTIES LABELS "gpu" TIMEOUT 120)
endif()
if(RVX_ENABLE_VULKAN)
    add_test(NAME VulkanValidation COMMAND VulkanValidation)
    set_tests_properties(VulkanValidation PROPERTIES LABELS "gpu" TIMEOUT 120)
endif()
if(RVX_ENABLE_DX11)
    add_test(NAME DX11Validation COMMAND DX11Validation)
    set_tests_properties(DX11Validation PROPERTIES LABELS "gpu" TIMEOUT 120)
endif()
if(RVX_ENABLE_DX12 AND RVX_ENABLE_VULKAN)
    add_test(NAME CrossBackendValidation COMMAND CrossBackendValidation)
    set_tests_properties(CrossBackendValidation PROPERTIES LABELS "gpu" TIMEOUT 180)
endif()
