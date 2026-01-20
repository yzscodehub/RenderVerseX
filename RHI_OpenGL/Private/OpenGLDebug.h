#pragma once

#include "OpenGLCommon.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <source_location>

namespace RVX
{
    // =============================================================================
    // OpenGL Resource Types (for tracking)
    // =============================================================================
    enum class GLResourceType : uint8
    {
        Buffer,
        Texture,
        Sampler,
        Shader,
        Program,
        VAO,
        FBO,
        Sync,
        Query,
        Unknown
    };

    inline const char* ToString(GLResourceType type)
    {
        switch (type)
        {
            case GLResourceType::Buffer:  return "Buffer";
            case GLResourceType::Texture: return "Texture";
            case GLResourceType::Sampler: return "Sampler";
            case GLResourceType::Shader:  return "Shader";
            case GLResourceType::Program: return "Program";
            case GLResourceType::VAO:     return "VAO";
            case GLResourceType::FBO:     return "FBO";
            case GLResourceType::Sync:    return "Sync";
            case GLResourceType::Query:   return "Query";
            default:                      return "Unknown";
        }
    }

    // =============================================================================
    // Resource Tracking Info
    // =============================================================================
    struct GLResourceInfo
    {
        GLuint handle = 0;
        GLResourceType type = GLResourceType::Unknown;
        std::string debugName;
        std::string creationFile;
        int creationLine = 0;
        uint64 creationFrame = 0;
        uint64 size = 0;  // Memory size in bytes (if applicable)
    };

    // =============================================================================
    // Debug Statistics
    // =============================================================================
    struct GLDebugStats
    {
        // Per-frame counters (reset each frame)
        std::atomic<uint32> drawCalls{0};
        std::atomic<uint32> dispatchCalls{0};
        std::atomic<uint32> stateChanges{0};
        std::atomic<uint32> bufferBinds{0};
        std::atomic<uint32> textureBinds{0};
        std::atomic<uint32> programBinds{0};
        std::atomic<uint32> fboBinds{0};
        std::atomic<uint32> vaoBinds{0};

        // Cumulative counters
        std::atomic<uint32> buffersCreated{0};
        std::atomic<uint32> buffersDestroyed{0};
        std::atomic<uint32> texturesCreated{0};
        std::atomic<uint32> texturesDestroyed{0};
        std::atomic<uint64> totalBufferMemory{0};
        std::atomic<uint64> totalTextureMemory{0};

        void ResetFrameCounters()
        {
            drawCalls = 0;
            dispatchCalls = 0;
            stateChanges = 0;
            bufferBinds = 0;
            textureBinds = 0;
            programBinds = 0;
            fboBinds = 0;
            vaoBinds = 0;
        }
    };

    // =============================================================================
    // OpenGL Debug System
    // =============================================================================
    class OpenGLDebug
    {
    public:
        static OpenGLDebug& Get()
        {
            static OpenGLDebug instance;
            return instance;
        }

        // Initialization
        void Initialize(bool enableDebugOutput);
        void Shutdown();

        // Frame management
        void BeginFrame(uint64 frameIndex);
        void EndFrame();

        // Resource tracking
        void TrackResource(GLuint handle, GLResourceType type, const char* debugName,
                          const std::source_location& loc = std::source_location::current());
        void UntrackResource(GLuint handle, GLResourceType type);
        void SetResourceSize(GLuint handle, GLResourceType type, uint64 size);
        const GLResourceInfo* GetResourceInfo(GLuint handle, GLResourceType type) const;

        // GL Object Labels (visible in GPU debuggers like RenderDoc)
        void SetObjectLabel(GLenum type, GLuint handle, const char* name);
        void SetBufferLabel(GLuint buffer, const char* name);
        void SetTextureLabel(GLuint texture, const char* name);
        void SetSamplerLabel(GLuint sampler, const char* name);
        void SetShaderLabel(GLuint shader, const char* name);
        void SetProgramLabel(GLuint program, const char* name);
        void SetFramebufferLabel(GLuint fbo, const char* name);
        void SetVAOLabel(GLuint vao, const char* name);

        // Debug groups (visible in GPU debuggers)
        void PushDebugGroup(const char* name);
        void PopDebugGroup();

        // Validation helpers
        bool ValidateBuffer(GLuint buffer, const char* operation);
        bool ValidateTexture(GLuint texture, const char* operation);
        bool ValidateProgram(GLuint program, const char* operation);
        bool ValidateFBO(GLuint fbo, const char* operation);

        // Error checking with context
        void CheckError(const char* operation, const std::source_location& loc = std::source_location::current());

        // Statistics
        GLDebugStats& GetStats() { return m_stats; }
        const GLDebugStats& GetStats() const { return m_stats; }
        void LogFrameStats();

        // Dump all tracked resources (for leak detection)
        void DumpTrackedResources();

        // State validation
        bool IsDebugEnabled() const { return m_debugEnabled; }
        uint64 GetCurrentFrame() const { return m_currentFrame; }

    private:
        OpenGLDebug() = default;
        ~OpenGLDebug() = default;

        uint64 MakeResourceKey(GLuint handle, GLResourceType type) const
        {
            return (static_cast<uint64>(type) << 32) | handle;
        }

        bool m_debugEnabled = false;
        bool m_hasKHRDebug = false;
        uint64 m_currentFrame = 0;

        mutable std::mutex m_resourceMutex;
        std::unordered_map<uint64, GLResourceInfo> m_trackedResources;

        GLDebugStats m_stats;
    };

    // =============================================================================
    // Debug Macros
    // =============================================================================

#ifdef RVX_GL_DEBUG
    // Check GL error after operation
    #define GL_DEBUG_CHECK(operation) \
        OpenGLDebug::Get().CheckError(operation)

    // Track resource creation
    #define GL_DEBUG_TRACK(handle, type, name) \
        OpenGLDebug::Get().TrackResource(handle, type, name)

    // Untrack resource deletion
    #define GL_DEBUG_UNTRACK(handle, type) \
        OpenGLDebug::Get().UntrackResource(handle, type)

    // Validate resource before use
    #define GL_DEBUG_VALIDATE_BUFFER(buffer, op) \
        if (!OpenGLDebug::Get().ValidateBuffer(buffer, op)) return

    #define GL_DEBUG_VALIDATE_TEXTURE(texture, op) \
        if (!OpenGLDebug::Get().ValidateTexture(texture, op)) return

    #define GL_DEBUG_VALIDATE_PROGRAM(program, op) \
        if (!OpenGLDebug::Get().ValidateProgram(program, op)) return

    // Statistics tracking
    #define GL_DEBUG_STAT_INC(stat) \
        ++OpenGLDebug::Get().GetStats().stat

    // Debug groups
    #define GL_DEBUG_GROUP_BEGIN(name) \
        OpenGLDebug::Get().PushDebugGroup(name)

    #define GL_DEBUG_GROUP_END() \
        OpenGLDebug::Get().PopDebugGroup()

    // Scoped debug group (RAII)
    class GLDebugScope
    {
    public:
        explicit GLDebugScope(const char* name) { OpenGLDebug::Get().PushDebugGroup(name); }
        ~GLDebugScope() { OpenGLDebug::Get().PopDebugGroup(); }
        GLDebugScope(const GLDebugScope&) = delete;
        GLDebugScope& operator=(const GLDebugScope&) = delete;
    };
    #define GL_DEBUG_SCOPE(name) \
        GLDebugScope _glDebugScope##__LINE__(name)

#else
    // Release mode: no-ops
    #define GL_DEBUG_CHECK(operation) ((void)0)
    #define GL_DEBUG_TRACK(handle, type, name) ((void)0)
    #define GL_DEBUG_UNTRACK(handle, type) ((void)0)
    #define GL_DEBUG_VALIDATE_BUFFER(buffer, op) ((void)0)
    #define GL_DEBUG_VALIDATE_TEXTURE(texture, op) ((void)0)
    #define GL_DEBUG_VALIDATE_PROGRAM(program, op) ((void)0)
    #define GL_DEBUG_STAT_INC(stat) ((void)0)
    #define GL_DEBUG_GROUP_BEGIN(name) ((void)0)
    #define GL_DEBUG_GROUP_END() ((void)0)
    #define GL_DEBUG_SCOPE(name) ((void)0)
#endif

    // =============================================================================
    // Validation Helpers (always compiled, used for error reporting)
    // =============================================================================
    
    // Log detailed buffer info
    void LogBufferInfo(GLuint buffer, const char* context);
    
    // Log detailed texture info
    void LogTextureInfo(GLuint texture, const char* context);
    
    // Log detailed program info (including shader compilation errors)
    void LogProgramInfo(GLuint program, const char* context);
    
    // Log FBO completeness status
    void LogFBOStatus(GLuint fbo, const char* context);
    
    // Validate FBO completeness and log detailed error if incomplete
    bool ValidateFBOCompleteness(GLuint fbo, const char* context);

    // =============================================================================
    // Error Code to String
    // =============================================================================
    inline const char* GLErrorToString(GLenum error)
    {
        switch (error)
        {
            case GL_NO_ERROR:                      return "GL_NO_ERROR";
            case GL_INVALID_ENUM:                  return "GL_INVALID_ENUM";
            case GL_INVALID_VALUE:                 return "GL_INVALID_VALUE";
            case GL_INVALID_OPERATION:             return "GL_INVALID_OPERATION";
            case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
            case GL_OUT_OF_MEMORY:                 return "GL_OUT_OF_MEMORY";
            case GL_STACK_UNDERFLOW:               return "GL_STACK_UNDERFLOW";
            case GL_STACK_OVERFLOW:                return "GL_STACK_OVERFLOW";
            default:                               return "Unknown GL Error";
        }
    }

    inline const char* FBOStatusToString(GLenum status)
    {
        switch (status)
        {
            case GL_FRAMEBUFFER_COMPLETE:                      return "COMPLETE";
            case GL_FRAMEBUFFER_UNDEFINED:                     return "UNDEFINED";
            case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:         return "INCOMPLETE_ATTACHMENT";
            case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: return "INCOMPLETE_MISSING_ATTACHMENT";
            case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:        return "INCOMPLETE_DRAW_BUFFER";
            case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:        return "INCOMPLETE_READ_BUFFER";
            case GL_FRAMEBUFFER_UNSUPPORTED:                   return "UNSUPPORTED";
            case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:        return "INCOMPLETE_MULTISAMPLE";
            case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:      return "INCOMPLETE_LAYER_TARGETS";
            default:                                           return "Unknown FBO Status";
        }
    }

} // namespace RVX
