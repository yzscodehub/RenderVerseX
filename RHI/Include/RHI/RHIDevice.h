#pragma once

#include "RHI/RHIResources.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"
#include "RHI/RHISampler.h"
#include "RHI/RHIShader.h"
#include "RHI/RHIPipeline.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHISwapChain.h"
#include "RHI/RHISynchronization.h"
#include "RHI/RHICapabilities.h"
#include "RHI/RHIHeap.h"
#include "RHI/RHIQuery.h"
#include "RHI/RHIUpload.h"

namespace RVX
{
    // =============================================================================
    // Memory Statistics
    // =============================================================================
    struct RHIMemoryStats
    {
        // Total statistics
        uint64 totalAllocated = 0;       // Total allocated bytes
        uint64 totalUsed = 0;            // Actually used bytes
        uint64 peakUsage = 0;            // Peak usage bytes
        uint32 allocationCount = 0;      // Number of allocations

        // Per-type statistics
        uint64 bufferMemory = 0;         // Buffer memory usage
        uint64 textureMemory = 0;        // Texture memory usage
        uint64 renderTargetMemory = 0;   // Render target/depth stencil usage

        // Budget info (requires supportsMemoryBudgetQuery)
        uint64 budgetBytes = 0;          // GPU memory budget
        uint64 currentUsageBytes = 0;    // Current usage
    };
    // =============================================================================
    // Device Description
    // =============================================================================
    struct RHIDeviceDesc
    {
        bool enableDebugLayer = true;
        bool enableGPUValidation = false;
        uint32 preferredAdapterIndex = 0;  // 0 = auto-select
        const char* applicationName = "RenderVerseX";
    };

    // =============================================================================
    // Device Interface
    // =============================================================================
    class IRHIDevice
    {
    public:
        virtual ~IRHIDevice() = default;

        // =========================================================================
        // Resource Creation
        // =========================================================================
        virtual RHIBufferRef CreateBuffer(const RHIBufferDesc& desc) = 0;
        virtual RHITextureRef CreateTexture(const RHITextureDesc& desc) = 0;
        virtual RHITextureViewRef CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc = {}) = 0;
        virtual RHISamplerRef CreateSampler(const RHISamplerDesc& desc) = 0;
        virtual RHIShaderRef CreateShader(const RHIShaderDesc& desc) = 0;

        // =========================================================================
        // Memory Heap Management (for Placed Resources / Memory Aliasing)
        // =========================================================================
        virtual RHIHeapRef CreateHeap(const RHIHeapDesc& desc) = 0;
        virtual RHITextureRef CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc) = 0;
        virtual RHIBufferRef CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc) = 0;
        
        // Query memory requirements before creating placed resources
        struct MemoryRequirements
        {
            uint64 size = 0;
            uint64 alignment = 0;
        };
        virtual MemoryRequirements GetTextureMemoryRequirements(const RHITextureDesc& desc) = 0;
        virtual MemoryRequirements GetBufferMemoryRequirements(const RHIBufferDesc& desc) = 0;

        // =========================================================================
        // Pipeline Creation
        // =========================================================================
        virtual RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc) = 0;
        virtual RHIPipelineLayoutRef CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) = 0;
        virtual RHIPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) = 0;
        virtual RHIPipelineRef CreateComputePipeline(const RHIComputePipelineDesc& desc) = 0;

        // =========================================================================
        // Descriptor Set
        // =========================================================================
        virtual RHIDescriptorSetRef CreateDescriptorSet(const RHIDescriptorSetDesc& desc) = 0;

        // =========================================================================
        // Query Pool
        // =========================================================================
        virtual RHIQueryPoolRef CreateQueryPool(const RHIQueryPoolDesc& desc) = 0;

        // =========================================================================
        // Command Context
        // =========================================================================
        virtual RHICommandContextRef CreateCommandContext(RHICommandQueueType type) = 0;
        virtual void SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence = nullptr) = 0;
        virtual void SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence = nullptr) = 0;

        // =========================================================================
        // SwapChain
        // =========================================================================
        virtual RHISwapChainRef CreateSwapChain(const RHISwapChainDesc& desc) = 0;

        // =========================================================================
        // Synchronization
        // =========================================================================
        virtual RHIFenceRef CreateFence(uint64 initialValue = 0) = 0;
        virtual void WaitForFence(RHIFence* fence, uint64 value) = 0;
        virtual void WaitIdle() = 0;

        // =========================================================================
        // Frame Management
        // =========================================================================
        virtual void BeginFrame() = 0;
        virtual void EndFrame() = 0;
        virtual uint32 GetCurrentFrameIndex() const = 0;

        // =========================================================================
        // Upload Resources
        // =========================================================================
        
        /**
         * @brief Create a staging buffer for CPU->GPU data transfer
         * @param desc Staging buffer description
         * @return Staging buffer, nullptr on failure
         */
        virtual RHIStagingBufferRef CreateStagingBuffer(const RHIStagingBufferDesc& desc) = 0;

        /**
         * @brief Create a ring buffer for per-frame temporary data
         * @param desc Ring buffer description
         * @return Ring buffer, nullptr on failure
         */
        virtual RHIRingBufferRef CreateRingBuffer(const RHIRingBufferDesc& desc) = 0;

        // =========================================================================
        // Memory Statistics
        // =========================================================================
        
        /**
         * @brief Get current GPU memory statistics
         * @return Memory statistics structure
         */
        virtual RHIMemoryStats GetMemoryStats() const = 0;

        // =========================================================================
        // Debug Resource Groups
        // =========================================================================
        
        /**
         * @brief Begin a resource creation group for debug tools (PIX/RenderDoc)
         * Resources created in this group will be shown together
         * @param name Group name
         */
        virtual void BeginResourceGroup(const char* name) = 0;

        /**
         * @brief End the current resource creation group
         */
        virtual void EndResourceGroup() = 0;

        // =========================================================================
        // Capabilities
        // =========================================================================
        virtual const RHICapabilities& GetCapabilities() const = 0;
        virtual RHIBackendType GetBackendType() const = 0;
    };

    // =============================================================================
    // Device Factory
    // =============================================================================
    std::unique_ptr<IRHIDevice> CreateRHIDevice(RHIBackendType backend, const RHIDeviceDesc& desc);

} // namespace RVX
