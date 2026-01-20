#include "OpenGLDebug.h"
#include "Core/Log.h"
#include <sstream>

namespace RVX
{
    // =============================================================================
    // OpenGLDebug Implementation
    // =============================================================================

    void OpenGLDebug::Initialize(bool enableDebugOutput)
    {
        m_debugEnabled = enableDebugOutput;
        
        // Check for GL_KHR_debug extension
        GLint numExtensions;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
        for (GLint i = 0; i < numExtensions; ++i)
        {
            const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
            if (ext && strcmp(ext, "GL_KHR_debug") == 0)
            {
                m_hasKHRDebug = true;
                break;
            }
        }

        if (m_debugEnabled)
        {
            RVX_RHI_INFO("OpenGL Debug System initialized (KHR_debug: {})", 
                        m_hasKHRDebug ? "available" : "not available");
        }
    }

    void OpenGLDebug::Shutdown()
    {
        // Log any remaining tracked resources (potential leaks)
        if (m_debugEnabled && !m_trackedResources.empty())
        {
            RVX_RHI_WARN("OpenGL Debug: {} resources still tracked at shutdown (potential leaks):",
                        m_trackedResources.size());
            DumpTrackedResources();
        }

        m_trackedResources.clear();
    }

    void OpenGLDebug::BeginFrame(uint64 frameIndex)
    {
        m_currentFrame = frameIndex;
        m_stats.ResetFrameCounters();
    }

    void OpenGLDebug::EndFrame()
    {
        if (m_debugEnabled)
        {
            // Optionally log stats every N frames
            // LogFrameStats();
        }
    }

    void OpenGLDebug::TrackResource(GLuint handle, GLResourceType type, const char* debugName,
                                    const std::source_location& loc)
    {
        if (!m_debugEnabled) return;

        std::lock_guard<std::mutex> lock(m_resourceMutex);
        
        GLResourceInfo info;
        info.handle = handle;
        info.type = type;
        info.debugName = debugName ? debugName : "";
        info.creationFile = loc.file_name();
        info.creationLine = static_cast<int>(loc.line());
        info.creationFrame = m_currentFrame;
        
        m_trackedResources[MakeResourceKey(handle, type)] = info;

        RVX_RHI_DEBUG("GL Resource Created: {} #{} '{}' at {}:{}",
                     ToString(type), handle, info.debugName, 
                     info.creationFile, info.creationLine);
    }

    void OpenGLDebug::UntrackResource(GLuint handle, GLResourceType type)
    {
        if (!m_debugEnabled) return;

        std::lock_guard<std::mutex> lock(m_resourceMutex);
        
        uint64 key = MakeResourceKey(handle, type);
        auto it = m_trackedResources.find(key);
        if (it != m_trackedResources.end())
        {
            RVX_RHI_DEBUG("GL Resource Destroyed: {} #{} '{}' (created at frame {})",
                         ToString(type), handle, it->second.debugName, it->second.creationFrame);
            m_trackedResources.erase(it);
        }
        else
        {
            RVX_RHI_WARN("GL Resource Destroyed but not tracked: {} #{}", ToString(type), handle);
        }
    }

    void OpenGLDebug::SetResourceSize(GLuint handle, GLResourceType type, uint64 size)
    {
        if (!m_debugEnabled) return;

        std::lock_guard<std::mutex> lock(m_resourceMutex);
        
        uint64 key = MakeResourceKey(handle, type);
        auto it = m_trackedResources.find(key);
        if (it != m_trackedResources.end())
        {
            it->second.size = size;
        }
    }

    const GLResourceInfo* OpenGLDebug::GetResourceInfo(GLuint handle, GLResourceType type) const
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        
        uint64 key = MakeResourceKey(handle, type);
        auto it = m_trackedResources.find(key);
        return it != m_trackedResources.end() ? &it->second : nullptr;
    }

    void OpenGLDebug::SetObjectLabel(GLenum type, GLuint handle, const char* name)
    {
        if (!m_hasKHRDebug || !name || !name[0]) return;
        glObjectLabel(type, handle, -1, name);
    }

    void OpenGLDebug::SetBufferLabel(GLuint buffer, const char* name)
    {
        SetObjectLabel(GL_BUFFER, buffer, name);
    }

    void OpenGLDebug::SetTextureLabel(GLuint texture, const char* name)
    {
        SetObjectLabel(GL_TEXTURE, texture, name);
    }

    void OpenGLDebug::SetSamplerLabel(GLuint sampler, const char* name)
    {
        SetObjectLabel(GL_SAMPLER, sampler, name);
    }

    void OpenGLDebug::SetShaderLabel(GLuint shader, const char* name)
    {
        SetObjectLabel(GL_SHADER, shader, name);
    }

    void OpenGLDebug::SetProgramLabel(GLuint program, const char* name)
    {
        SetObjectLabel(GL_PROGRAM, program, name);
    }

    void OpenGLDebug::SetFramebufferLabel(GLuint fbo, const char* name)
    {
        SetObjectLabel(GL_FRAMEBUFFER, fbo, name);
    }

    void OpenGLDebug::SetVAOLabel(GLuint vao, const char* name)
    {
        SetObjectLabel(GL_VERTEX_ARRAY, vao, name);
    }

    void OpenGLDebug::PushDebugGroup(const char* name)
    {
        if (!m_hasKHRDebug || !name) return;
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
    }

    void OpenGLDebug::PopDebugGroup()
    {
        if (!m_hasKHRDebug) return;
        glPopDebugGroup();
    }

    bool OpenGLDebug::ValidateBuffer(GLuint buffer, const char* operation)
    {
        if (buffer == 0)
        {
            RVX_RHI_ERROR("Invalid buffer (0) in operation: {}", operation);
            return false;
        }

        if (!glIsBuffer(buffer))
        {
            RVX_RHI_ERROR("Handle {} is not a valid buffer in operation: {}", buffer, operation);
            LogBufferInfo(buffer, operation);
            return false;
        }

        return true;
    }

    bool OpenGLDebug::ValidateTexture(GLuint texture, const char* operation)
    {
        if (texture == 0)
        {
            RVX_RHI_ERROR("Invalid texture (0) in operation: {}", operation);
            return false;
        }

        if (!glIsTexture(texture))
        {
            RVX_RHI_ERROR("Handle {} is not a valid texture in operation: {}", texture, operation);
            LogTextureInfo(texture, operation);
            return false;
        }

        return true;
    }

    bool OpenGLDebug::ValidateProgram(GLuint program, const char* operation)
    {
        if (program == 0)
        {
            RVX_RHI_ERROR("Invalid program (0) in operation: {}", operation);
            return false;
        }

        if (!glIsProgram(program))
        {
            RVX_RHI_ERROR("Handle {} is not a valid program in operation: {}", program, operation);
            return false;
        }

        return true;
    }

    bool OpenGLDebug::ValidateFBO(GLuint fbo, const char* operation)
    {
        // FBO 0 is the default framebuffer, which is valid
        if (fbo != 0 && !glIsFramebuffer(fbo))
        {
            RVX_RHI_ERROR("Handle {} is not a valid framebuffer in operation: {}", fbo, operation);
            return false;
        }

        return true;
    }

    void OpenGLDebug::CheckError(const char* operation, const std::source_location& loc)
    {
        GLenum error;
        bool hasError = false;
        while ((error = glGetError()) != GL_NO_ERROR)
        {
            RVX_RHI_ERROR("OpenGL Error {} in '{}' at {}:{}",
                         GLErrorToString(error), operation,
                         loc.file_name(), loc.line());
            hasError = true;
        }

        if (hasError)
        {
            // Provide additional context if available
            // This could dump current state, etc.
        }
    }

    void OpenGLDebug::LogFrameStats()
    {
        RVX_RHI_DEBUG("=== OpenGL Frame {} Stats ===", m_currentFrame);
        RVX_RHI_DEBUG("  Draw Calls: {}", m_stats.drawCalls.load());
        RVX_RHI_DEBUG("  Dispatch Calls: {}", m_stats.dispatchCalls.load());
        RVX_RHI_DEBUG("  State Changes: {}", m_stats.stateChanges.load());
        RVX_RHI_DEBUG("  Buffer Binds: {}", m_stats.bufferBinds.load());
        RVX_RHI_DEBUG("  Texture Binds: {}", m_stats.textureBinds.load());
        RVX_RHI_DEBUG("  Program Binds: {}", m_stats.programBinds.load());
        RVX_RHI_DEBUG("  FBO Binds: {}", m_stats.fboBinds.load());
        RVX_RHI_DEBUG("  VAO Binds: {}", m_stats.vaoBinds.load());
    }

    void OpenGLDebug::DumpTrackedResources()
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        
        for (const auto& [key, info] : m_trackedResources)
        {
            RVX_RHI_WARN("  - {} #{} '{}' (created at {}:{}, frame {}, size {} bytes)",
                        ToString(info.type), info.handle, info.debugName,
                        info.creationFile, info.creationLine,
                        info.creationFrame, info.size);
        }
    }

    // =============================================================================
    // Validation Helpers Implementation
    // =============================================================================

    void LogBufferInfo(GLuint buffer, const char* context)
    {
        if (!glIsBuffer(buffer))
        {
            RVX_RHI_ERROR("[{}] Buffer {} is not a valid GL buffer object", context, buffer);
            return;
        }

        GLint size = 0, usage = 0, access = 0, mapped = 0;
        glGetNamedBufferParameteriv(buffer, GL_BUFFER_SIZE, &size);
        glGetNamedBufferParameteriv(buffer, GL_BUFFER_USAGE, &usage);
        glGetNamedBufferParameteriv(buffer, GL_BUFFER_ACCESS, &access);
        glGetNamedBufferParameteriv(buffer, GL_BUFFER_MAPPED, &mapped);

        RVX_RHI_INFO("[{}] Buffer {} info: size={}, usage=0x{:X}, mapped={}",
                    context, buffer, size, usage, mapped != 0);

        // Get debug name if tracked
        const auto* info = OpenGLDebug::Get().GetResourceInfo(buffer, GLResourceType::Buffer);
        if (info)
        {
            RVX_RHI_INFO("[{}] Buffer {} debug name: '{}', created at {}:{}",
                        context, buffer, info->debugName, info->creationFile, info->creationLine);
        }
    }

    void LogTextureInfo(GLuint texture, const char* context)
    {
        if (!glIsTexture(texture))
        {
            RVX_RHI_ERROR("[{}] Texture {} is not a valid GL texture object", context, texture);
            return;
        }

        // Note: For detailed texture info, we'd need to bind it and query parameters
        // This is a simplified version
        const auto* info = OpenGLDebug::Get().GetResourceInfo(texture, GLResourceType::Texture);
        if (info)
        {
            RVX_RHI_INFO("[{}] Texture {} debug name: '{}', created at {}:{}, size={} bytes",
                        context, texture, info->debugName, info->creationFile, info->creationLine, info->size);
        }
        else
        {
            RVX_RHI_INFO("[{}] Texture {} (untracked)", context, texture);
        }
    }

    void LogProgramInfo(GLuint program, const char* context)
    {
        if (!glIsProgram(program))
        {
            RVX_RHI_ERROR("[{}] Program {} is not a valid GL program object", context, program);
            return;
        }

        GLint linkStatus = 0, validateStatus = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        glGetProgramiv(program, GL_VALIDATE_STATUS, &validateStatus);

        RVX_RHI_INFO("[{}] Program {} info: linked={}, validated={}",
                    context, program, linkStatus != 0, validateStatus != 0);

        // Get info log if there's an error
        if (linkStatus == 0)
        {
            GLint logLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
            if (logLength > 0)
            {
                std::string infoLog(logLength, '\0');
                glGetProgramInfoLog(program, logLength, nullptr, infoLog.data());
                RVX_RHI_ERROR("[{}] Program {} link error:\n{}", context, program, infoLog);
            }
        }

        // Get debug name if tracked
        const auto* info = OpenGLDebug::Get().GetResourceInfo(program, GLResourceType::Program);
        if (info)
        {
            RVX_RHI_INFO("[{}] Program {} debug name: '{}', created at {}:{}",
                        context, program, info->debugName, info->creationFile, info->creationLine);
        }
    }

    void LogFBOStatus(GLuint fbo, const char* context)
    {
        GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
        RVX_RHI_INFO("[{}] FBO {} status: {}", context, fbo, FBOStatusToString(status));

        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            // Get attached images info
            GLint colorAttachments[8];
            for (int i = 0; i < 8; ++i)
            {
                glGetNamedFramebufferAttachmentParameteriv(fbo, GL_COLOR_ATTACHMENT0 + i,
                    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &colorAttachments[i]);
            }

            GLint depthType = 0, stencilType = 0;
            glGetNamedFramebufferAttachmentParameteriv(fbo, GL_DEPTH_ATTACHMENT,
                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &depthType);
            glGetNamedFramebufferAttachmentParameteriv(fbo, GL_STENCIL_ATTACHMENT,
                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencilType);

            std::stringstream ss;
            ss << "Attachments: ";
            for (int i = 0; i < 8; ++i)
            {
                if (colorAttachments[i] != GL_NONE)
                    ss << "COLOR" << i << " ";
            }
            if (depthType != GL_NONE) ss << "DEPTH ";
            if (stencilType != GL_NONE) ss << "STENCIL ";

            RVX_RHI_INFO("[{}] FBO {} {}", context, fbo, ss.str());
        }
    }

    bool ValidateFBOCompleteness(GLuint fbo, const char* context)
    {
        GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            RVX_RHI_ERROR("[{}] FBO {} is incomplete: {}", context, fbo, FBOStatusToString(status));
            LogFBOStatus(fbo, context);
            return false;
        }
        return true;
    }

} // namespace RVX
