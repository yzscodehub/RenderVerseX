# =============================================================================
# Tests - Validation test suites
# =============================================================================

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

# DX12 Validation Tests
if(RVX_ENABLE_DX12)
    add_executable(DX12Validation
        DX12Validation/main.cpp
    )
    target_link_libraries(DX12Validation PRIVATE
        RVX_TestFramework
        RVX::RenderGraph
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

# Render pass binding validation tests
add_executable(RenderPassBindingValidation
    RenderPassBindingValidation/main.cpp
)
target_compile_definitions(RenderPassBindingValidation PRIVATE
    RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_include_directories(RenderPassBindingValidation PRIVATE
    ${CMAKE_SOURCE_DIR}/Render/Include
    ${CMAKE_SOURCE_DIR}/RHI/Include
    ${CMAKE_SOURCE_DIR}/ShaderCompiler/Include
)
target_link_libraries(RenderPassBindingValidation PRIVATE
    RVX_TestFramework
)
target_compile_features(RenderPassBindingValidation PRIVATE cxx_std_20)

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
)
target_compile_features(ActorComponentValidation PRIVATE cxx_std_20)

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

# RHI DX12 backend source validation tests
add_executable(RHIDX12SourceValidation
    RHIDX12SourceValidation/main.cpp
)
target_compile_definitions(RHIDX12SourceValidation PRIVATE
    RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(RHIDX12SourceValidation PRIVATE
    RVX_TestFramework
)
target_compile_features(RHIDX12SourceValidation PRIVATE cxx_std_20)

# RHI DX11 backend source validation tests
add_executable(RHIDX11SourceValidation
    RHIDX11SourceValidation/main.cpp
)
target_compile_definitions(RHIDX11SourceValidation PRIVATE
    RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(RHIDX11SourceValidation PRIVATE
    RVX_TestFramework
)
target_compile_features(RHIDX11SourceValidation PRIVATE cxx_std_20)

# RHI Vulkan backend source validation tests
add_executable(RHIVulkanSourceValidation
    RHIVulkanSourceValidation/main.cpp
)
target_compile_definitions(RHIVulkanSourceValidation PRIVATE
    RVX_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(RHIVulkanSourceValidation PRIVATE
    RVX_TestFramework
)
target_compile_features(RHIVulkanSourceValidation PRIVATE cxx_std_20)

# Cross-Backend Validation Tests (requires both DX12 and Vulkan)
if(RVX_ENABLE_DX12 AND RVX_ENABLE_VULKAN)
    add_executable(CrossBackendValidation
        CrossBackendValidation/main.cpp
    )
    target_link_libraries(CrossBackendValidation PRIVATE
        RVX_TestFramework
        RVX::RenderGraph
        ${RVX_ALL_BACKENDS}
        glfw
    )
endif()

# System Integration Test (Spatial + Scene + Asset)
add_executable(SystemIntegrationTest
    SystemIntegration/main.cpp
)
target_link_libraries(SystemIntegrationTest PRIVATE
    RVX::Core
    Spatial
    RVX::Scene
    RVX::Resource
    RVX::Audio
)
target_compile_features(SystemIntegrationTest PRIVATE cxx_std_20)

# Copy test shaders
file(GLOB TEST_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsl")
foreach(SHADER ${TEST_SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    configure_file(${SHADER} ${CMAKE_CURRENT_BINARY_DIR}/Shaders/${SHADER_NAME} COPYONLY)
endforeach()
