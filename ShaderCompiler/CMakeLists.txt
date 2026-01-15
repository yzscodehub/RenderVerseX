# =============================================================================
# ShaderCompiler Module - HLSL compilation and reflection
# =============================================================================
add_library(RVX_ShaderCompiler STATIC)

target_sources(RVX_ShaderCompiler PRIVATE
    Private/ShaderCompiler.cpp
    Private/ShaderManager.cpp
    Private/ShaderLayout.cpp
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
        d3dcompiler
    )
endif()

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
