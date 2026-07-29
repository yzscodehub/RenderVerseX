# =============================================================================
# BasicRHI Sample - Direct RHI fundamentals
# =============================================================================
# Demonstrates:
# - Direct RHI device and swapchain setup
# - RenderGraph for automatic barrier management
# - Basic triangle/textured quad/cube assets consolidated under one sample
# =============================================================================
add_executable(BasicRHI
    main.cpp
)

target_link_libraries(BasicRHI PRIVATE
    RVX::Core
    RVX::RHI
    RVX::RHI_BackendFactory
    RVX::Render          # Includes RenderGraph
    RVX::Runtime         # Camera
    RVX::SampleCommon
    RVX::ShaderCompiler
    RVX::HAL
    glfw
)

# Link enabled backends
if(RVX_ENABLE_DX11)
    target_link_libraries(BasicRHI PRIVATE RVX::RHI_DX11)
endif()

if(RVX_ENABLE_DX12)
    target_link_libraries(BasicRHI PRIVATE RVX::RHI_DX12)
endif()

if(RVX_ENABLE_VULKAN)
    target_link_libraries(BasicRHI PRIVATE RVX::RHI_Vulkan)
endif()

if(RVX_ENABLE_METAL)
    target_link_libraries(BasicRHI PRIVATE RVX::RHI_Metal)
endif()

if(RVX_ENABLE_OPENGL)
    target_link_libraries(BasicRHI PRIVATE RVX::RHI_OpenGL)
endif()

# Copy shaders to output directory
add_custom_command(TARGET BasicRHI POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders
    $<TARGET_FILE_DIR:BasicRHI>/Shaders
)
