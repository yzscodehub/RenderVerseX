# =============================================================================
# Showcase Samples - integrated engine feature demonstrations
# =============================================================================

function(rvx_add_engine_showcase TARGET_NAME SOURCE_FILE DEFAULT_MODE)
    add_executable(${TARGET_NAME}
        ${SOURCE_FILE}
    )

    target_compile_definitions(${TARGET_NAME} PRIVATE
        RVX_FEATURE_SHOWCASE_DEFAULT_MODE=${DEFAULT_MODE}
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        RVX::Core
        RVX::Engine
        RVX::Runtime
        RVX::Render
        RVX::Resource
        RVX::Scene
        RVX::World
        RVX::RHI
        RVX::ShaderCompiler
        RVX::HAL
    )

    if(RVX_ENABLE_DX11)
        target_link_libraries(${TARGET_NAME} PRIVATE RVX::RHI_DX11)
    endif()
    if(RVX_ENABLE_DX12)
        target_link_libraries(${TARGET_NAME} PRIVATE RVX::RHI_DX12)
    endif()
    if(RVX_ENABLE_VULKAN)
        target_link_libraries(${TARGET_NAME} PRIVATE RVX::RHI_Vulkan)
    endif()
    if(RVX_ENABLE_METAL)
        target_link_libraries(${TARGET_NAME} PRIVATE RVX::RHI_Metal)
    endif()
    if(RVX_ENABLE_OPENGL)
        target_link_libraries(${TARGET_NAME} PRIVATE RVX::RHI_OpenGL)
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endfunction()

add_subdirectory(ModelViewer)
add_subdirectory(RenderingShowcase)
add_subdirectory(SceneInteractionShowcase)
add_subdirectory(TerrainWaterShowcase)
add_subdirectory(ParticleFXShowcase)
