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
)

target_include_directories(RVX_TestCommon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(RVX_TestCommon PUBLIC
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
    rvx_add_gtest(
        NAME DX12Validation
        SOURCES DX12Validation/main.cpp
        LIBS
            RVX::Core
            RVX::RenderGraph
            RVX::RHI
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
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
            RVX::RenderGraph
            RVX::RHI
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
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
            ${RVX_ALL_BACKENDS}
            glfw
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
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
rvx_add_gtest(
    NAME ResourceInstantiationValidation
    SOURCES ResourceInstantiationValidation/main.cpp
    LIBS
        RVX::Core
        RVX::Resource
        RVX::Scene
        RVX::World
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
    INCLUDES
        ${CMAKE_SOURCE_DIR}/Render/Include
        ${CMAKE_SOURCE_DIR}/RHI/Include
    LABELS unit
    TIMEOUT 120
)

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
