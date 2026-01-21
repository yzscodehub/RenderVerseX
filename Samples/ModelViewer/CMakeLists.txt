# =============================================================================
# ModelViewer Sample
# =============================================================================
# Demonstrates the integrated Resource/Scene/Render pipeline
# =============================================================================

add_executable(ModelViewer main.cpp)

target_link_libraries(ModelViewer PRIVATE
    RVX::Core
    RVX::Engine
    RVX::Render
    RVX::Runtime
    RVX::Resource
    RVX::Scene
    RVX::World
    RVX::RHI
    RVX::ShaderCompiler
    RVX::HAL
)

# Link all enabled backends
if(RVX_ENABLE_DX11)
    target_link_libraries(ModelViewer PRIVATE RVX::RHI_DX11)
endif()
if(RVX_ENABLE_DX12)
    target_link_libraries(ModelViewer PRIVATE RVX::RHI_DX12)
endif()
if(RVX_ENABLE_VULKAN)
    target_link_libraries(ModelViewer PRIVATE RVX::RHI_Vulkan)
endif()
if(RVX_ENABLE_METAL)
    target_link_libraries(ModelViewer PRIVATE RVX::RHI_Metal)
endif()
if(RVX_ENABLE_OPENGL)
    target_link_libraries(ModelViewer PRIVATE RVX::RHI_OpenGL)
endif()

# Set working directory for debugging
set_target_properties(ModelViewer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
)
