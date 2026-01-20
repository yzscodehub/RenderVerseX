#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "RHI/RHIShader.h"
#include <string>

namespace RVX
{
    class OpenGLDevice;

    // =============================================================================
    // OpenGL Shader
    // Represents a compiled GLSL shader stage
    // =============================================================================
    class OpenGLShader : public RHIShader
    {
    public:
        // Create from GLSL source (normal path)
        OpenGLShader(OpenGLDevice* device, const RHIShaderDesc& desc, const std::string& glslSource);
        
        // Create from SPIR-V bytecode (when GL_ARB_gl_spirv is available)
        OpenGLShader(OpenGLDevice* device, const RHIShaderDesc& desc, bool useSpirvPath);
        
        ~OpenGLShader() override;

        // RHIShader interface
        RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<uint8>& GetBytecode() const override { return m_bytecode; }

        // OpenGL specific
        GLuint GetHandle() const { return m_shader; }
        const std::string& GetGLSLSource() const { return m_glslSource; }
        const std::string& GetEntryPoint() const { return m_entryPoint; }
        bool IsValid() const { return m_shader != 0; }

    private:
        bool CompileGLSL(const std::string& source);
        bool CompileSPIRV(const void* bytecode, size_t size);

        OpenGLDevice* m_device = nullptr;
        GLuint m_shader = 0;
        RHIShaderStage m_stage = RHIShaderStage::None;
        std::vector<uint8> m_bytecode;
        std::string m_glslSource;
        std::string m_entryPoint;
    };

    // =============================================================================
    // OpenGL Program
    // Links multiple shader stages into a program
    // =============================================================================
    class OpenGLProgram
    {
    public:
        OpenGLProgram(OpenGLDevice* device, const char* debugName = nullptr);
        ~OpenGLProgram();

        // Attach shaders and link
        void AttachShader(OpenGLShader* shader);
        bool Link();

        // Query
        GLuint GetHandle() const { return m_program; }
        bool IsLinked() const { return m_linked; }

        // Uniform locations (cached)
        GLint GetUniformLocation(const char* name);
        GLuint GetUniformBlockIndex(const char* name);
        GLuint GetShaderStorageBlockIndex(const char* name);

        // Set uniforms (convenience methods)
        void SetUniform1i(const char* name, int value);
        void SetUniform1f(const char* name, float value);
        void SetUniform2f(const char* name, float x, float y);
        void SetUniform3f(const char* name, float x, float y, float z);
        void SetUniform4f(const char* name, float x, float y, float z, float w);
        void SetUniformMatrix4f(const char* name, const float* matrix, bool transpose = false);

    private:
        OpenGLDevice* m_device = nullptr;
        GLuint m_program = 0;
        bool m_linked = false;
        std::string m_debugName;

        // Cached uniform locations
        std::unordered_map<std::string, GLint> m_uniformCache;
        std::unordered_map<std::string, GLuint> m_uniformBlockCache;
        std::unordered_map<std::string, GLuint> m_ssboBlockCache;
    };

} // namespace RVX
