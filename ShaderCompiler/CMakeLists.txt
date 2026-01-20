# =============================================================================
# ShaderCompiler Module - HLSL compilation and reflection
# =============================================================================
add_library(RVX_ShaderCompiler STATIC)

# Common sources
target_sources(RVX_ShaderCompiler PRIVATE
    Private/ShaderCompiler.cpp
    Private/ShaderManager.cpp
    Private/ShaderLayout.cpp
    Private/ShaderReflection.cpp
    Private/ShaderCache.cpp
    Private/ShaderPermutationCache.cpp
)

# Platform-specific DXC compiler implementation
if(WIN32)
    target_sources(RVX_ShaderCompiler PRIVATE
        Private/DXCCompiler.cpp
        Private/DX11SlotMapper.cpp
        Private/SPIRVCrossTranslator.cpp
    )
elseif(APPLE)
    target_sources(RVX_ShaderCompiler PRIVATE
        Private/DXCCompiler_Apple.cpp
        Private/SPIRVCrossTranslator.cpp
    )
endif()

target_include_directories(RVX_ShaderCompiler PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_ShaderCompiler PUBLIC
    RVX::RHI
)

# Windows: Link DirectX Shader Compiler, D3DCompiler, and SPIRV-Cross for OpenGL support
if(WIN32)
    target_link_libraries(RVX_ShaderCompiler PRIVATE
        Microsoft::DirectXShaderCompiler
        d3dcompiler
        spirv-cross-core
        spirv-cross-glsl
    )
endif()

# Apple: Use glslang for HLSL -> SPIR-V, then SPIRV-Cross for SPIR-V -> MSL
if(APPLE)
    # Link glslang for HLSL compilation (HLSL support is built into main library)
    target_link_libraries(RVX_ShaderCompiler PRIVATE
        glslang::glslang
        glslang::glslang-default-resource-limits
        glslang::SPIRV
    )
    
    # Link SPIRV-Cross libraries for MSL translation
    target_link_libraries(RVX_ShaderCompiler PRIVATE
        spirv-cross-core
        spirv-cross-glsl
        spirv-cross-hlsl
        spirv-cross-msl
    )
endif()

# Vulkan: Link SPIRV-Reflect for shader reflection
if(RVX_ENABLE_VULKAN)
    # vcpkg's spirv-reflect port exports `unofficial::spirv-reflect` (not
    # `unofficial::spirv-reflect::spirv-reflect`). Keep a small compatibility
    # shim in case target names differ across environments.
    if(TARGET unofficial::spirv-reflect)
        target_link_libraries(RVX_ShaderCompiler PRIVATE unofficial::spirv-reflect)
    elseif(TARGET unofficial::spirv-reflect::spirv-reflect)
        target_link_libraries(RVX_ShaderCompiler PRIVATE unofficial::spirv-reflect::spirv-reflect)
    elseif(TARGET spirv-reflect::spirv-reflect)
        target_link_libraries(RVX_ShaderCompiler PRIVATE spirv-reflect::spirv-reflect)
    else()
        message(FATAL_ERROR "SPIRV-Reflect CMake target not found. Expected one of: unofficial::spirv-reflect, unofficial::spirv-reflect::spirv-reflect, spirv-reflect::spirv-reflect")
    endif()
endif()

add_library(RVX::ShaderCompiler ALIAS RVX_ShaderCompiler)
