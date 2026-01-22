#pragma once

#include "RHI/RHIDefinitions.h"
#include <string>

namespace RVX
{
    // =============================================================================
    // DX11 Threading Mode
    // =============================================================================
    enum class DX11ThreadingMode : uint8
    {
        SingleThreaded,   // Always single-threaded (safest)
        DeferredContext,  // Use Deferred Context for multi-threading
        Adaptive,         // Auto-select based on DrawCall count
    };

    // =============================================================================
    // Device Capabilities
    // =============================================================================
    struct RHICapabilities
    {
        RHIBackendType backendType = RHIBackendType::None;
        std::string adapterName;
        std::string driverVersion;

        // Memory info
        uint64 dedicatedVideoMemory = 0;
        uint64 sharedSystemMemory = 0;

        // Basic limits
        uint32 maxTextureSize = 16384;
        uint32 maxTextureSize2D = 16384;
        uint32 maxTextureSize3D = 2048;
        uint32 maxTextureSizeCube = 16384;
        uint32 maxTextureArrayLayers = 2048;
        uint32 maxTextureLayers = 2048;
        uint32 maxColorAttachments = 8;
        uint32 maxComputeWorkGroupSize[3] = {1024, 1024, 64};
        uint32 maxComputeWorkGroupCount = 65535;
        uint32 maxPushConstantSize = 128;

        // Bindless support
        bool supportsBindless = false;
        uint32 maxBindlessTextures = 0;
        uint32 maxBindlessBuffers = 0;
        uint32 maxBindlessSamplers = 0;

        // Advanced features
        bool supportsRaytracing = false;
        bool supportsMeshShaders = false;
        bool supportsVariableRateShading = false;
        bool supportsAsyncCompute = false;
        bool supportsConservativeRasterization = false;

        // Dynamic state support
        bool supportsDepthBounds = false;           // DX12/Vulkan only
        bool supportsDynamicLineWidth = false;      // Vulkan/OpenGL only
        bool supportsSeparateStencilRef = false;    // All modern APIs

        // Advanced rendering features
        bool supportsSplitBarrier = false;          // DX12/Vulkan only
        bool supportsSecondaryCommandBuffer = false;// DX12/Vulkan/Metal

        // Memory features
        bool supportsMemoryBudgetQuery = false;     // DX12(DXGI)/Vulkan(VK_EXT_memory_budget)
        bool supportsPersistentMapping = false;     // Vulkan/DX12/OpenGL4.4+

        // DX11-specific
        struct DX11Specific
        {
            DX11ThreadingMode threadingMode = DX11ThreadingMode::Adaptive;
            uint32 minDrawCallsForMultithread = 500;
            bool supportsDeferredContext = false;
            uint32 featureLevel = 0;  // e.g., 0xB000 for 11.0
        } dx11;

        // DX12-specific
        struct DX12Specific
        {
            uint32 resourceBindingTier = 0;  // 1, 2, or 3
            bool supportsRootSignature1_1 = false;
            bool supportsSM6_0 = false;
            bool supportsSM6_6 = false;
        } dx12;

        // Vulkan-specific
        struct VulkanSpecific
        {
            bool supportsDescriptorIndexing = false;
            bool supportsBufferDeviceAddress = false;
            uint32 maxPushConstantSize = 128;
            uint32 apiVersion = 0;  // VK_MAKE_VERSION
        } vulkan;

        // OpenGL-specific
        struct OpenGLSpecific
        {
            // Version info
            uint32 majorVersion = 0;       // e.g., 4
            uint32 minorVersion = 0;       // e.g., 5
            bool coreProfile = true;
            std::string renderer;          // GPU name
            std::string vendor;            // Vendor name
            std::string glslVersion;       // GLSL version string

            // Core feature detection
            bool hasDSA = false;                    // Direct State Access (4.5+)
            bool hasARBSpirv = false;               // GL_ARB_gl_spirv (4.6+)
            bool hasBindlessTexture = false;        // GL_ARB_bindless_texture
            bool hasComputeShader = false;          // 4.3+
            bool hasSSBO = false;                   // 4.3+
            bool hasMultiBind = false;              // 4.4+
            bool hasTextureView = false;            // GL_ARB_texture_view (4.3+)
            bool hasBufferStorage = false;          // GL_ARB_buffer_storage (4.4+)
            bool hasSeparateShaderObjects = false;  // Separate shader objects
            bool hasDebugOutput = false;            // GL_KHR_debug
            bool hasPersistentMapping = false;      // Persistent mapping (4.4+)

            // Binding point limits (runtime queried)
            uint32 maxUniformBufferBindings = 14;   // GL_MAX_UNIFORM_BUFFER_BINDINGS
            uint32 maxTextureUnits = 16;            // GL_MAX_TEXTURE_IMAGE_UNITS
            uint32 maxImageUnits = 8;               // GL_MAX_IMAGE_UNITS
            uint32 maxSSBOBindings = 8;             // GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
            uint32 maxVertexAttribs = 16;           // GL_MAX_VERTEX_ATTRIBS
            uint32 maxUniformBlockSize = 65536;     // GL_MAX_UNIFORM_BLOCK_SIZE
            uint32 maxSSBOSize = 0;                 // GL_MAX_SHADER_STORAGE_BLOCK_SIZE

            // Compute shader limits
            uint32 maxComputeSharedMemorySize = 32768;
        } opengl;
    };

} // namespace RVX
