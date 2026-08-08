#pragma once

/**
 * @file RenderGraph.h
 * @brief Frame graph and automatic resource management
 * 
 * RenderGraph provides automatic resource state tracking, barrier insertion,
 * pass culling, and memory aliasing for transient resources.
 */

#include "RHI/RHI.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace RVX
{
    inline constexpr const char* RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID = "RVX.RenderGraph.Diagnostics";
    inline constexpr uint32 RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION = 6;

    class RenderSubmissionResourceBatch;
    class TransientResourcePool;

    // =============================================================================
    // Render Graph Handle Types
    // =============================================================================
    struct RGTextureHandle
    {
        uint32 index = RVX_INVALID_INDEX;
        bool hasSubresourceRange = false;
        RHISubresourceRange subresourceRange = RHISubresourceRange::All();
        uint64 graphIdentity = 0;
        uint64 recordingGeneration = 0;

        bool IsValid() const { return index != RVX_INVALID_INDEX; }
        
        // Subresource access
        RGTextureHandle Subresource(uint32 mipLevel, uint32 arraySlice = 0) const;
        RGTextureHandle MipRange(uint32 baseMip, uint32 mipCount) const;
    };

    struct RGBufferHandle
    {
        uint32 index = RVX_INVALID_INDEX;
        bool hasRange = false;
        uint64 rangeOffset = 0;
        uint64 rangeSize = RVX_WHOLE_SIZE;
        uint64 graphIdentity = 0;
        uint64 recordingGeneration = 0;

        bool IsValid() const { return index != RVX_INVALID_INDEX; }

        // Range access
        RGBufferHandle Range(uint64 offset, uint64 size) const;
    };

    // =============================================================================
    // Render Graph Pass Type
    // =============================================================================
    enum class RenderGraphPassType
    {
        Graphics,
        Compute,
        RayTracing,
        Copy,
    };

    // =============================================================================
    // Render Graph Builder
    // =============================================================================
    class RenderGraphBuilder
    {
    public:
        // Read resources
        RGTextureHandle Read(RGTextureHandle texture, RHIShaderStage stages = RHIShaderStage::AllGraphics);
        RGTextureHandle Read(RGTextureHandle texture,
                             RHIResourceState state,
                             RHIShaderStage stages = RHIShaderStage::AllGraphics);
        RGTextureHandle Read(RGTextureHandle texture, const RHIAccessSnapshot& access);
        RGBufferHandle Read(RGBufferHandle buffer, RHIShaderStage stages = RHIShaderStage::AllGraphics);
        RGBufferHandle Read(RGBufferHandle buffer,
                            RHIResourceState state,
                            RHIShaderStage stages = RHIShaderStage::AllGraphics);
        RGBufferHandle Read(RGBufferHandle buffer, const RHIAccessSnapshot& access);

        // Write resources
        RGTextureHandle Write(RGTextureHandle texture,
                              RHIResourceState state = RHIResourceState::RenderTarget,
                              RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve);
        RGTextureHandle Write(RGTextureHandle texture,
                              const RHIAccessSnapshot& access,
                              RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve);
        RGBufferHandle Write(RGBufferHandle buffer,
                             RHIResourceState state = RHIResourceState::UnorderedAccess,
                             RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve);
        RGBufferHandle Write(RGBufferHandle buffer,
                             const RHIAccessSnapshot& access,
                             RHIDiscardIntent discardIntent = RHIDiscardIntent::Preserve);

        // Read-write resources
        RGTextureHandle ReadWrite(RGTextureHandle texture);
        RGTextureHandle ReadWrite(RGTextureHandle texture, const RHIAccessSnapshot& access);
        RGBufferHandle ReadWrite(RGBufferHandle buffer);
        RGBufferHandle ReadWrite(RGBufferHandle buffer, const RHIAccessSnapshot& access);

        // Subresource-level access
        RGTextureHandle ReadMip(RGTextureHandle texture, uint32 mipLevel);
        RGTextureHandle WriteMip(RGTextureHandle texture, uint32 mipLevel);

        // Depth-stencil
        void SetDepthStencil(RGTextureHandle texture, bool depthWrite = true, bool stencilWrite = false);

    private:
        class Impl;
        Impl* m_impl = nullptr;
        friend class RenderGraph;
    };

    // =============================================================================
    // Render Graph
    // =============================================================================
    class RenderGraph
    {
    public:
        enum class QueueExecutionMode : uint8
        {
            GraphicsOnly = 0,
            /** @brief Record and submit an explicit Graphics/Compute/Copy DAG. */
            MultiQueue,
        };

        RenderGraph();
        ~RenderGraph();

        void SetDevice(IRHIDevice* device);
        void SetTransientResourcePool(TransientResourcePool* pool);

        /**
         * @brief Select the physical queue contract used while recording passes.
         * @return False when passes have already been recorded and the mode cannot change.
         *
         * GraphicsOnly is the fail-closed default: Graphics, Compute, and Copy
         * passes all declare Graphics-domain resource access. MultiQueue must
         * be selected before AddPass so barriers and execution agree.
         */
        bool SetQueueExecutionMode(QueueExecutionMode mode);
        QueueExecutionMode GetQueueExecutionMode() const;

        /**
         * @brief Enable independent queue-batch recording on Core JobSystem workers.
         *
         * Disabled by default until every participating pass callback is audited
         * for concurrent recording. Batches in the same dependency level may run
         * concurrently; dependency levels always complete in order.
         */
        void SetParallelRecordingEnabled(bool enabled) noexcept;
        [[nodiscard]] bool IsParallelRecordingEnabled() const noexcept;

        /** @brief Stable non-zero identity for this graph instance. */
        uint64 GetGraphIdentity() const;

        /** @brief Non-zero resource-recording generation; changes on Clear(). */
        uint64 GetRecordingGeneration() const;

        // Create transient resources
        RGTextureHandle CreateTexture(const RHITextureDesc& desc);
        RGBufferHandle CreateBuffer(const RHIBufferDesc& desc);

        // Import external resources
        RGTextureHandle ImportTexture(RHITexture* texture, RHIResourceState initialState);
        RGBufferHandle ImportBuffer(RHIBuffer* buffer, RHIResourceState initialState);
        RGTextureHandle ImportTexture(RHITexture* texture, const RHITextureAccessSnapshot& initialAccess);
        RGBufferHandle ImportBuffer(RHIBuffer* buffer, const RHIBufferAccessSnapshot& initialAccess);

        // Export final state for external usage
        void SetExportState(RGTextureHandle texture, RHIResourceState finalState);
        void SetExportState(RGBufferHandle buffer, RHIResourceState finalState);
        void SetExportAccess(RGTextureHandle texture, const RHIAccessSnapshot& finalAccess);
        void SetExportAccess(RGBufferHandle buffer, const RHIAccessSnapshot& finalAccess);

        /** @brief Get the realized snapshot after Execute, or the supplied initial snapshot otherwise. */
        RHITextureAccessSnapshot GetRealizedAccess(RGTextureHandle texture) const;
        RHIBufferAccessSnapshot GetRealizedAccess(RGBufferHandle buffer) const;

        // Get actual RHI resources from handles (valid after Compile)
        RHITexture* GetTexture(RGTextureHandle handle) const;
        RHIBuffer* GetBuffer(RGBufferHandle handle) const;

        // Get resource descriptions
        const RHITextureDesc* GetTextureDesc(RGTextureHandle handle) const;
        const RHIBufferDesc* GetBufferDesc(RGBufferHandle handle) const;

        // Add passes
        template<typename Data>
        void AddPass(
            const char* name,
            RenderGraphPassType type,
            std::function<void(RenderGraphBuilder&, Data&)> setup,
            std::function<void(const Data&, RHICommandContext&)> execute);

        // Compile the graph
        void Compile();

        // Execute the graph (graphics only)
        void Execute(RHICommandContext& ctx);

        struct RecordedQueueSubmission
        {
            RHIQueueSubmissionPlan plan;
            std::vector<RHICommandContextRef> ownedContexts;
        };

        /** @brief Record one independent command context per planned queue batch. */
        bool RecordQueueSubmission(RecordedQueueSubmission& submission);

        /**
         * @brief Rebuild this recorded graph with Graphics-only access domains.
         * @note This is the only legal fallback after a MultiQueue plan cannot
         * be recorded; MultiQueue barriers must never execute on one context.
         */
        bool RecompileGraphicsOnly();

        /**
         * @brief Execute the graph with async compute support
         * @param graphicsCtx Graphics command context
         * @param computeCtx Compute command context (can be nullptr to run compute on graphics)
         * @param computeFence Fence for graphics-compute synchronization
         * @param frameIndex Frame index used for fence value generation
         *
         * When computeCtx is provided, compute passes run asynchronously on the compute queue.
         * Fences are automatically inserted to synchronize resource access between queues.
         * @warning This legacy recording API is not integrated with RenderContext's
         * terminal frame submission. Production rendering must remain GraphicsOnly
         * until queue-batch submission consumes the graph SubmissionPlan.
         */
        void ExecuteAsync(RHICommandContext& graphicsCtx,
                          RHICommandContext* computeCtx,
                          RHIFence* computeFence,
                          uint64 frameIndex = 0);

        enum class AsyncComputeFallbackReason : uint8
        {
            None,
            GraphNotCompiled,
            AsyncPlanningDisabled,
            BackendUnsupported,
            QueueFenceSignalUnsupported,
            QueueFenceWaitUnsupported,
            MissingComputeContext,
            MissingFence,
            NoEligibleComputePasses,
        };

        struct CompileStats
        {
            // Compile validity / capability honesty
            bool compileValid = true;
            bool executionOrderFallbackUsed = false;
            bool asyncComputeSupported = false;
            bool asyncFallbackUsed = false;
            AsyncComputeFallbackReason asyncFallbackReason = AsyncComputeFallbackReason::None;
            uint32 asyncComputeEligiblePasses = 0;
            uint32 asyncComputeScheduledPasses = 0;
            uint32 asyncGraphicsScheduledPasses = 0;
            uint32 asyncFenceSignalCount = 0;
            uint32 asyncFenceWaitCount = 0;
            uint32 asyncCrossQueueDependencyCount = 0;
            uint32 asyncFinalQueueJoinCount = 0;
            uint32 executionQueueMismatchCount = 0;
            uint32 lastExecutedPassCount = 0;
            uint64 lastExecutionCpuDurationNanoseconds = 0;
            bool parallelRecordingEnabled = false;
            bool parallelRecordingUsed = false;
            uint32 lastParallelRecordingLevelCount = 0;
            uint32 lastParallelRecordingBatchCount = 0;
            bool memoryAliasingEnabled = false;
            bool memoryAliasingUnsupportedRequested = false;
            bool explicitAliasingBarriersSupported = false;

            // Pass statistics
            uint32 totalPasses = 0;
            uint32 culledPasses = 0;

            // Validation statistics
            uint32 validationWarningCount = 0;
            uint32 validationErrorCount = 0;
            uint32 emptyPassUsageCount = 0;
            uint32 invalidResourceUsageCount = 0;
            uint32 incompatibleStateUsageCount = 0;
            uint32 shaderStageMismatchUsageCount = 0;
            uint32 readBeforeWriteHazardCount = 0;
            uint32 uninitializedExportCount = 0;
            uint32 accessSnapshotMismatchCount = 0;
            uint32 compatibilityStateProjectionCount = 0;

            // Barrier statistics
            uint32 barrierCount = 0;
            uint32 textureBarrierCount = 0;
            uint32 bufferBarrierCount = 0;
            uint32 mergedBarrierCount = 0;
            uint32 mergedTextureBarrierCount = 0;
            uint32 mergedBufferBarrierCount = 0;
            uint32 crossPassMergedBarrierCount = 0;
            
            // Memory aliasing statistics
            uint32 totalTransientTextures = 0;
            uint32 totalTransientBuffers = 0;
            uint32 aliasedTextureCount = 0;
            uint32 aliasedBufferCount = 0;
            uint64 memoryWithoutAliasing = 0;  // Total memory if no aliasing
            uint64 memoryWithAliasing = 0;      // Actual memory used with aliasing
            uint32 transientHeapCount = 0;
            
            // Memory savings percentage (0-100)
            float GetMemorySavingsPercent() const {
                if (memoryWithoutAliasing == 0) return 0.0f;
                return 100.0f * (1.0f - static_cast<float>(memoryWithAliasing) / 
                    static_cast<float>(memoryWithoutAliasing));
            }
        };

        enum class DiagnosticResourceType : uint8
        {
            Texture,
            Buffer,
        };

        enum class DiagnosticExecutionQueue : uint8
        {
            Unknown,
            Graphics,
            Compute,
            Copy,
        };

        enum class DiagnosticSyncReason : uint8
        {
            CrossQueueDependency,
            FinalQueueJoin,
        };

        enum class DiagnosticAccessType : uint8
        {
            Read,
            Write,
            ReadWrite,
        };

        struct ResourceUsageDiagnostic
        {
            DiagnosticResourceType type = DiagnosticResourceType::Texture;
            DiagnosticAccessType access = DiagnosticAccessType::Read;
            uint32 resourceIndex = RVX_INVALID_INDEX;
            RHIResourceState desiredState = RHIResourceState::Common;
            RHIAccessSnapshot desiredAccess;
            RHIShaderStage stages = RHIShaderStage::None;
            bool hasSubresourceRange = false;
            RHISubresourceRange subresourceRange = RHISubresourceRange::All();
            bool hasRange = false;
            uint64 offset = 0;
            uint64 size = RVX_WHOLE_SIZE;
        };

        struct PassDiagnostic
        {
            uint32 index = RVX_INVALID_INDEX;
            std::string name;
            RenderGraphPassType type = RenderGraphPassType::Graphics;
            bool culled = false;
            DiagnosticExecutionQueue plannedExecutionQueue =
                DiagnosticExecutionQueue::Graphics;
            bool executedLastRun = false;
            DiagnosticExecutionQueue executionQueue = DiagnosticExecutionQueue::Unknown;
            uint32 executionSerial = RVX_INVALID_INDEX;
            uint64 cpuDurationNanoseconds = 0;
            uint32 textureBarrierCount = 0;
            uint32 bufferBarrierCount = 0;
            uint32 aliasingBarrierCount = 0;
            std::vector<uint32> dependencies;
            std::vector<uint32> dependents;
            std::vector<ResourceUsageDiagnostic> usages;
        };

        struct ResourceDiagnostic
        {
            DiagnosticResourceType type = DiagnosticResourceType::Texture;
            uint32 index = RVX_INVALID_INDEX;
            std::string name;
            bool imported = false;
            bool pooled = false;
            bool used = false;
            uint32 firstUsePass = RVX_INVALID_INDEX;
            uint32 lastUsePass = RVX_INVALID_INDEX;
            uint64 estimatedMemoryBytes = 0;
            bool aliased = false;
            uint32 aliasHeapIndex = RVX_INVALID_INDEX;
            uint64 aliasHeapOffset = 0;
            RHIResourceState initialState = RHIResourceState::Undefined;
            RHIResourceState currentState = RHIResourceState::Undefined;
            RHIAccessSnapshot initialAccess;
            RHIAccessSnapshot currentAccess;
            bool hasExportState = false;
            RHIResourceState exportState = RHIResourceState::Undefined;
            bool hasExportAccess = false;
            RHIAccessSnapshot exportAccess;

            // Texture fields
            uint32 width = 0;
            uint32 height = 0;
            uint32 depth = 0;
            uint32 mipLevels = 0;
            uint32 arraySize = 0;
            RHIFormat format = RHIFormat::Unknown;

            // Buffer fields
            uint64 bufferSize = 0;
            uint32 stride = 0;
        };

        struct QueueBatchDiagnostic
        {
            uint32 batchIndex = RVX_INVALID_INDEX;
            DiagnosticExecutionQueue queue = DiagnosticExecutionQueue::Unknown;
            uint32 firstExecutionSerial = RVX_INVALID_INDEX;
            uint32 lastExecutionSerial = RVX_INVALID_INDEX;
            uint64 cpuDurationNanoseconds = 0;
            std::vector<uint32> passIndices;
        };

        struct PlannedQueueBatchDiagnostic
        {
            uint32 batchIndex = RVX_INVALID_INDEX;
            uint32 dependencyLevel = 0;
            DiagnosticExecutionQueue queue = DiagnosticExecutionQueue::Unknown;
            std::vector<uint32> passIndices;
            std::vector<uint32> prerequisiteBatchIndices;
            std::vector<uint32> prerequisiteSyncIndices;
            bool syntheticInitialRelease = false;
            bool syntheticTerminal = false;
        };

        struct PlannedQueueSyncDiagnostic
        {
            uint32 syncIndex = RVX_INVALID_INDEX;
            uint32 sourceBatchIndex = RVX_INVALID_INDEX;
            uint32 targetBatchIndex = RVX_INVALID_INDEX;
            DiagnosticExecutionQueue sourceQueue = DiagnosticExecutionQueue::Unknown;
            DiagnosticExecutionQueue targetQueue = DiagnosticExecutionQueue::Unknown;
            DiagnosticSyncReason reason = DiagnosticSyncReason::CrossQueueDependency;
            uint32 sourcePassIndex = RVX_INVALID_INDEX;
            uint32 targetPassIndex = RVX_INVALID_INDEX;
            bool coveredByActualSync = false;
            uint32 actualSyncIndex = RVX_INVALID_INDEX;
        };

        struct SubmissionPlan
        {
            std::vector<PlannedQueueBatchDiagnostic> queueBatches;
            std::vector<PlannedQueueSyncDiagnostic> queueSyncs;
            uint32 queueBatchCount = 0;
            uint32 dependencyLevelCount = 0;
            uint32 asyncOverlapCandidateLevelCount = 0;
            uint32 computeBatchCount = 0;
            uint32 copyBatchCount = 0;
            uint32 queueSyncCount = 0;
            uint32 crossQueueSyncCount = 0;
            uint32 terminalGraphicsBatchIndex = RVX_INVALID_INDEX;
        };

        struct QueueSyncDiagnostic
        {
            uint32 syncIndex = RVX_INVALID_INDEX;
            DiagnosticExecutionQueue sourceQueue = DiagnosticExecutionQueue::Unknown;
            DiagnosticExecutionQueue targetQueue = DiagnosticExecutionQueue::Unknown;
            DiagnosticSyncReason reason = DiagnosticSyncReason::CrossQueueDependency;
            uint64 fenceValue = 0;
            uint32 sourcePassIndex = RVX_INVALID_INDEX;
            uint32 targetPassIndex = RVX_INVALID_INDEX;
            bool coversPlannedSync = false;
            uint32 plannedSyncIndex = RVX_INVALID_INDEX;
        };

        struct Diagnostics
        {
            const char* schemaId = RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_ID;
            uint32 schemaVersion = RVX_RENDER_GRAPH_DIAGNOSTICS_SCHEMA_VERSION;
            CompileStats compileStats;
            std::vector<PassDiagnostic> passes;
            std::vector<ResourceDiagnostic> resources;
            std::vector<QueueBatchDiagnostic> queueBatches;
            std::vector<PlannedQueueBatchDiagnostic> plannedQueueBatches;
            std::vector<PlannedQueueSyncDiagnostic> plannedQueueSyncs;
            std::vector<QueueSyncDiagnostic> queueSyncs;
            std::vector<uint32> executionOrder;
            uint32 plannedQueueBatchCount = 0;
            uint32 plannedDependencyLevelCount = 0;
            uint32 plannedAsyncOverlapCandidateLevelCount = 0;
            uint32 plannedComputeBatchCount = 0;
            uint32 plannedCopyBatchCount = 0;
            uint32 plannedQueueSyncCount = 0;
            uint32 plannedCrossQueueSyncCount = 0;
            uint32 plannedTerminalGraphicsBatchIndex = RVX_INVALID_INDEX;
            uint32 plannedQueueSyncCoveredCount = 0;
            uint32 plannedQueueSyncUncoveredCount = 0;
            uint32 actualQueueBatchCount = 0;
            uint32 actualQueueSwitchCount = 0;
            uint32 actualQueueSyncCount = 0;
            uint32 actualCrossQueueSyncCount = 0;
            uint32 actualMatchedPlannedSyncCount = 0;
            uint32 actualUnplannedQueueSyncCount = 0;
            uint32 actualConservativeFinalJoinCount = 0;
            uint64 estimatedTransientMemoryBytes = 0;
            uint64 estimatedUsedTransientMemoryBytes = 0;
            uint64 estimatedImportedMemoryBytes = 0;
        };

        const CompileStats& GetCompileStats() const;
        const std::vector<std::string>& GetCompileDiagnostics() const;
        SubmissionPlan GetSubmissionPlan() const;
        Diagnostics GetDiagnostics() const;
        std::string ExportDiagnosticsText() const;
        std::string ExportDiagnosticsJson() const;
        bool SaveDiagnosticsJson(const char* filename) const;

        // Memory aliasing control
        void SetMemoryAliasingEnabled(bool enabled);
        bool IsMemoryAliasingEnabled() const;

        /** @brief Retain graph-owned objects in the current submission batch. */
        [[nodiscard]] bool RetainSubmissionResources(
            RenderSubmissionResourceBatch& batch) const;

        // Debug/Visualization
        std::string ExportGraphviz() const;
        bool SaveGraphviz(const char* filename) const;

        // Clear for next frame
        void Clear();

    private:
        void AddPassInternal(
            const char* name,
            RenderGraphPassType type,
            std::function<void(RenderGraphBuilder&)> setup,
            std::function<void(RHICommandContext&)> execute);

        class Impl;
        std::unique_ptr<Impl> m_impl;
    };

    template<typename Data>
    void RenderGraph::AddPass(
        const char* name,
        RenderGraphPassType type,
        std::function<void(RenderGraphBuilder&, Data&)> setup,
        std::function<void(const Data&, RHICommandContext&)> execute)
    {
        auto data = std::make_shared<Data>();
        AddPassInternal(
            name,
            type,
            [data, setup](RenderGraphBuilder& builder)
            {
                setup(builder, *data);
            },
            [data, execute](RHICommandContext& ctx)
            {
                execute(*data, ctx);
            });
    }

} // namespace RVX
