# =============================================================================
# TexturedQuad Sample - Demonstrates texture sampling
# =============================================================================

add_executable(TexturedQuad main.cpp)

target_link_libraries(TexturedQuad PRIVATE
    RVX::Core
    RVX::RHI
    RVX::RenderGraph
    RVX::ShaderCompiler
    glfw
)

# Link all enabled backends
if(RVX_ENABLE_DX11)
    target_link_libraries(TexturedQuad PRIVATE RVX::RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    target_link_libraries(TexturedQuad PRIVATE RVX::RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    target_link_libraries(TexturedQuad PRIVATE RVX::RHI_Vulkan)
endif()
if(RVX_ENABLE_METAL)
    target_link_libraries(TexturedQuad PRIVATE RVX::RHI_Metal)
endif()

# Copy shaders to output directory
set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
set(SHADER_DST "$<TARGET_FILE_DIR:TexturedQuad>/Shaders")

add_custom_command(TARGET TexturedQuad POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DST}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SHADER_SRC}/TexturedQuad.hlsl"
        ${SHADER_DST}
    COMMENT "Copying TexturedQuad shaders..."
)
