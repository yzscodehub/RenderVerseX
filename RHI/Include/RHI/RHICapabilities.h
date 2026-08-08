#pragma once

#include "RHI/RHIDefinitions.h"
#include "RHI/RHIIndirectExecution.h"
#include "RHI/RHIQueueTopology.h"
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr const char* RVX_RHI_CAPABILITY_REPORT_SCHEMA_ID = "RVX.RHI.CapabilityReport";
    inline constexpr uint32 RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION = 5;

    // =============================================================================
    // DX11 Threading Mode
    // =============================================================================
    enum class DX11ThreadingMode : uint8
    {
        SingleThreaded,   // Always single-threaded (safest)
        DeferredContext,  // Use Deferred Context for multi-threading
        Adaptive,         // Auto-select based on DrawCall count
    };

    /** @brief Native barrier API selected by the DX12 backend. */
    enum class DX12BarrierDialect : uint8
    {
        Legacy = 0,
        Enhanced,
    };

    const char* GetDX12BarrierDialectName(DX12BarrierDialect dialect);

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
        bool supportsRaytracingPipeline = false;
        bool supportsRayQuery = false;
        bool supportsAccelerationStructureUpdate = false;
        bool supportsAccelerationStructureCompaction = false;
        uint32 maxRayRecursionDepth = 0;
        uint32 shaderGroupHandleSize = 0;
        uint32 shaderGroupHandleAlignment = 0;
        uint32 shaderTableBaseAlignment = 0;
        bool supportsMeshShaders = false;
        bool supportsVariableRateShading = false;
        bool supportsComputePipeline = false;
        bool supportsAsyncCompute = false;
        /**
         * @brief Temporary compatibility projection of indexedIndirectExecution.supportsCountBuffer.
         * New code must consume indexedIndirectExecution directly.
         * TODO(Task 16B): remove this projection after diagnostics and tools migrate.
         */
        bool supportsIndirectDrawCount = false;
        RHIIndexedIndirectExecutionCapabilities indexedIndirectExecution;
        bool supportsConservativeRasterization = false;

        // Query support
        bool supportsTimestampQueries = false;
        bool supportsOcclusionQueries = false;
        bool supportsPipelineStatisticsQueries = false;
        uint64 timestampFrequency = 0;

        // Synchronization support
        bool supportsHostFenceSignal = false;          // Fence value can be set directly by the host/CPU.
        bool supportsDefaultQueueFenceSignal = false;  // Fence signal through the backend's default submit path.
        bool supportsExplicitQueueFenceSignal = false; // Fence signal on an explicitly selected GPU queue.
        bool supportsQueueFenceWait = false;           // GPU queue can wait on a fence value without CPU blocking.
        bool supportsMultiQueueBatchSubmit = false;    // SubmitCommandContexts can submit mixed queue types in one batch.
        bool emulatesQueueFences = false;              // Queue fence behavior is emulated rather than native GPU sync.
        RHIQueueTopology queueTopology;                // Logical queues mapped to stable physical completion domains.

        // Dynamic state support
        bool supportsDepthBounds = false;           // DX12/Vulkan only
        bool supportsDynamicLineWidth = false;      // Vulkan/OpenGL only
        bool supportsSeparateStencilRef = false;    // All modern APIs

        // Advanced rendering features
        bool supportsSplitBarrier = false;          // DX12/Vulkan only
        bool supportsSecondaryCommandBuffer = false;// DX12/Vulkan/Metal

        // Descriptor and barrier contract support
        bool supportsDescriptorSets = false;        // Descriptor-set style binding is implemented directly or through backend emulation.
        bool supportsDynamicDescriptorOffsets = false; // Dynamic buffer offsets are supported by descriptor-set binding.
        uint32 maxDescriptorSets = 0;               // Maximum descriptor set slots supported by the base contract.
        bool supportsExplicitResourceBarriers = false; // Backend requires/supports explicit resource barrier commands.
        bool supportsExplicitAliasingBarriers = false; // Backend can order placed resources that reuse the same memory.
        bool emulatesResourceBarriers = false;      // Barrier API is emulated/no-op because backend tracks transitions implicitly.

        // Memory features
        bool supportsMemoryBudgetQuery = false;     // DX12(DXGI)/Vulkan(VK_EXT_memory_budget)
        bool supportsPersistentMapping = false;     // Vulkan/DX12/OpenGL4.4+
        bool supportsExplicitHeapManagement = false;// Explicit heap/placed resource APIs are implemented.

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
            bool supportsEnhancedBarriers = false;
            DX12BarrierDialect barrierDialect = DX12BarrierDialect::Legacy;
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

    enum class RHICapabilityFeature : uint8
    {
        ComputePipeline = 0,
        DescriptorSets = 1,
        ExplicitResourceBarriers = 2,
        QueueSynchronization = 3,
        AsyncCompute = 4,
        IndirectDrawCount = 5,
        RayTracing = 6,
        BindlessResources = 7,
        QuerySupport = 8,
        MemoryBudget = 9,
        ExplicitHeapManagement = 10,
        IndexedIndirectExecution = 11,
    };

    enum class RHICapabilityStatus : uint8
    {
        Unsupported = 0,
        Supported,
        Emulated,
    };

    const char* GetRHICapabilityFeatureName(RHICapabilityFeature feature);
    const char* GetRHICapabilityStatusName(RHICapabilityStatus status);

    struct RHICapabilityReportEntry
    {
        RHICapabilityFeature feature = RHICapabilityFeature::ComputePipeline;
        RHICapabilityStatus status = RHICapabilityStatus::Unsupported;
        bool supported = false;
        bool emulated = false;
        std::string requiredCapability;
        std::string diagnosticMessage;
    };

    struct RHICapabilityReport
    {
        uint32 schemaVersion = RVX_RHI_CAPABILITY_REPORT_SCHEMA_VERSION;
        RHIBackendType backendType = RHIBackendType::None;
        std::string adapterName;
        std::string driverVersion;
        bool validationPassed = false;
        std::string validationMessage;
        RHIQueueTopology queueTopology;
        RHIIndexedIndirectExecutionCapabilities indexedIndirectExecution;
        std::vector<RHICapabilityReportEntry> entries;
        uint32 supportedCount = 0;
        uint32 emulatedCount = 0;
        uint32 unsupportedCount = 0;
        bool renderGraphBaselineSupported = false;
        std::vector<std::string> renderGraphBaselineMissingRequirements;
    };

    /**
     * @brief Result of validating the public RHI capability contract.
     */
    struct RHICapabilityValidationResult
    {
        bool valid = true;
        std::string message;

        explicit operator bool() const { return valid; }
    };

    /**
     * @brief Validate that a backend capability report is internally consistent.
     *
     * This checks the public contract only: feature flags must agree with their
     * limits and fallback flags, without assuming a specific GPU model.
     */
    RHICapabilityValidationResult ValidateRHICapabilities(const RHICapabilities& capabilities);

    /**
     * @brief Build a versioned feature capability report for diagnostics/tooling.
     */
    RHICapabilityReport BuildRHICapabilityReport(const RHICapabilities& capabilities);

    /**
     * @brief Export a stable, human-readable capability report for logs/tools.
     */
    std::string ExportRHICapabilityReportText(const RHICapabilityReport& report);

    /**
     * @brief Export a stable machine-readable capability report for tools/CI.
     */
    std::string ExportRHICapabilityReportJson(const RHICapabilityReport& report);

} // namespace RVX
