# =============================================================================
# RHI_OpenGL Module - OpenGL backend implementation
# =============================================================================
add_library(RVX_RHI_OpenGL STATIC)

target_sources(RVX_RHI_OpenGL PRIVATE
    # Core
    Private/OpenGLCommon.cpp
    Private/OpenGLDebug.cpp
    Private/OpenGLDevice.cpp
    Private/OpenGLStateCache.cpp
    Private/OpenGLDeletionQueue.cpp
    Private/OpenGLCaches.cpp
    
    # Resources
    Private/OpenGLResources.cpp
    
    # Shader & Pipeline
    Private/OpenGLShader.cpp
    Private/OpenGLPipeline.cpp
    Private/OpenGLDescriptor.cpp
    
    # Command
    Private/OpenGLCommandContext.cpp
    
    # SwapChain & Sync
    Private/OpenGLSwapChain.cpp
    Private/OpenGLSync.cpp
)

target_include_directories(RVX_RHI_OpenGL PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(RVX_RHI_OpenGL PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(RVX_RHI_OpenGL PUBLIC
    RVX::RHI
    RVX::Core
    RVX::ShaderCompiler
)

# OpenGL Loader (glad)
find_package(glad CONFIG REQUIRED)
target_link_libraries(RVX_RHI_OpenGL PRIVATE glad::glad)

# GLFW for context/window
target_link_libraries(RVX_RHI_OpenGL PRIVATE glfw)

# Platform-specific OpenGL libraries
if(WIN32)
    target_link_libraries(RVX_RHI_OpenGL PRIVATE opengl32)
elseif(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(RVX_RHI_OpenGL PRIVATE OpenGL::GL)
endif()

# Compile definitions
target_compile_definitions(RVX_RHI_OpenGL PRIVATE
    RVX_OPENGL_BACKEND=1
)

# Debug mode enables detailed GL error checking
target_compile_definitions(RVX_RHI_OpenGL PRIVATE
    $<$<CONFIG:Debug>:RVX_GL_DEBUG=1>
)

add_library(RVX::RHI_OpenGL ALIAS RVX_RHI_OpenGL)
