#include "OpenGLDevice.h"
#include "OpenGLResources.h"
#include "OpenGLShader.h"
#include "OpenGLPipeline.h"
#include "OpenGLDescriptor.h"
#include "OpenGLCommandContext.h"
#include "OpenGLSwapChain.h"
#include "OpenGLSync.h"
#include "OpenGL/OpenGLDevice.h"
#include "Core/Log.h"

namespace RVX
{
    // =============================================================================
    // Debug Callback
    // =============================================================================
    static void GLAPIENTRY GLDebugCallback(
        GLenum source,
        GLenum type,
        GLuint id,
        GLenum severity,
        GLsizei /*length*/,
        const GLchar* message,
        const void* /*userParam*/)
    {
        // Ignore non-significant error codes
        if (id == 131169 || id == 131185 || id == 131218 || id == 131204)
            return;

        // Ignore debug group push/pop messages (these are for GPU profilers like RenderDoc)
        if (type == GL_DEBUG_TYPE_PUSH_GROUP || type == GL_DEBUG_TYPE_POP_GROUP)
            return;

        // Ignore notification-level messages from application (our own debug scopes)
        if (source == GL_DEBUG_SOURCE_APPLICATION && severity == GL_DEBUG_SEVERITY_NOTIFICATION)
            return;

        const char* sourceStr = "Unknown";
        switch (source)
        {
            case GL_DEBUG_SOURCE_API:             sourceStr = "API"; break;
            case GL_DEBUG_SOURCE_WINDOW_SYSTEM:   sourceStr = "Window System"; break;
            case GL_DEBUG_SOURCE_SHADER_COMPILER: sourceStr = "Shader Compiler"; break;
            case GL_DEBUG_SOURCE_THIRD_PARTY:     sourceStr = "Third Party"; break;
            case GL_DEBUG_SOURCE_APPLICATION:     sourceStr = "Application"; break;
            case GL_DEBUG_SOURCE_OTHER:           sourceStr = "Other"; break;
        }

        const char* typeStr = "Unknown";
        switch (type)
        {
            case GL_DEBUG_TYPE_ERROR:               typeStr = "Error"; break;
            case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: typeStr = "Deprecated"; break;
            case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  typeStr = "Undefined Behavior"; break;
            case GL_DEBUG_TYPE_PORTABILITY:         typeStr = "Portability"; break;
            case GL_DEBUG_TYPE_PERFORMANCE:         typeStr = "Performance"; break;
            case GL_DEBUG_TYPE_MARKER:              typeStr = "Marker"; break;
            case GL_DEBUG_TYPE_OTHER:               typeStr = "Other"; break;
        }

        switch (severity)
        {
            case GL_DEBUG_SEVERITY_HIGH:
                RVX_RHI_ERROR("OpenGL [{}][{}] {}: {}", sourceStr, typeStr, id, message);
                break;
            case GL_DEBUG_SEVERITY_MEDIUM:
                RVX_RHI_WARN("OpenGL [{}][{}] {}: {}", sourceStr, typeStr, id, message);
                break;
            case GL_DEBUG_SEVERITY_LOW:
                RVX_RHI_INFO("OpenGL [{}][{}] {}: {}", sourceStr, typeStr, id, message);
                break;
            case GL_DEBUG_SEVERITY_NOTIFICATION:
                RVX_RHI_DEBUG("OpenGL [{}][{}] {}: {}", sourceStr, typeStr, id, message);
                break;
        }
    }

    // =============================================================================
    // OpenGLDevice Implementation
    // =============================================================================
    OpenGLDevice::OpenGLDevice(const RHIDeviceDesc& desc)
    {
        RVX_RHI_INFO("Creating OpenGL Device...");
        
        // Store the GL thread ID
        m_glThreadId = std::this_thread::get_id();
        
        // Initialize OpenGL context (assumes GLFW window already exists with context)
        if (!InitializeContext())
        {
            RVX_RHI_ERROR("Failed to initialize OpenGL context");
            return;
        }
        
        // Query capabilities
        QueryCapabilities();
        
        // Load extensions
        LoadExtensions();
        
        // Initialize debug system
        OpenGLDebug::Get().Initialize(desc.enableDebugLayer);

        // Enable debug output if requested
        if (desc.enableDebugLayer && m_extensions.GL_KHR_debug)
        {
            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(GLDebugCallback, nullptr);
            glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
            RVX_RHI_INFO("OpenGL debug output enabled");
        }
        
        // Clear any leftover GL errors from driver/GLFW initialization
        while (glGetError() != GL_NO_ERROR) {}
        
        m_initialized = true;
        m_frameIndex = 0;
        RVX_RHI_INFO("OpenGL Device created successfully");
    }

    OpenGLDevice::~OpenGLDevice()
    {
        if (m_initialized)
        {
            WaitIdle();
            
            // Flush deletion queue
            m_deletionQueue.FlushAll();
            
            // Clear caches before debug shutdown (they are member variables destroyed after destructor body)
            m_fboCache.Clear();
            m_vaoCache.Clear();
            
            // Shutdown debug system (must be after all resources are destroyed)
            OpenGLDebug::Get().Shutdown();
            
            RVX_RHI_INFO("OpenGL Device destroyed");
        }
    }

    bool OpenGLDevice::InitializeContext()
    {
        // Load OpenGL functions using glad
        // Note: assumes glfwMakeContextCurrent has already been called
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
        {
            RVX_RHI_ERROR("Failed to load OpenGL functions via glad");
            return false;
        }
        
        // Check OpenGL version
        GLint major, minor;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        
        RVX_RHI_INFO("OpenGL Version: {}.{}", major, minor);
        RVX_RHI_INFO("GLSL Version: {}", (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
        RVX_RHI_INFO("Renderer: {}", (const char*)glGetString(GL_RENDERER));
        RVX_RHI_INFO("Vendor: {}", (const char*)glGetString(GL_VENDOR));
        
        // Require OpenGL 4.5
        if (major < 4 || (major == 4 && minor < 5))
        {
            RVX_RHI_ERROR("OpenGL 4.5 or higher required, got {}.{}", major, minor);
            return false;
        }
        
        return true;
    }

    void OpenGLDevice::QueryCapabilities()
    {
        m_capabilities.backendType = RHIBackendType::OpenGL;
        
        // Version info
        GLint major, minor;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        m_capabilities.opengl.majorVersion = static_cast<uint32>(major);
        m_capabilities.opengl.minorVersion = static_cast<uint32>(minor);
        m_capabilities.opengl.coreProfile = true;
        
        // Strings
        m_capabilities.adapterName = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        m_capabilities.opengl.renderer = m_capabilities.adapterName;
        m_capabilities.opengl.vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        m_capabilities.opengl.glslVersion = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        
        // Texture limits
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, reinterpret_cast<GLint*>(&m_capabilities.maxTextureSize2D));
        m_capabilities.maxTextureSize = m_capabilities.maxTextureSize2D;
        glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, reinterpret_cast<GLint*>(&m_capabilities.maxTextureSize3D));
        glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, reinterpret_cast<GLint*>(&m_capabilities.maxTextureSizeCube));
        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, reinterpret_cast<GLint*>(&m_capabilities.maxTextureArrayLayers));
        m_capabilities.maxTextureLayers = m_capabilities.maxTextureArrayLayers;
        glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, reinterpret_cast<GLint*>(&m_capabilities.maxColorAttachments));
        
        // Binding limits
        glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, reinterpret_cast<GLint*>(&m_capabilities.opengl.maxUniformBufferBindings));
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, reinterpret_cast<GLint*>(&m_capabilities.opengl.maxTextureUnits));
        glGetIntegerv(GL_MAX_IMAGE_UNITS, reinterpret_cast<GLint*>(&m_capabilities.opengl.maxImageUnits));
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, reinterpret_cast<GLint*>(&m_capabilities.opengl.maxSSBOBindings));
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, reinterpret_cast<GLint*>(&m_capabilities.opengl.maxVertexAttribs));
        glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, reinterpret_cast<GLint*>(&m_capabilities.opengl.maxUniformBlockSize));
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, reinterpret_cast<GLint*>(&m_capabilities.opengl.maxSSBOSize));
        
        // Compute shader limits
        glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_SIZE, reinterpret_cast<GLint*>(&m_capabilities.maxComputeWorkGroupSizeX));
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, reinterpret_cast<GLint*>(&m_capabilities.maxComputeWorkGroupSizeX));
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1, reinterpret_cast<GLint*>(&m_capabilities.maxComputeWorkGroupSizeY));
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2, reinterpret_cast<GLint*>(&m_capabilities.maxComputeWorkGroupSizeZ));
        glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, reinterpret_cast<GLint*>(&m_capabilities.opengl.maxComputeSharedMemorySize));
        
        // Feature detection
        m_capabilities.opengl.hasDSA = (major > 4) || (major == 4 && minor >= 5);
        m_capabilities.opengl.hasComputeShader = (major > 4) || (major == 4 && minor >= 3);
        m_capabilities.opengl.hasSSBO = m_capabilities.opengl.hasComputeShader;
        m_capabilities.opengl.hasMultiBind = (major > 4) || (major == 4 && minor >= 4);
        m_capabilities.opengl.hasBufferStorage = m_capabilities.opengl.hasMultiBind;
        m_capabilities.opengl.hasPersistentMapping = m_capabilities.opengl.hasBufferStorage;
        m_capabilities.opengl.hasTextureView = m_capabilities.opengl.hasComputeShader;
        
        // Push constant size (simulated via UBO)
        m_capabilities.maxPushConstantSize = 256;
    }

    void OpenGLDevice::LoadExtensions()
    {
        GLint numExtensions;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
        
        for (GLint i = 0; i < numExtensions; ++i)
        {
            const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
            if (!ext) continue;
            
            if (strcmp(ext, "GL_ARB_gl_spirv") == 0)
                m_extensions.GL_ARB_gl_spirv = true;
            else if (strcmp(ext, "GL_ARB_bindless_texture") == 0)
                m_extensions.GL_ARB_bindless_texture = true;
            else if (strcmp(ext, "GL_ARB_shader_draw_parameters") == 0)
                m_extensions.GL_ARB_shader_draw_parameters = true;
            else if (strcmp(ext, "GL_ARB_indirect_parameters") == 0)
                m_extensions.GL_ARB_indirect_parameters = true;
            else if (strcmp(ext, "GL_ARB_buffer_storage") == 0)
                m_extensions.GL_ARB_buffer_storage = true;
            else if (strcmp(ext, "GL_ARB_direct_state_access") == 0)
                m_extensions.GL_ARB_direct_state_access = true;
            else if (strcmp(ext, "GL_ARB_texture_view") == 0)
                m_extensions.GL_ARB_texture_view = true;
            else if (strcmp(ext, "GL_ARB_multi_bind") == 0)
                m_extensions.GL_ARB_multi_bind = true;
            else if (strcmp(ext, "GL_ARB_separate_shader_objects") == 0)
                m_extensions.GL_ARB_separate_shader_objects = true;
            else if (strcmp(ext, "GL_KHR_debug") == 0)
                m_extensions.GL_KHR_debug = true;
            else if (strcmp(ext, "GL_NV_mesh_shader") == 0)
                m_extensions.GL_NV_mesh_shader = true;
        }
        
        // Update capabilities based on extensions
        m_capabilities.opengl.hasARBSpirv = m_extensions.GL_ARB_gl_spirv;
        m_capabilities.opengl.hasBindlessTexture = m_extensions.GL_ARB_bindless_texture;
        m_capabilities.opengl.hasDebugOutput = m_extensions.GL_KHR_debug;
        m_capabilities.opengl.hasSeparateShaderObjects = m_extensions.GL_ARB_separate_shader_objects;
        
        m_capabilities.supportsBindless = m_extensions.GL_ARB_bindless_texture;
        m_capabilities.supportsMeshShaders = m_extensions.GL_NV_mesh_shader;
    }

    // =============================================================================
    // Resource Creation
    // =============================================================================
    RHIBufferRef OpenGLDevice::CreateBuffer(const RHIBufferDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateBuffer");
        
        auto buffer = MakeRef<OpenGLBuffer>(this, desc);
        if (buffer->GetHandle() == 0)
        {
            RVX_RHI_ERROR("Failed to create buffer '{}'", desc.debugName ? desc.debugName : "");
            return nullptr;
        }
        return buffer;
    }

    RHITextureRef OpenGLDevice::CreateTexture(const RHITextureDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateTexture");
        
        auto texture = MakeRef<OpenGLTexture>(this, desc);
        if (texture->GetHandle() == 0)
        {
            RVX_RHI_ERROR("Failed to create texture '{}'", desc.debugName ? desc.debugName : "");
            return nullptr;
        }
        return texture;
    }

    RHITextureViewRef OpenGLDevice::CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateTextureView");
        
        if (!texture)
        {
            RVX_RHI_ERROR("CreateTextureView: texture is null");
            return nullptr;
        }
        
        auto* glTexture = static_cast<OpenGLTexture*>(texture);
        return MakeRef<OpenGLTextureView>(this, glTexture, desc);
    }

    RHISamplerRef OpenGLDevice::CreateSampler(const RHISamplerDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateSampler");
        
        auto sampler = MakeRef<OpenGLSampler>(this, desc);
        if (sampler->GetHandle() == 0)
        {
            RVX_RHI_ERROR("Failed to create sampler '{}'", desc.debugName ? desc.debugName : "");
            return nullptr;
        }
        return sampler;
    }

    RHIShaderRef OpenGLDevice::CreateShader(const RHIShaderDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateShader");
        
        // OpenGL shaders need GLSL source, which should be compiled separately
        // and stored in the shader bytecode or provided through a custom path.
        // For now, we expect the bytecode to actually be GLSL source text.
        
        if (!desc.bytecode || desc.bytecodeSize == 0)
        {
            RVX_RHI_ERROR("CreateShader: No bytecode/source provided for '{}'", 
                         desc.debugName ? desc.debugName : "");
            return nullptr;
        }
        
        // Treat bytecode as GLSL source
        std::string glslSource(reinterpret_cast<const char*>(desc.bytecode), desc.bytecodeSize);
        
        auto shader = MakeRef<OpenGLShader>(this, desc, glslSource);
        if (!shader->IsValid())
        {
            RVX_RHI_ERROR("Failed to create shader '{}'", desc.debugName ? desc.debugName : "");
            return nullptr;
        }
        
        return shader;
    }

    // =============================================================================
    // Heap Management (Not supported in OpenGL)
    // =============================================================================
    RHIHeapRef OpenGLDevice::CreateHeap(const RHIHeapDesc& /*desc*/)
    {
        RVX_RHI_WARN("OpenGL does not support explicit heap management");
        return nullptr;
    }

    RHITextureRef OpenGLDevice::CreatePlacedTexture(RHIHeap* /*heap*/, uint64 /*offset*/, const RHITextureDesc& /*desc*/)
    {
        RVX_RHI_WARN("OpenGL does not support placed textures");
        return nullptr;
    }

    RHIBufferRef OpenGLDevice::CreatePlacedBuffer(RHIHeap* /*heap*/, uint64 /*offset*/, const RHIBufferDesc& /*desc*/)
    {
        RVX_RHI_WARN("OpenGL does not support placed buffers");
        return nullptr;
    }

    IRHIDevice::MemoryRequirements OpenGLDevice::GetTextureMemoryRequirements(const RHITextureDesc& /*desc*/)
    {
        return {0, 0};
    }

    IRHIDevice::MemoryRequirements OpenGLDevice::GetBufferMemoryRequirements(const RHIBufferDesc& /*desc*/)
    {
        return {0, 0};
    }

    // =============================================================================
    // Pipeline Creation
    // =============================================================================
    RHIDescriptorSetLayoutRef OpenGLDevice::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateDescriptorSetLayout");
        return MakeRef<OpenGLDescriptorSetLayout>(this, desc);
    }

    RHIPipelineLayoutRef OpenGLDevice::CreatePipelineLayout(const RHIPipelineLayoutDesc& desc)
    {
        GL_DEBUG_SCOPE("CreatePipelineLayout");
        return MakeRef<OpenGLPipelineLayout>(this, desc);
    }

    RHIPipelineRef OpenGLDevice::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateGraphicsPipeline");
        
        auto pipeline = MakeRef<OpenGLGraphicsPipeline>(this, desc);
        if (!pipeline->IsValid())
        {
            RVX_RHI_ERROR("Failed to create graphics pipeline '{}'", 
                         desc.debugName ? desc.debugName : "");
            return nullptr;
        }
        return pipeline;
    }

    RHIPipelineRef OpenGLDevice::CreateComputePipeline(const RHIComputePipelineDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateComputePipeline");
        
        auto pipeline = MakeRef<OpenGLComputePipeline>(this, desc);
        if (!pipeline->IsValid())
        {
            RVX_RHI_ERROR("Failed to create compute pipeline '{}'", 
                         desc.debugName ? desc.debugName : "");
            return nullptr;
        }
        return pipeline;
    }

    RHIDescriptorSetRef OpenGLDevice::CreateDescriptorSet(const RHIDescriptorSetDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateDescriptorSet");
        
        if (!desc.layout)
        {
            RVX_RHI_ERROR("CreateDescriptorSet: layout is null");
            return nullptr;
        }
        
        return MakeRef<OpenGLDescriptorSet>(this, desc);
    }

    RHIQueryPoolRef OpenGLDevice::CreateQueryPool(const RHIQueryPoolDesc& /*desc*/)
    {
        // TODO: Implement OpenGL query pool support
        RVX_RHI_WARN("OpenGL query pools not yet implemented");
        return nullptr;
    }

    // =============================================================================
    // Command Context
    // =============================================================================
    RHICommandContextRef OpenGLDevice::CreateCommandContext(RHICommandQueueType type)
    {
        GL_DEBUG_SCOPE("CreateCommandContext");
        return MakeRef<OpenGLCommandContext>(this, type);
    }

    void OpenGLDevice::SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence)
    {
        // OpenGL executes commands immediately, so nothing to do here
        // The context has already executed all commands
        if (context)
        {
            context->End();
        }
        
        // Signal fence
        if (signalFence)
        {
            auto* glFence = static_cast<OpenGLFence*>(signalFence);
            glFence->Signal(glFence->GetCompletedValue() + 1);
        }
    }

    void OpenGLDevice::SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence)
    {
        for (auto* ctx : contexts)
        {
            if (ctx)
            {
                ctx->End();
            }
        }
        
        // Signal fence after all contexts
        if (signalFence)
        {
            auto* glFence = static_cast<OpenGLFence*>(signalFence);
            glFence->Signal(glFence->GetCompletedValue() + 1);
        }
    }

    // =============================================================================
    // SwapChain
    // =============================================================================
    RHISwapChainRef OpenGLDevice::CreateSwapChain(const RHISwapChainDesc& desc)
    {
        GL_DEBUG_SCOPE("CreateSwapChain");
        return MakeRef<OpenGLSwapChain>(this, desc);
    }

    // =============================================================================
    // Synchronization
    // =============================================================================
    RHIFenceRef OpenGLDevice::CreateFence(uint64 initialValue)
    {
        GL_DEBUG_SCOPE("CreateFence");
        return MakeRef<OpenGLFence>(this, initialValue);
    }

    void OpenGLDevice::WaitForFence(RHIFence* fence, uint64 value)
    {
        if (fence)
        {
            fence->Wait(value);
        }
    }

    void OpenGLDevice::WaitIdle()
    {
        if (m_initialized)
        {
            glFinish();
        }
    }

    // =============================================================================
    // Frame Management
    // =============================================================================
    void OpenGLDevice::BeginFrame()
    {
        OpenGLDebug::Get().BeginFrame(m_frameIndex);
        
        // Process deletion queue - delete resources that are safe to delete
        m_deletionQueue.ProcessDeletions(m_frameIndex);
        
        // Reset state cache if needed (usually not necessary unless context was lost)
    }

    void OpenGLDevice::EndFrame()
    {
        OpenGLDebug::Get().EndFrame();
        
        // Clean up unused cached resources
        m_fboCache.Cleanup(m_frameIndex);
        m_vaoCache.Cleanup(m_frameIndex);
        
        m_currentFrameIndex = (m_currentFrameIndex + 1) % RVX_GL_MAX_FRAME_COUNT;
        m_frameIndex++;
        
        glFlush();
    }

    // =============================================================================
    // Factory Function
    // =============================================================================
    std::unique_ptr<IRHIDevice> CreateOpenGLDevice(const RHIDeviceDesc& desc)
    {
        auto device = std::make_unique<OpenGLDevice>(desc);
        if (device->GetCapabilities().opengl.majorVersion == 0)
        {
            RVX_RHI_ERROR("Failed to create OpenGL device");
            return nullptr;
        }
        return device;
    }

} // namespace RVX
