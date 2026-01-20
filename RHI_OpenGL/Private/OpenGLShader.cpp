#include "OpenGLShader.h"
#include "OpenGLDevice.h"
#include "OpenGLConversions.h"
#include "Core/Log.h"
#include <sstream>

namespace RVX
{
    // =============================================================================
    // OpenGLShader Implementation
    // =============================================================================

    OpenGLShader::OpenGLShader(OpenGLDevice* device, const RHIShaderDesc& desc, const std::string& glslSource)
        : m_device(device)
        , m_stage(desc.stage)
        , m_glslSource(glslSource)
        , m_entryPoint(desc.entryPoint ? desc.entryPoint : "main")
    {
        // Store original bytecode if provided
        if (desc.bytecode && desc.bytecodeSize > 0)
        {
            m_bytecode.resize(desc.bytecodeSize);
            memcpy(m_bytecode.data(), desc.bytecode, desc.bytecodeSize);
        }

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        if (!CompileGLSL(glslSource))
        {
            RVX_RHI_ERROR("Failed to compile shader '{}'", GetDebugName());
        }
    }

    OpenGLShader::OpenGLShader(OpenGLDevice* device, const RHIShaderDesc& desc, bool useSpirvPath)
        : m_device(device)
        , m_stage(desc.stage)
        , m_entryPoint(desc.entryPoint ? desc.entryPoint : "main")
    {
        if (desc.bytecode && desc.bytecodeSize > 0)
        {
            m_bytecode.resize(desc.bytecodeSize);
            memcpy(m_bytecode.data(), desc.bytecode, desc.bytecodeSize);
        }

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        if (useSpirvPath && device->GetExtensions().GL_ARB_gl_spirv)
        {
            if (!CompileSPIRV(desc.bytecode, desc.bytecodeSize))
            {
                RVX_RHI_ERROR("Failed to compile SPIR-V shader '{}'", GetDebugName());
            }
        }
        else
        {
            RVX_RHI_ERROR("SPIR-V path requested but GL_ARB_gl_spirv not available");
        }
    }

    OpenGLShader::~OpenGLShader()
    {
        if (m_shader != 0)
        {
            glDeleteShader(m_shader);
            GL_DEBUG_UNTRACK(m_shader, GLResourceType::Shader);
            RVX_RHI_DEBUG("Destroyed OpenGL Shader #{} '{}'", m_shader, GetDebugName());
            m_shader = 0;
        }
    }

    bool OpenGLShader::CompileGLSL(const std::string& source)
    {
        GLenum shaderType = ToGLShaderType(m_stage);
        if (shaderType == 0)
        {
            RVX_RHI_ERROR("Unsupported shader stage: {}", static_cast<int>(m_stage));
            return false;
        }

        m_shader = glCreateShader(shaderType);
        if (m_shader == 0)
        {
            RVX_RHI_ERROR("Failed to create shader object");
            return false;
        }

        const char* sourcePtr = source.c_str();
        GLint sourceLength = static_cast<GLint>(source.length());
        
        GL_CHECK(glShaderSource(m_shader, 1, &sourcePtr, &sourceLength));
        GL_CHECK(glCompileShader(m_shader));

        // Check compilation status
        GLint compileStatus = 0;
        glGetShaderiv(m_shader, GL_COMPILE_STATUS, &compileStatus);

        if (compileStatus == GL_FALSE)
        {
            GLint logLength = 0;
            glGetShaderiv(m_shader, GL_INFO_LOG_LENGTH, &logLength);

            std::string infoLog(logLength, '\0');
            glGetShaderInfoLog(m_shader, logLength, nullptr, infoLog.data());

            RVX_RHI_ERROR("Shader '{}' compilation failed:\n{}", GetDebugName(), infoLog);
            
            // Log the source with line numbers for debugging
            std::istringstream stream(source);
            std::string line;
            int lineNum = 1;
            RVX_RHI_DEBUG("Shader source:");
            while (std::getline(stream, line))
            {
                RVX_RHI_DEBUG("{:4}: {}", lineNum++, line);
            }

            glDeleteShader(m_shader);
            m_shader = 0;
            return false;
        }

        // Set debug label
        OpenGLDebug::Get().SetShaderLabel(m_shader, GetDebugName().c_str());
        GL_DEBUG_TRACK(m_shader, GLResourceType::Shader, GetDebugName().c_str());

        RVX_RHI_DEBUG("Compiled OpenGL Shader #{} '{}' ({} lines)",
                     m_shader, GetDebugName(), std::count(source.begin(), source.end(), '\n') + 1);

        return true;
    }

    bool OpenGLShader::CompileSPIRV(const void* bytecode, size_t size)
    {
        GLenum shaderType = ToGLShaderType(m_stage);
        if (shaderType == 0)
        {
            RVX_RHI_ERROR("Unsupported shader stage: {}", static_cast<int>(m_stage));
            return false;
        }

        m_shader = glCreateShader(shaderType);
        if (m_shader == 0)
        {
            RVX_RHI_ERROR("Failed to create shader object");
            return false;
        }

        // Load SPIR-V binary
        GL_CHECK(glShaderBinary(1, &m_shader, GL_SHADER_BINARY_FORMAT_SPIR_V,
                               bytecode, static_cast<GLsizei>(size)));

        // Specialize the shader
        GL_CHECK(glSpecializeShader(m_shader, m_entryPoint.c_str(), 0, nullptr, nullptr));

        // Check compilation status
        GLint compileStatus = 0;
        glGetShaderiv(m_shader, GL_COMPILE_STATUS, &compileStatus);

        if (compileStatus == GL_FALSE)
        {
            GLint logLength = 0;
            glGetShaderiv(m_shader, GL_INFO_LOG_LENGTH, &logLength);

            std::string infoLog(logLength, '\0');
            glGetShaderInfoLog(m_shader, logLength, nullptr, infoLog.data());

            RVX_RHI_ERROR("SPIR-V shader '{}' specialization failed:\n{}", GetDebugName(), infoLog);

            glDeleteShader(m_shader);
            m_shader = 0;
            return false;
        }

        OpenGLDebug::Get().SetShaderLabel(m_shader, GetDebugName().c_str());
        GL_DEBUG_TRACK(m_shader, GLResourceType::Shader, GetDebugName().c_str());

        RVX_RHI_DEBUG("Compiled SPIR-V Shader #{} '{}' ({} bytes)",
                     m_shader, GetDebugName(), size);

        return true;
    }

    // =============================================================================
    // OpenGLProgram Implementation
    // =============================================================================

    OpenGLProgram::OpenGLProgram(OpenGLDevice* device, const char* debugName)
        : m_device(device)
        , m_debugName(debugName ? debugName : "")
    {
        m_program = glCreateProgram();
        
        if (m_program == 0)
        {
            RVX_RHI_ERROR("Failed to create program object");
            return;
        }

        if (!m_debugName.empty())
        {
            OpenGLDebug::Get().SetProgramLabel(m_program, m_debugName.c_str());
        }

        RVX_RHI_DEBUG("Created OpenGL Program #{} '{}'", m_program, m_debugName);
    }

    OpenGLProgram::~OpenGLProgram()
    {
        if (m_program != 0)
        {
            glDeleteProgram(m_program);
            GL_DEBUG_UNTRACK(m_program, GLResourceType::Program);
            RVX_RHI_DEBUG("Destroyed OpenGL Program #{} '{}'", m_program, m_debugName);
            m_program = 0;
        }
    }

    void OpenGLProgram::AttachShader(OpenGLShader* shader)
    {
        if (!shader || !shader->IsValid())
        {
            RVX_RHI_ERROR("Cannot attach invalid shader to program '{}'", m_debugName);
            return;
        }

        GL_CHECK(glAttachShader(m_program, shader->GetHandle()));
        RVX_RHI_DEBUG("Attached shader #{} to program #{}", shader->GetHandle(), m_program);
    }

    bool OpenGLProgram::Link()
    {
        GL_CHECK(glLinkProgram(m_program));

        GLint linkStatus = 0;
        glGetProgramiv(m_program, GL_LINK_STATUS, &linkStatus);

        if (linkStatus == GL_FALSE)
        {
            GLint logLength = 0;
            glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLength);

            std::string infoLog(logLength, '\0');
            glGetProgramInfoLog(m_program, logLength, nullptr, infoLog.data());

            RVX_RHI_ERROR("Program '{}' link failed:\n{}", m_debugName, infoLog);
            return false;
        }

        m_linked = true;
        GL_DEBUG_TRACK(m_program, GLResourceType::Program, m_debugName.c_str());

        // Log some program info
        GLint numActiveUniforms = 0, numActiveAttribs = 0, numActiveUniformBlocks = 0;
        glGetProgramiv(m_program, GL_ACTIVE_UNIFORMS, &numActiveUniforms);
        glGetProgramiv(m_program, GL_ACTIVE_ATTRIBUTES, &numActiveAttribs);
        glGetProgramiv(m_program, GL_ACTIVE_UNIFORM_BLOCKS, &numActiveUniformBlocks);

        RVX_RHI_DEBUG("Linked Program #{} '{}': {} uniforms, {} attribs, {} UBOs",
                     m_program, m_debugName, numActiveUniforms, numActiveAttribs, numActiveUniformBlocks);

        return true;
    }

    GLint OpenGLProgram::GetUniformLocation(const char* name)
    {
        auto it = m_uniformCache.find(name);
        if (it != m_uniformCache.end())
        {
            return it->second;
        }

        GLint location = glGetUniformLocation(m_program, name);
        m_uniformCache[name] = location;
        
        if (location == -1)
        {
            RVX_RHI_DEBUG("Uniform '{}' not found in program '{}'", name, m_debugName);
        }
        
        return location;
    }

    GLuint OpenGLProgram::GetUniformBlockIndex(const char* name)
    {
        auto it = m_uniformBlockCache.find(name);
        if (it != m_uniformBlockCache.end())
        {
            return it->second;
        }

        GLuint index = glGetUniformBlockIndex(m_program, name);
        m_uniformBlockCache[name] = index;
        
        if (index == GL_INVALID_INDEX)
        {
            RVX_RHI_DEBUG("Uniform block '{}' not found in program '{}'", name, m_debugName);
        }
        
        return index;
    }

    GLuint OpenGLProgram::GetShaderStorageBlockIndex(const char* name)
    {
        auto it = m_ssboBlockCache.find(name);
        if (it != m_ssboBlockCache.end())
        {
            return it->second;
        }

        GLuint index = glGetProgramResourceIndex(m_program, GL_SHADER_STORAGE_BLOCK, name);
        m_ssboBlockCache[name] = index;
        
        if (index == GL_INVALID_INDEX)
        {
            RVX_RHI_DEBUG("SSBO block '{}' not found in program '{}'", name, m_debugName);
        }
        
        return index;
    }

    void OpenGLProgram::SetUniform1i(const char* name, int value)
    {
        GLint loc = GetUniformLocation(name);
        if (loc != -1)
        {
            GL_CHECK(glProgramUniform1i(m_program, loc, value));
        }
    }

    void OpenGLProgram::SetUniform1f(const char* name, float value)
    {
        GLint loc = GetUniformLocation(name);
        if (loc != -1)
        {
            GL_CHECK(glProgramUniform1f(m_program, loc, value));
        }
    }

    void OpenGLProgram::SetUniform2f(const char* name, float x, float y)
    {
        GLint loc = GetUniformLocation(name);
        if (loc != -1)
        {
            GL_CHECK(glProgramUniform2f(m_program, loc, x, y));
        }
    }

    void OpenGLProgram::SetUniform3f(const char* name, float x, float y, float z)
    {
        GLint loc = GetUniformLocation(name);
        if (loc != -1)
        {
            GL_CHECK(glProgramUniform3f(m_program, loc, x, y, z));
        }
    }

    void OpenGLProgram::SetUniform4f(const char* name, float x, float y, float z, float w)
    {
        GLint loc = GetUniformLocation(name);
        if (loc != -1)
        {
            GL_CHECK(glProgramUniform4f(m_program, loc, x, y, z, w));
        }
    }

    void OpenGLProgram::SetUniformMatrix4f(const char* name, const float* matrix, bool transpose)
    {
        GLint loc = GetUniformLocation(name);
        if (loc != -1)
        {
            GL_CHECK(glProgramUniformMatrix4fv(m_program, loc, 1, transpose ? GL_TRUE : GL_FALSE, matrix));
        }
    }

} // namespace RVX
