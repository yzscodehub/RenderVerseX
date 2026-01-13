# =============================================================================
# ShaderCompiler Module - HLSL compilation and reflection
# =============================================================================
add_library(RVX_ShaderCompiler STATIC)

target_sources(RVX_ShaderCompiler PRIVATE
    Private/ShaderCompiler.cpp
    Private/DXCCompiler.cpp
    Private/ShaderReflection.cpp
    Private/ShaderCache.cpp
    Private/ShaderPermutationCache.cpp
    Private/DX11SlotMapper.cpp
)

target_include_directories(RVX_ShaderCompiler PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_ShaderCompiler PUBLIC
    RVX::RHI
)

if(WIN32)
    target_link_libraries(RVX_ShaderCompiler PRIVATE
        Microsoft::DirectXShaderCompiler
    )
endif()

if(RVX_ENABLE_VULKAN)
    target_link_libraries(RVX_ShaderCompiler PRIVATE
        unofficial::spirv-reflect::spirv-reflect
    )
endif()

add_library(RVX::ShaderCompiler ALIAS RVX_ShaderCompiler)
