#pragma once

#include "Core/Types.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace RVX
{
    // =============================================================================
    // OpenGL Extensions Detection
    // =============================================================================
    struct OpenGLExtensions
    {
        bool GL_ARB_gl_spirv = false;               // OpenGL 4.6: native SPIR-V shaders
        bool GL_ARB_bindless_texture = false;       // Bindless textures
        bool GL_ARB_shader_draw_parameters = false; // gl_DrawID etc.
        bool GL_ARB_indirect_parameters = false;    // Indirect draw parameters
        bool GL_ARB_buffer_storage = false;         // Persistent mapping
        bool GL_ARB_direct_state_access = false;    // DSA (core in 4.5)
        bool GL_ARB_texture_view = false;           // Texture views
        bool GL_ARB_multi_bind = false;             // Multi-bind
        bool GL_ARB_separate_shader_objects = false;// Separate shader objects
        bool GL_KHR_debug = false;                  // Debug output
        bool GL_NV_mesh_shader = false;             // NVIDIA Mesh Shader
    };

    // =============================================================================
    // OpenGL Error Checking
    // =============================================================================
#ifdef RVX_GL_DEBUG
    inline void GLCheckError(const char* file, int line)
    {
        GLenum err;
        while ((err = glGetError()) != GL_NO_ERROR)
        {
            const char* errStr = "Unknown";
            switch (err)
            {
                case GL_INVALID_ENUM:      errStr = "GL_INVALID_ENUM"; break;
                case GL_INVALID_VALUE:     errStr = "GL_INVALID_VALUE"; break;
                case GL_INVALID_OPERATION: errStr = "GL_INVALID_OPERATION"; break;
                case GL_OUT_OF_MEMORY:     errStr = "GL_OUT_OF_MEMORY"; break;
                case GL_INVALID_FRAMEBUFFER_OPERATION: errStr = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
            }
            RVX_RHI_ERROR("OpenGL Error {} at {}:{}", errStr, file, line);
        }
    }
    #define GL_CHECK(call) do { call; GLCheckError(__FILE__, __LINE__); } while(0)
#else
    #define GL_CHECK(call) call
#endif

    // =============================================================================
    // Frame Count for Triple Buffering
    // =============================================================================
    constexpr uint32 RVX_GL_MAX_FRAME_COUNT = 3;

} // namespace RVX
