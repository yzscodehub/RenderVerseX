# =============================================================================
# ComputeDemo Sample - Demonstrates compute shader usage
# =============================================================================

add_executable(ComputeDemo main.cpp)

target_link_libraries(ComputeDemo PRIVATE
    RVX::Core
    RVX::RHI
    RVX::RenderGraph
    RVX::ShaderCompiler
    glfw
)

# Link all enabled backends
if(RVX_ENABLE_DX11)
    target_link_libraries(ComputeDemo PRIVATE RVX::RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    target_link_libraries(ComputeDemo PRIVATE RVX::RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    target_link_libraries(ComputeDemo PRIVATE RVX::RHI_Vulkan)
endif()
if(RVX_ENABLE_METAL)
    target_link_libraries(ComputeDemo PRIVATE RVX::RHI_Metal)
endif()
if(RVX_ENABLE_OPENGL)
    target_link_libraries(ComputeDemo PRIVATE RVX::RHI_OpenGL)
endif()

# Copy shaders to output directory
set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
set(SHADER_DST "$<TARGET_FILE_DIR:ComputeDemo>/Shaders")

add_custom_command(TARGET ComputeDemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DST}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SHADER_SRC}/ProceduralTexture.hlsl"
        "${SHADER_SRC}/FullscreenQuad.hlsl"
        ${SHADER_DST}
    COMMENT "Copying ComputeDemo shaders..."
)
