#pragma once

#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/TransientResourcePool.h"
#include "RHI/RHIHeap.h"
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
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
        bool imported = false;
        
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
        RenderGraph::DiagnosticExecutionQueue plannedExecutionQueue =
            RenderGraph::DiagnosticExecutionQueue::Graphics;
        std::vector<ResourceUsage> usages;
        std::vector<uint32> readTextures;
        std::vector<uint32> writeTextures;
        std::vector<uint32> readBuffers;
        std::vector<uint32> writeBuffers;
        bool culled = false;
        bool executedLastRun = false;
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
