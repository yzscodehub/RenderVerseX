#include "MetalDevice.h"
#include "MetalConversions.h"
#include "MetalResources.h"
#include "MetalCommandContext.h"
#include "MetalPipeline.h"
#include "MetalSwapChain.h"
#include "MetalSynchronization.h"
#include "MetalQuery.h"
#include "MetalUpload.h"

namespace RVX
{
    // =============================================================================
    // Factory Function
    // =============================================================================
    std::unique_ptr<IRHIDevice> CreateMetalDevice(const RHIDeviceDesc& desc)
    {
        return std::make_unique<MetalDevice>(desc);
    }

    // =============================================================================
    // MetalDevice Implementation
    // =============================================================================
    MetalDevice::MetalDevice(const RHIDeviceDesc& desc)
    {
        // Get the default Metal device
        m_device = MTLCreateSystemDefaultDevice();
        if (!m_device)
        {
            RVX_RHI_ERROR("Failed to create Metal device - no Metal-capable GPU found");
            return;
        }

        RVX_RHI_INFO("Created Metal device: {}", [[m_device name] UTF8String]);

        // Create command queue
        m_commandQueue = [m_device newCommandQueue];
        if (!m_commandQueue)
        {
            RVX_RHI_ERROR("Failed to create Metal command queue");
            m_device = nil;
            return;
        }

        // Query device capabilities
        QueryCapabilities();

        // Create frame synchronization semaphore for triple buffering
        m_frameSemaphore = dispatch_semaphore_create(kMetalMaxFramesInFlight);

        RVX_RHI_INFO("Metal backend initialized successfully (max {} frames in flight)", kMetalMaxFramesInFlight);
    }

    MetalDevice::~MetalDevice()
    {
        WaitIdle();

        // Release frame semaphore
        if (m_frameSemaphore)
        {
            // Signal any waiting threads before destroying
            for (uint32 i = 0; i < kMetalMaxFramesInFlight; ++i)
            {
                dispatch_semaphore_signal(m_frameSemaphore);
            }
            m_frameSemaphore = nullptr;
        }

        m_commandQueue = nil;
        m_device = nil;
    }

    void MetalDevice::QueryCapabilities()
    {
        m_capabilities.backendType = RHIBackendType::Metal;
        m_capabilities.adapterName = [[m_device name] UTF8String];

        // Basic limits
        m_capabilities.maxTextureSize = 16384;
        m_capabilities.maxTextureSize2D = 16384;
        m_capabilities.maxTextureSize3D = 2048;
        m_capabilities.maxTextureSizeCube = 16384;
        m_capabilities.maxTextureArrayLayers = 2048;
        m_capabilities.maxColorAttachments = 8;

        // Compute limits
        m_capabilities.maxComputeWorkGroupSize[0] = 1024;
        m_capabilities.maxComputeWorkGroupSize[1] = 1024;
        m_capabilities.maxComputeWorkGroupSize[2] = 1024;

        // Feature detection
        m_capabilities.supportsAsyncCompute = true;

        // Check for raytracing support (Apple Silicon)
        if (@available(macOS 11.0, iOS 14.0, *))
        {
            m_capabilities.supportsRaytracing = [m_device supportsRaytracing];
        }

        // Dynamic state and advanced features
        m_capabilities.supportsDepthBounds = false;             // Metal doesn't support depth bounds
        m_capabilities.supportsDynamicLineWidth = false;        // Metal doesn't support dynamic line width
        m_capabilities.supportsSeparateStencilRef = true;       // Metal supports separate stencil refs
        m_capabilities.supportsSplitBarrier = false;            // Metal uses automatic barriers
        m_capabilities.supportsSecondaryCommandBuffer = true;   // Metal supports parallel encoders
        m_capabilities.supportsMemoryBudgetQuery = true;        // Metal supports memory budget
        m_capabilities.supportsPersistentMapping = true;        // Metal supports persistent mapping

        RVX_RHI_INFO("Metal Capabilities:");
        RVX_RHI_INFO("  Adapter: {}", m_capabilities.adapterName);
        RVX_RHI_INFO("  Raytracing: {}", m_capabilities.supportsRaytracing ? "Yes" : "No");
    }

    // =============================================================================
    // Resource Creation
    // =============================================================================
    RHIBufferRef MetalDevice::CreateBuffer(const RHIBufferDesc& desc)
    {
        return MakeRef<MetalBuffer>(m_device, desc);
    }

    RHITextureRef MetalDevice::CreateTexture(const RHITextureDesc& desc)
    {
        return MakeRef<MetalTexture>(m_device, desc);
    }

    RHITextureViewRef MetalDevice::CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
    {
        return MakeRef<MetalTextureView>(static_cast<MetalTexture*>(texture), desc);
    }

    RHISamplerRef MetalDevice::CreateSampler(const RHISamplerDesc& desc)
    {
        return MakeRef<MetalSampler>(m_device, desc);
    }

    RHIShaderRef MetalDevice::CreateShader(const RHIShaderDesc& desc)
    {
        return MakeRef<MetalShader>(m_device, desc);
    }

    // =============================================================================
    // Heap Management
    // =============================================================================
    RHIHeapRef MetalDevice::CreateHeap(const RHIHeapDesc& desc)
    {
        return MakeRef<MetalHeap>(m_device, desc);
    }

    RHITextureRef MetalDevice::CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc)
    {
        auto* metalHeap = static_cast<MetalHeap*>(heap);
        if (!metalHeap || !metalHeap->GetMTLHeap())
        {
            RVX_RHI_ERROR("CreatePlacedTexture: Invalid heap");
            return nullptr;
        }
        return MakeRef<MetalTexture>(metalHeap->GetMTLHeap(), offset, desc);
    }

    RHIBufferRef MetalDevice::CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc)
    {
        auto* metalHeap = static_cast<MetalHeap*>(heap);
        if (!metalHeap || !metalHeap->GetMTLHeap())
        {
            RVX_RHI_ERROR("CreatePlacedBuffer: Invalid heap");
            return nullptr;
        }
        return MakeRef<MetalBuffer>(metalHeap->GetMTLHeap(), offset, desc);
    }

    IRHIDevice::MemoryRequirements MetalDevice::GetTextureMemoryRequirements(const RHITextureDesc& desc)
    {
        MemoryRequirements reqs;

        // Create a temporary texture descriptor to query actual requirements
        MTLTextureDescriptor* texDesc = [[MTLTextureDescriptor alloc] init];

        // Set texture type
        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                texDesc.textureType = MTLTextureType1D;
                break;
            case RHITextureDimension::Texture2D:
                if (desc.arraySize > 1)
                    texDesc.textureType = MTLTextureType2DArray;
                else if (static_cast<uint32>(desc.sampleCount) > 1)
                    texDesc.textureType = MTLTextureType2DMultisample;
                else
                    texDesc.textureType = MTLTextureType2D;
                break;
            case RHITextureDimension::Texture3D:
                texDesc.textureType = MTLTextureType3D;
                break;
            case RHITextureDimension::TextureCube:
                texDesc.textureType = MTLTextureTypeCube;
                break;
        }

        texDesc.pixelFormat = ToMTLPixelFormat(desc.format);
        texDesc.width = desc.width;
        texDesc.height = desc.height;
        texDesc.depth = desc.depth;
        texDesc.mipmapLevelCount = desc.mipLevels;
        texDesc.arrayLength = desc.arraySize;
        texDesc.sampleCount = static_cast<NSUInteger>(desc.sampleCount);
        texDesc.storageMode = MTLStorageModePrivate;

        // Query actual size and alignment from Metal (available since macOS 10.13, iOS 10.0)
        MTLSizeAndAlign sizeAndAlign = [m_device heapTextureSizeAndAlignWithDescriptor:texDesc];

        reqs.size = sizeAndAlign.size;
        reqs.alignment = sizeAndAlign.align;
        return reqs;
    }

    IRHIDevice::MemoryRequirements MetalDevice::GetBufferMemoryRequirements(const RHIBufferDesc& desc)
    {
        MemoryRequirements reqs;

        // Determine storage mode based on memory type
        MTLResourceOptions options = MTLResourceStorageModePrivate;
        switch (desc.memoryType)
        {
            case RHIMemoryType::Default:
                options = MTLResourceStorageModePrivate;
                break;
            case RHIMemoryType::Upload:
                options = MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
                break;
            case RHIMemoryType::Readback:
                options = MTLResourceStorageModeShared;
                break;
        }

        // Query actual size and alignment from Metal (available since macOS 10.13, iOS 10.0)
        MTLSizeAndAlign sizeAndAlign = [m_device heapBufferSizeAndAlignWithLength:desc.size options:options];

        reqs.size = sizeAndAlign.size;
        reqs.alignment = sizeAndAlign.align;
        return reqs;
    }

    // =============================================================================
    // Pipeline Creation
    // =============================================================================
    RHIDescriptorSetLayoutRef MetalDevice::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
    {
        return MakeRef<MetalDescriptorSetLayout>(desc);
    }

    RHIPipelineLayoutRef MetalDevice::CreatePipelineLayout(const RHIPipelineLayoutDesc& desc)
    {
        return MakeRef<MetalPipelineLayout>(desc);
    }

    RHIPipelineRef MetalDevice::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc)
    {
        return MakeRef<MetalGraphicsPipeline>(m_device, desc);
    }

    RHIPipelineRef MetalDevice::CreateComputePipeline(const RHIComputePipelineDesc& desc)
    {
        return MakeRef<MetalComputePipeline>(m_device, desc);
    }

    // =============================================================================
    // Descriptor Set
    // =============================================================================
    RHIDescriptorSetRef MetalDevice::CreateDescriptorSet(const RHIDescriptorSetDesc& desc)
    {
        return MakeRef<MetalDescriptorSet>(desc);
    }

    // =============================================================================
    // Query Pool
    // =============================================================================
    RHIQueryPoolRef MetalDevice::CreateQueryPool(const RHIQueryPoolDesc& desc)
    {
        auto queryPool = MakeRef<MetalQueryPool>(m_device, desc);
        if (!queryPool->IsSupported())
        {
            RVX_RHI_WARN("Metal: Query pool type {} not supported on this device", 
                        static_cast<int>(desc.type));
            return nullptr;
        }
        return queryPool;
    }

    // =============================================================================
    // Command Context
    // =============================================================================
    RHICommandContextRef MetalDevice::CreateCommandContext(RHICommandQueueType type)
    {
        return MakeRef<MetalCommandContext>(this, type);
    }

    void MetalDevice::SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence)
    {
        std::lock_guard<std::mutex> lock(m_submitMutex);

        auto* metalContext = static_cast<MetalCommandContext*>(context);

        // Add completion handler to signal frame semaphore when GPU finishes
        if (m_frameInFlight)
        {
            id<MTLCommandBuffer> cmdBuffer = metalContext->GetCommandBuffer();
            if (cmdBuffer)
            {
                __block dispatch_semaphore_t sem = m_frameSemaphore;
                [cmdBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
                    dispatch_semaphore_signal(sem);
                }];
            }
        }

        metalContext->Submit(signalFence);
    }

    void MetalDevice::SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence)
    {
        std::lock_guard<std::mutex> lock(m_submitMutex);

        for (size_t i = 0; i < contexts.size(); ++i)
        {
            auto* metalContext = static_cast<MetalCommandContext*>(contexts[i]);
            // Only signal fence on last submission
            metalContext->Submit(i == contexts.size() - 1 ? signalFence : nullptr);
        }
    }

    // =============================================================================
    // SwapChain
    // =============================================================================
    RHISwapChainRef MetalDevice::CreateSwapChain(const RHISwapChainDesc& desc)
    {
        return MakeRef<MetalSwapChain>(this, desc);
    }

    // =============================================================================
    // Synchronization
    // =============================================================================
    RHIFenceRef MetalDevice::CreateFence(uint64 initialValue)
    {
        return MakeRef<MetalFence>(m_device, initialValue);
    }

    void MetalDevice::WaitForFence(RHIFence* fence, uint64 value)
    {
        if (fence)
        {
            static_cast<MetalFence*>(fence)->Wait(value);
        }
    }

    void MetalDevice::WaitIdle()
    {
        // Create a command buffer and wait for it to complete
        id<MTLCommandBuffer> cmdBuffer = [m_commandQueue commandBuffer];
        [cmdBuffer commit];
        [cmdBuffer waitUntilCompleted];
    }

    // =============================================================================
    // Frame Management
    // =============================================================================
    void MetalDevice::BeginFrame()
    {
        // Wait for an available frame slot (blocks if all frames are in flight)
        // This prevents CPU from getting too far ahead of GPU
        dispatch_semaphore_wait(m_frameSemaphore, DISPATCH_TIME_FOREVER);
        m_frameInFlight = true;
    }

    void MetalDevice::EndFrame()
    {
        // Note: The frame semaphore is signaled when command buffer completes
        // This is done via addCompletedHandler in SubmitCommandContext
        m_currentFrameIndex = (m_currentFrameIndex + 1) % kMetalMaxFramesInFlight;
        m_frameInFlight = false;
    }

    // =============================================================================
    // Upload Resources
    // =============================================================================
    RHIStagingBufferRef MetalDevice::CreateStagingBuffer(const RHIStagingBufferDesc& desc)
    {
        auto stagingBuffer = MakeRef<MetalStagingBuffer>(m_device, desc);
        if (!stagingBuffer->GetMTLBuffer())
        {
            RVX_RHI_ERROR("Metal: Failed to create staging buffer");
            return nullptr;
        }
        return stagingBuffer;
    }

    RHIRingBufferRef MetalDevice::CreateRingBuffer(const RHIRingBufferDesc& desc)
    {
        auto ringBuffer = MakeRef<MetalRingBuffer>(m_device, desc);
        if (!ringBuffer->GetMTLBuffer())
        {
            RVX_RHI_ERROR("Metal: Failed to create ring buffer");
            return nullptr;
        }
        return ringBuffer;
    }

    // =============================================================================
    // Memory Statistics
    // =============================================================================
    RHIMemoryStats MetalDevice::GetMemoryStats() const
    {
        RHIMemoryStats stats = {};

        // Query Metal device memory info
        if (@available(macOS 10.13, iOS 11.0, *))
        {
            stats.currentUsageBytes = [m_device currentAllocatedSize];
        }

        // Metal doesn't provide explicit budget, use recommended working set
        if (@available(macOS 10.13, iOS 11.0, *))
        {
            stats.budgetBytes = [m_device recommendedMaxWorkingSetSize];
        }

        stats.totalAllocated = stats.currentUsageBytes;
        stats.totalUsed = stats.currentUsageBytes;

        return stats;
    }

    // =============================================================================
    // Debug Resource Groups
    // =============================================================================
    void MetalDevice::BeginResourceGroup(const char* name)
    {
        (void)name;
        // Metal doesn't have native resource grouping
    }

    void MetalDevice::EndResourceGroup()
    {
        // No-op for Metal
    }

} // namespace RVX
