# =============================================================================
# Tests - Validation test suites
# =============================================================================

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
        RVX::RHI_DX12
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
        RVX::RHI_Vulkan
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
        RVX::RHI_DX11
        glfw
    )
endif()

# RenderGraph Validation Tests
add_executable(RenderGraphValidation
    RenderGraphValidation/main.cpp
)
target_link_libraries(RenderGraphValidation PRIVATE
    RVX_TestFramework
    RVX::RenderGraph
)

# Copy test shaders
file(GLOB TEST_SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.hlsl")
foreach(SHADER ${TEST_SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    configure_file(${SHADER} ${CMAKE_CURRENT_BINARY_DIR}/Shaders/${SHADER_NAME} COPYONLY)
endforeach()
