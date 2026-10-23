#pragma once

#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/TransientResourcePool.h"
#include "RHI/RHIHeap.h"
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    inline constexpr uint64 RVX_RENDER_GRAPH_ESTIMATED_MEMORY_SATURATED =
        std::numeric_limits<uint64>::max();

    [[nodiscard]] constexpr uint64 SaturatingAddRenderGraphEstimatedMemory(
        uint64 left,
        uint64 right) noexcept
    {
        return right > RVX_RENDER_GRAPH_ESTIMATED_MEMORY_SATURATED - left
            ? RVX_RENDER_GRAPH_ESTIMATED_MEMORY_SATURATED
            : left + right;
    }

    [[nodiscard]] constexpr uint64 SaturatingMultiplyRenderGraphEstimatedMemory(
        uint64 left,
        uint64 right) noexcept
    {
        return left != 0 &&
                right > RVX_RENDER_GRAPH_ESTIMATED_MEMORY_SATURATED / left
            ? RVX_RENDER_GRAPH_ESTIMATED_MEMORY_SATURATED
            : left * right;
    }

    [[nodiscard]] constexpr uint64 AlignRenderGraphEstimatedMemory(
        uint64 value,
        uint64 alignment) noexcept
    {
        if (alignment == 0 || value == 0)
        {
            return value;
        }

        const uint64 remainder = value % alignment;
        if (remainder == 0)
        {
            return value;
        }

        const uint64 padding = alignment - remainder;
        return SaturatingAddRenderGraphEstimatedMemory(value, padding);
    }

    /**
     * @brief Conservative, overflow-safe backend-neutral texture estimate.
     *
     * Saturation is intentional: callers must prefer an unmistakable maximum
     * estimate to a wrapped low footprint that could pass a memory policy.
     */
    [[nodiscard]] inline uint64 EstimateRenderGraphTextureMemorySize(
        const RHITextureDesc& desc) noexcept
    {
        const uint64 formatBytesPerPixel = GetFormatBytesPerPixel(desc.format);
        const uint64 bytesPerPixel = formatBytesPerPixel != 0
            ? formatBytesPerPixel
            : 4u;
        const uint64 arraySize = desc.arraySize != 0 ? desc.arraySize : 1u;
        const uint32 mipLevels = desc.mipLevels != 0 ? desc.mipLevels : 1u;
        const uint64 sampleCount = static_cast<uint32>(desc.sampleCount) != 0
            ? static_cast<uint32>(desc.sampleCount)
            : 1u;
        uint64 width = desc.width != 0 ? desc.width : 1u;
        uint64 height = desc.height != 0 ? desc.height : 1u;
        uint64 depth = desc.depth != 0 ? desc.depth : 1u;
        uint64 totalSize = 0;

        for (uint32 mip = 0; mip < mipLevels; ++mip)
        {
            uint64 mipSize = SaturatingMultiplyRenderGraphEstimatedMemory(
                width, height);
            mipSize = SaturatingMultiplyRenderGraphEstimatedMemory(
                mipSize, depth);
            mipSize = SaturatingMultiplyRenderGraphEstimatedMemory(
                mipSize, bytesPerPixel);
            mipSize = SaturatingMultiplyRenderGraphEstimatedMemory(
                mipSize, arraySize);
            totalSize = SaturatingAddRenderGraphEstimatedMemory(
                totalSize, mipSize);
            if (totalSize == RVX_RENDER_GRAPH_ESTIMATED_MEMORY_SATURATED)
            {
                return totalSize;
            }

            if (width == 1 && height == 1 && depth == 1)
            {
                const uint64 remainingMipCount =
                    static_cast<uint64>(mipLevels - mip - 1u);
                totalSize = SaturatingAddRenderGraphEstimatedMemory(
                    totalSize,
                    SaturatingMultiplyRenderGraphEstimatedMemory(
                        mipSize, remainingMipCount));
                break;
            }

            width = width > 1 ? width / 2u : 1u;
            height = height > 1 ? height / 2u : 1u;
            depth = depth > 1 ? depth / 2u : 1u;
        }

        totalSize = SaturatingMultiplyRenderGraphEstimatedMemory(
            totalSize, sampleCount);
        return AlignRenderGraphEstimatedMemory(totalSize, 65536u);
    }

    /** @brief Conservative, overflow-safe aligned buffer estimate. */
    [[nodiscard]] constexpr uint64 EstimateRenderGraphBufferMemorySize(
        const RHIBufferDesc& desc) noexcept
    {
        return AlignRenderGraphEstimatedMemory(desc.size, 256u);
    }

    enum class ResourceType
    {
        Texture,
        Buffer,
    };

    enum class RGAccessType
    {
        Read,
        Write,
        ReadWrite,
    };

    constexpr bool IsRenderGraphQueueAffinityCompatible(
        RenderGraphPassType passType,
        RenderGraphQueueAffinity queueAffinity) noexcept
    {
        switch (passType)
        {
            case RenderGraphPassType::Graphics:
            case RenderGraphPassType::RayTracing:
                return queueAffinity == RenderGraphQueueAffinity::Default ||
                       queueAffinity == RenderGraphQueueAffinity::Graphics;
            case RenderGraphPassType::Compute:
                return queueAffinity == RenderGraphQueueAffinity::Default ||
                       queueAffinity == RenderGraphQueueAffinity::Graphics ||
                       queueAffinity == RenderGraphQueueAffinity::Compute;
            case RenderGraphPassType::Copy:
                return queueAffinity == RenderGraphQueueAffinity::Default ||
                       queueAffinity == RenderGraphQueueAffinity::Graphics ||
                       queueAffinity == RenderGraphQueueAffinity::Copy;
            default:
                return false;
        }
    }

    // =============================================================================
    // Resource Lifetime - for memory aliasing
    // =============================================================================
    struct ResourceLifetime
    {
        uint32 firstUsePass = UINT32_MAX;  // First pass that uses this resource
        uint32 lastUsePass = 0;             // Last pass that uses this resource
        uint64 memorySize = 0;              // Required memory size
        uint64 alignment = 0;               // Alignment requirement
        bool isUsed = false;                // Whether this resource is actually used
    };

    // =============================================================================
    // Memory Alias - assigned heap and offset for a resource
    // =============================================================================
    struct MemoryAlias
    {
        uint32 heapIndex = UINT32_MAX;      // Which heap this resource is allocated from
        uint64 heapOffset = 0;               // Offset within the heap
        bool isAliased = false;              // Whether this resource shares memory with others
        ResourceType predecessorType = ResourceType::Texture;
        uint32 predecessorResourceIndex = RVX_INVALID_INDEX;
    };

    enum class RGPhysicalBinding : uint8
    {
        Unrealized = 0,
        Imported,
        Owned,
        Pooled,
        BorrowedValidation,
    };

    // =============================================================================
    // Transient Heap - a single memory heap for aliased resources
    // =============================================================================
    struct TransientHeap
    {
        uint64 size = 0;                     // Total heap size
        uint32 resourceCount = 0;            // Number of resources using this heap
        
        // RHI Heap handle (set during resource creation)
        RHIHeapRef heap;
    };

    struct TextureResource
    {
        RHITextureDesc desc;
        RGPhysicalBinding binding = RGPhysicalBinding::Unrealized;
        RHITextureRef strongBinding;
        RHITexture* borrowedValidationBinding = nullptr;
        std::optional<TransientTextureLease> pooledLease;
        RHIResourceState initialState = RHIResourceState::Undefined;
        RHIResourceState currentState = RHIResourceState::Undefined;
        RHITextureAccessSnapshot initialAccessSnapshot;
        RHITextureAccessSnapshot currentAccessSnapshot;
        std::unordered_map<uint32, RHIResourceState> subresourceStates;
        std::unordered_map<uint32, RHIAccessSnapshot> subresourceAccesses;
        bool hasSubresourceTracking = false;
        std::optional<RHIResourceState> exportState;
        std::optional<RHIAccessSnapshot> exportAccess;
        std::optional<RHIAccessSnapshot> poolTerminalAccess;
        bool imported = false;
        std::string importedDiagnosticName;
        RGImportedResourceClass importedResourceClass =
            RGImportedResourceClass::External;
        
        // Memory aliasing
        ResourceLifetime lifetime;
        MemoryAlias alias;
        
        bool IsPooled() const
        {
            return binding == RGPhysicalBinding::Pooled;
        }

        RHITexture* GetTexture() const
        {
            if (binding == RGPhysicalBinding::Pooled)
                return pooledLease ? pooledLease->texture : nullptr;
            if (binding == RGPhysicalBinding::Imported ||
                binding == RGPhysicalBinding::Owned)
            {
                return strongBinding.Get();
            }
            if (binding == RGPhysicalBinding::BorrowedValidation)
                return borrowedValidationBinding;
            return nullptr;
        }
    };

    struct BufferResource
    {
        RHIBufferDesc desc;
        RGPhysicalBinding binding = RGPhysicalBinding::Unrealized;
        RHIBufferRef strongBinding;
        RHIBuffer* borrowedValidationBinding = nullptr;
        std::optional<TransientBufferLease> pooledLease;
        RHIResourceState initialState = RHIResourceState::Undefined;
        RHIResourceState currentState = RHIResourceState::Undefined;
        RHIBufferAccessSnapshot initialAccessSnapshot;
        RHIBufferAccessSnapshot currentAccessSnapshot;
        std::optional<RHIResourceState> exportState;
        std::optional<RHIAccessSnapshot> exportAccess;
        std::optional<RHIAccessSnapshot> poolTerminalAccess;
        struct RangeState
        {
            uint64 offset = 0;
            uint64 size = 0;
            RHIResourceState state = RHIResourceState::Common;
            RHIAccessSnapshot access;
        };
        std::vector<RangeState> rangeStates;
        bool hasRangeTracking = false;
        bool imported = false;
        std::string importedDiagnosticName;
        RGImportedResourceClass importedResourceClass =
            RGImportedResourceClass::External;
        
        // Memory aliasing
        ResourceLifetime lifetime;
        MemoryAlias alias;
        
        bool IsPooled() const
        {
            return binding == RGPhysicalBinding::Pooled;
        }

        RHIBuffer* GetBuffer() const
        {
            if (binding == RGPhysicalBinding::Pooled)
                return pooledLease ? pooledLease->buffer : nullptr;
            if (binding == RGPhysicalBinding::Imported ||
                binding == RGPhysicalBinding::Owned)
            {
                return strongBinding.Get();
            }
            if (binding == RGPhysicalBinding::BorrowedValidation)
                return borrowedValidationBinding;
            return nullptr;
        }
    };

    struct TextureViewResource
    {
        RGTextureHandle texture;
        RHITextureViewDesc desc;
        std::string debugName;
        RHITextureViewRef realizedView;
    };

    struct ResourceUsage
    {
        ResourceType type = ResourceType::Texture;
        uint32 index = RVX_INVALID_INDEX;
        RHIResourceState desiredState = RHIResourceState::Common;
        RGAccessDesc logicalAccess;
        RHIAccessSnapshot desiredAccess;
        RGAccessType access = RGAccessType::Read;
        RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve;
        RHIShaderStage stages = RHIShaderStage::AllGraphics;  // Shader stages that will access this resource
        RHISubresourceRange subresourceRange = RHISubresourceRange::All();
        bool hasSubresourceRange = false;
        uint64 offset = 0;
        uint64 size = RVX_WHOLE_SIZE;
        bool hasRange = false;
        RHIAccessSnapshot plannedBeforeAccess;
        bool hasPlannedBeforeAccess = false;
    };

    // =============================================================================
    // Aliasing Barrier Info
    // =============================================================================
    struct AliasingBarrier
    {
        ResourceType beforeType = ResourceType::Texture;
        ResourceType afterType = ResourceType::Texture;
        uint32 beforeResourceIndex = RVX_INVALID_INDEX;  // Resource that was using this memory before
        uint32 afterResourceIndex = RVX_INVALID_INDEX;   // Resource that will use this memory now
    };

    /** @brief Allocation-free texture barrier recipe produced by the compiler. */
    struct PlannedTextureBarrier
    {
        uint32 resourceIndex = RVX_INVALID_INDEX;
        RHITextureBarrier barrier;
        bool resolveBeforeFromLease = false;
    };

    /** @brief Allocation-free buffer barrier recipe produced by the compiler. */
    struct PlannedBufferBarrier
    {
        uint32 resourceIndex = RVX_INVALID_INDEX;
        RHIBufferBarrier barrier;
        bool resolveBeforeFromLease = false;
    };

    struct Pass
    {
        std::string name;
        RenderGraphPassType type = RenderGraphPassType::Graphics;
        RenderGraphQueueAffinity queueAffinity =
            RenderGraphQueueAffinity::Default;
        RenderGraph::DiagnosticExecutionQueue plannedExecutionQueue =
            RenderGraph::DiagnosticExecutionQueue::Graphics;
        std::vector<ResourceUsage> usages;
        std::vector<uint32> readTextures;
        std::vector<uint32> writeTextures;
        std::vector<uint32> readBuffers;
        std::vector<uint32> writeBuffers;
        bool culled = false;
        bool executedLastRun = false;
        bool semanticExecutionSucceededLastRun = true;
        std::string semanticExecutionDiagnostic;
        RenderGraph::DiagnosticExecutionQueue lastExecutionQueue = RenderGraph::DiagnosticExecutionQueue::Unknown;
        uint32 lastExecutionSerial = RVX_INVALID_INDEX;
        uint64 lastCpuDurationNanoseconds = 0;
        std::vector<PlannedTextureBarrier> textureBarriers;
        std::vector<PlannedBufferBarrier> bufferBarriers;
        std::vector<PlannedTextureBarrier> postTextureBarriers;
        std::vector<PlannedBufferBarrier> postBufferBarriers;
        std::vector<AliasingBarrier> aliasingBarriers;  // For memory aliasing
        std::function<void(RenderGraphPassContext&)> execute;
    };

    struct InitialQueueReleaseBatch
    {
        RenderGraph::DiagnosticExecutionQueue queue =
            RenderGraph::DiagnosticExecutionQueue::Unknown;
        std::vector<uint32> targetPassIndices;
        std::vector<PlannedTextureBarrier> textureBarriers;
        std::vector<PlannedBufferBarrier> bufferBarriers;
        bool targetsTerminal = false;
    };

    /** @brief Definition/plan realization state; execution ownership is separate. */
    enum class RenderGraphRuntimeState : uint8
    {
        Recording = 0,
        Compiled,
        ResourcesRealized,
        Recorded,
        Transferred,
        CompileFailed,
    };

    struct RenderGraphImpl
    {
        struct QueueSyncPoint
        {
            RenderGraph::DiagnosticExecutionQueue sourceQueue = RenderGraph::DiagnosticExecutionQueue::Unknown;
            RenderGraph::DiagnosticExecutionQueue targetQueue = RenderGraph::DiagnosticExecutionQueue::Unknown;
            RenderGraph::DiagnosticSyncReason reason = RenderGraph::DiagnosticSyncReason::CrossQueueDependency;
            uint64 fenceValue = 0;
            uint32 sourcePassIndex = RVX_INVALID_INDEX;
            uint32 targetPassIndex = RVX_INVALID_INDEX;
        };

        RenderGraph* owner = nullptr;
        IRHIDevice* device = nullptr;
        TransientResourcePool* transientResourcePool = nullptr;
        IRenderGraphPassTimestampRecorder* passTimestampRecorder = nullptr;
        RHICapabilities capabilitySnapshot;
        bool hasCapabilitySnapshot = false;
        std::vector<TextureResource> textures;
        std::vector<BufferResource> buffers;
        // Compatibility query snapshots remain address-stable while a
        // definition is being recorded. Resource vectors may reallocate when
        // pass setup declares or imports additional resources.
        std::deque<RHITextureDesc> textureDescSnapshots;
        std::deque<RHIBufferDesc> bufferDescSnapshots;
        std::vector<TextureViewResource> textureViews;
        std::vector<Ref<RefCounted>> executionResources;
        std::vector<Pass> passes;
        std::vector<uint32> executionOrder;
        std::vector<std::vector<uint32>> passDependencies;
        std::vector<std::vector<uint32>> passDependents;
        std::vector<InitialQueueReleaseBatch> initialQueueReleaseBatches;
        std::vector<QueueSyncPoint> lastQueueSyncs;
        RenderGraph::CompileStats stats;
        std::vector<std::string> compileDiagnostics;
        RenderGraph::QueueExecutionMode queueExecutionMode =
            RenderGraph::QueueExecutionMode::GraphicsOnly;
        bool parallelRecordingEnabled = false;
        // Memory aliasing
        std::vector<TransientHeap> transientHeaps;
        bool enableMemoryAliasing = false;
        bool memoryAliasingRequested = false;
        bool canonicalizeTransientPoolAccess = false;
        TransientResourcePool::Stats executionPoolStats;
        RHIDescriptorDiagnostics executionDescriptorDiagnostics;

        void AppendTextureResource(TextureResource&& resource)
        {
            textureDescSnapshots.push_back(resource.desc);
            textures.push_back(std::move(resource));
            RVX_ASSERT(textureDescSnapshots.size() == textures.size());
        }

        void AppendBufferResource(BufferResource&& resource)
        {
            bufferDescSnapshots.push_back(resource.desc);
            buffers.push_back(std::move(resource));
            RVX_ASSERT(bufferDescSnapshots.size() == buffers.size());
        }
        
        // Aliasing statistics
        uint64 totalMemoryWithoutAliasing = 0;
        uint64 totalMemoryWithAliasing = 0;
        uint32 aliasedTextureCount = 0;
        uint32 aliasedBufferCount = 0;
        uint32 compatibilityStateProjectionCount = 0;
        RenderGraphRuntimeState runtimeState =
            RenderGraphRuntimeState::Recording;
    };

    void CompileRenderGraph(RenderGraphImpl& graph);
    bool RealizeRenderGraphResources(RenderGraphImpl& graph);
    std::vector<RenderGraph::PassDiagnostic> BuildRenderGraphPassDiagnostics(const RenderGraphImpl& graph);
    RenderGraph::SubmissionPlan BuildRenderGraphSubmissionPlan(
        const std::vector<RenderGraph::PassDiagnostic>& passes,
        const std::vector<uint32>& executionOrder);
    RenderGraph::SubmissionPlan BuildRenderGraphSubmissionPlan(const RenderGraphImpl& graph);
    void ExecuteRenderGraph(RenderGraphImpl& graph, RHICommandContext& ctx);
    bool RecordRenderGraphQueueSubmission(
        RenderGraphImpl& graph,
        RenderGraph::RecordedQueueSubmission& submission);

    // Memory aliasing functions
    void CalculateResourceLifetimes(RenderGraphImpl& graph);
    void ComputeMemoryAliases(RenderGraphImpl& graph);
    
} // namespace RVX
