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
rvx_add_gtest(
    NAME RenderGraphValidation
    SOURCES
        RenderGraphValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraph.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraphCompiler.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/RenderGraphExecutor.cpp
    LIBS
        RVX::Core
        RVX::RHI
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
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
    LIBS
        RVX::Core
        RVX::RHI
        RVX::Resource
        RVX::Scene
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
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
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
    LABELS unit
    TIMEOUT 120
)

# Render Scene Validation Tests
rvx_add_gtest(
    NAME RenderSceneValidation
    SOURCES
        RenderSceneValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderScene.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Renderer/RenderSceneCollector.cpp
    LIBS
        RVX::Core
        RVX::Runtime
        RVX::World
        RVX::Resource
        RVX::Scene
    INCLUDES
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
        RVX::Render
        RVX::Scene
    LABELS unit
    TIMEOUT 120
)

# Actor component validation tests
rvx_add_gtest(
    NAME ActorComponentValidation
    SOURCES ActorComponentValidation/main.cpp
    LIBS
        RVX::Core
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
rvx_add_gtest(
    NAME ResourceViewCacheValidation
    SOURCES
        ResourceViewCacheValidation/main.cpp
        ${CMAKE_SOURCE_DIR}/Render/Private/Graph/ResourceViewCache.cpp
    LIBS
        RVX::Core
        RVX::RHI
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

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
# CTest registration (labels: unit | gpu)
#   unit -> CPU-only, the CI gate set      (ctest -L unit)
#   gpu  -> needs a real device, headless-skip
# =============================================================================

# --- unit (CPU-only) ---
add_test(NAME ResourceInstantiationValidation COMMAND ResourceInstantiationValidation)
set_tests_properties(
    ResourceInstantiationValidation
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
