/**
 * @file GPUCulling.h
 * @brief GPU-driven visibility culling
 *
 * Implements GPU-based frustum and occlusion culling using compute shaders.
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "Render/Material/MaterialClassification.h"
#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Submission/RasterInstanceStream.h"
#include "RenderContracts/RenderIdentity.h"
#include "RHI/RHI.h"
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class PipelineCache;
    class RHICommandContext;
    class RenderRetirementQueue;
    class RenderScene;
    class RenderSubmissionResourceBatch;
    class GPUCullingRecordedState;
    struct GPUCompletionToken;
    struct GPUSceneResidentGraphLease;
    struct RenderDrawItem;
    struct RenderDrawPacket;
    struct RenderVisibilityCandidate;

    /**
     * @brief Indexed draw arguments associated with a render draw item
     */
    struct GPUIndexedDrawDesc
    {
        uint32 indexCount = 0;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
    };

    /**
     * @brief Fixed culling constant-buffer ABI shared by both compute paths.
     *
     * The uint count blocks deliberately avoid float conversion for resource
     * counts and preserve exact GPU-scene lease capacities.
     */
    struct alignas(16) GPUCullingConstants
    {
        Mat4 viewProj;
        Vec4 frustumPlanes[6];
        Vec4 cameraPosition;
        Vec4 params;
        uint32 counts[4];
        uint32 gpuSceneTableCounts0[4];
        uint32 gpuSceneTableCounts1[4];
    };

    static_assert(sizeof(GPUCullingConstants) == 240,
                  "GPUCullingConstants must match GPUCulling.hlsl and GPUSceneCulling.hlsl");
    static_assert(offsetof(GPUCullingConstants, viewProj) == 0);
    static_assert(offsetof(GPUCullingConstants, frustumPlanes) == 64);
    static_assert(offsetof(GPUCullingConstants, cameraPosition) == 160);
    static_assert(offsetof(GPUCullingConstants, params) == 176);
    static_assert(offsetof(GPUCullingConstants, counts) == 192);
    static_assert(offsetof(GPUCullingConstants, gpuSceneTableCounts0) == 208);
    static_assert(offsetof(GPUCullingConstants, gpuSceneTableCounts1) == 224);

    /** @brief Fixed GPU-scene culling candidate ABI; no native bools or uint64s. */
    struct GPUSceneCullingCandidate
    {
        uint32 primitiveSlot = 0;
        uint32 primitiveGeneration = 0;
        uint32 drawSlot = 0;
        uint32 drawGeneration = 0;
        uint32 objectIdLow = 0;
        uint32 objectIdHigh = 0;
        uint32 requiredPassMask = 0;
        uint32 drawGroupIndex = RVX_INVALID_INDEX;
        uint32 drawGroupVisibleOffset = 0;
        uint32 rasterInstanceIndex = RVX_INVALID_INDEX;
        uint32 materialParameterSlot = RVX_INVALID_INDEX;
        uint32 padding0 = 0;

        constexpr bool operator==(const GPUSceneCullingCandidate&) const = default;
    };

    static_assert(sizeof(GPUSceneCullingCandidate) == 48,
                  "GPUSceneCullingCandidate must match GPUSceneCulling.hlsli");
    static_assert(offsetof(GPUSceneCullingCandidate, primitiveSlot) == 0);
    static_assert(offsetof(GPUSceneCullingCandidate, primitiveGeneration) == 4);
    static_assert(offsetof(GPUSceneCullingCandidate, drawSlot) == 8);
    static_assert(offsetof(GPUSceneCullingCandidate, drawGeneration) == 12);
    static_assert(offsetof(GPUSceneCullingCandidate, objectIdLow) == 16);
    static_assert(offsetof(GPUSceneCullingCandidate, objectIdHigh) == 20);
    static_assert(offsetof(GPUSceneCullingCandidate, requiredPassMask) == 24);
    static_assert(offsetof(GPUSceneCullingCandidate, drawGroupIndex) == 28);
    static_assert(offsetof(GPUSceneCullingCandidate, drawGroupVisibleOffset) == 32);
    static_assert(offsetof(GPUSceneCullingCandidate, rasterInstanceIndex) == 36);
    static_assert(offsetof(GPUSceneCullingCandidate, materialParameterSlot) == 40);
    static_assert(offsetof(GPUSceneCullingCandidate, padding0) == 44);

    constexpr uint32 RVX_GPU_SCENE_CULLING_TABLE_COUNT = 6;

    /**
     * @brief Contiguous indirect command range for a mesh-compatible draw group
     */
    struct GPUCullingDrawGroup
    {
        RenderResourceHandle mesh;
        RenderResourceHandle material;
        uint64 meshId = 0;
        uint64 materialId = 0;
        MaterialPipelineVariant pipelineVariant = MaterialPipelineVariant::Opaque;
        RenderDrawGroupKey batchKey{};
        uint32 commandOffset = 0;
        uint32 visibleInstanceOffset = 0;
        uint32 countBufferOffset = 0;
        uint32 maxDrawCount = 0;
        uint32 visibleDrawCount = 0;
        uint32 indexCount = 0;
        uint32 firstIndex = 0;
        int32 vertexOffset = 0;
    };

    /**
     * @brief GPU culling configuration
     */
    struct GPUCullingConfig
    {
        uint32 maxInstances = 65536;
        bool enableFrustumCulling = true;
        bool enableOcclusionCulling = false;
        bool enableDistanceCulling = true;
        float maxDrawDistance = 1000.0f;
        bool twoPhaseOcclusion = false;  // Re-test with HiZ from current frame
    };

    enum class GPUCullingExecutionMode : uint8
    {
        CpuFallback,
        GpuCompute,
    };

    enum class GPUCullingFallbackReason : uint8
    {
        None,
        DeviceMissing,
        ComputePipelineUnsupported,
        DescriptorSetsUnsupported,
        IndirectDrawCountUnsupported,
        ShaderBackendUnsupported,
        ShaderFileMissing,
        DescriptorSetLayoutCreationFailed,
        PipelineLayoutCreationFailed,
        ShaderCompilationFailed,
        PipelineCreationFailed,
        DescriptorSetCreationFailed,
        PipelineResourcesUnavailable,
        IndirectDrawFirstInstanceUnsupported,
    };

    struct GPUCullingExecutionDecision
    {
        GPUCullingExecutionMode mode = GPUCullingExecutionMode::CpuFallback;
        GPUCullingFallbackReason fallbackReason = GPUCullingFallbackReason::DeviceMissing;
        bool gpuCapable = false;
        bool pipelineReady = false;
    };

    /** @brief Persistent access ownership for GPU-culling buffers across frames. */
    struct GPUCullingAccessSnapshots
    {
        RHIBufferAccessSnapshot constants;
        RHIBufferAccessSnapshot instances;
        RHIBufferAccessSnapshot gpuSceneCandidates;
        RHIBufferAccessSnapshot instanceIndices;
        RHIBufferAccessSnapshot visibility;
        RHIBufferAccessSnapshot visibleInstances;
        RHIBufferAccessSnapshot indirectDraws;
        RHIBufferAccessSnapshot drawCount;

        bool operator==(const GPUCullingAccessSnapshots&) const = default;
    };

    /**
     * @brief Renderer-private, sealed raster view of the GPU-scene resources.
     *
     * The refs intentionally keep exactly the candidate/primitive/transform
     * buffers alive after the uploader's caller drops its lease.  No public RHI
     * contract is introduced; a later render-pass owner must still import and
     * retain these resources through the established RenderGraph path.
     */
    struct GPUSceneRasterResourceSnapshot
    {
        [[nodiscard]] bool IsValid() const noexcept
        {
            return m_candidates && m_primitives && m_transforms &&
                   m_candidateCount != 0 && m_primitiveCapacity != 0 &&
                   m_transformCapacity != 0 &&
                   m_candidateCapacity >= m_candidateCount &&
                   m_leaseVersion != 0 &&
                   m_leaseVersion == m_exactLeaseVersion;
        }

        [[nodiscard]] RHIBuffer* GetCandidates() const noexcept
        {
            return m_candidates.Get();
        }
        [[nodiscard]] RHIBuffer* GetPrimitives() const noexcept
        {
            return m_primitives.Get();
        }
        [[nodiscard]] RHIBuffer* GetTransforms() const noexcept
        {
            return m_transforms.Get();
        }
        [[nodiscard]] uint32 GetCandidateCount() const noexcept
        {
            return m_candidateCount;
        }
        [[nodiscard]] uint32 GetCandidateCapacity() const noexcept
        {
            return m_candidateCapacity;
        }
        [[nodiscard]] uint32 GetPrimitiveCapacity() const noexcept
        {
            return m_primitiveCapacity;
        }
        [[nodiscard]] uint32 GetTransformCapacity() const noexcept
        {
            return m_transformCapacity;
        }
        [[nodiscard]] uint64 GetLeaseVersion() const noexcept
        {
            return m_leaseVersion;
        }

    private:
        friend class GPUCulling;
        friend class PipelineCache;

        RHIBufferRef m_candidates;
        RHIBufferRef m_primitives;
        RHIBufferRef m_transforms;
        uint32 m_candidateCount = 0;
        uint32 m_candidateCapacity = 0;
        uint32 m_primitiveCapacity = 0;
        uint32 m_transformCapacity = 0;
        uint64 m_leaseVersion = 0;
        // A caller can copy this sealed value but cannot manufacture or alter
        // one. PipelineCache requires it to match the exposed lease version.
        uint64 m_exactLeaseVersion = 0;
    };

    /** @brief Backend-neutral culling output for one indexed indirect submission. */
    struct GPUCullingIndexedIndirectSubmission
    {
        RHIIndexedIndirectExecutionDesc execution;
        const RHICapabilities* capabilities = nullptr;
    };

    /** @brief Immutable graph-recording identity for one sealed culling slice. */
    struct GPUCullingRecordingIdentity
    {
        uint64 graphIdentity = 0;
        uint64 graphRecordingGeneration = 0;
        uint64 frameSequence = 0;
        uint32 viewOrdinal = 0;
        uint64 recordEpoch = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return graphIdentity != 0 && graphRecordingGeneration != 0 &&
                   frameSequence != 0 && recordEpoch != 0;
        }

        bool operator==(const GPUCullingRecordingIdentity&) const = default;
    };

    /**
     * @brief GPU-driven culling system
     *
     * Performs visibility determination entirely on the GPU:
     * 1. Upload instance data to GPU
     * 2. Run culling compute shader
     * 3. Generate indirect draw commands
     * 4. Execute indirect draws
     *
     * Benefits:
     * - Minimal CPU overhead
     * - Scales to millions of instances
     * - GPU-parallel frustum culling
     * - Optional occlusion culling with HiZ
     */
    class GPUCulling
    {
    public:
        GPUCulling() = default;
        ~GPUCulling();

        // Non-copyable
        GPUCulling(const GPUCulling&) = delete;
        GPUCulling& operator=(const GPUCulling&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        void Initialize(IRHIDevice* device,
                        const GPUCullingConfig& config = {},
                        uint32 frameSlotCount = 1);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        /** @brief Transfer replaced owner resources using the prior-submit snapshot. */
        void RetireOwnerSnapshots(const GPUCompletionToken& completion,
                                  RenderRetirementQueue& retirement);

        /** @brief Move per-recording staging buffers into the current submission batch. */
        [[nodiscard]] bool RetainSubmissionResources(
            RenderSubmissionResourceBatch& batch);

        // =========================================================================
        // Configuration
        // =========================================================================

        const GPUCullingConfig& GetConfig() const { return m_config; }
        void SetConfig(const GPUCullingConfig& config);
        /** @brief Select the RenderContext frame slot whose completion was awaited. */
        [[nodiscard]] bool SetFrameSlot(uint32 frameSlot);
        [[nodiscard]] uint32 GetFrameSlotCount() const
        {
            return static_cast<uint32>(m_frameInputs.size());
        }
        [[nodiscard]] uint32 GetActiveFrameSlot() const
        {
            return m_activeFrameSlot;
        }
        [[nodiscard]] bool WasOcclusionRequested() const
        {
            return m_occlusionRequested;
        }
        [[nodiscard]] bool IsOcclusionAvailable() const { return false; }

        /**
         * @brief Seal the active frame-slot inputs for one RenderGraph recording.
         *
         * The returned state owns copied CPU inputs and strong references to
         * the selected completion-safe frame-slot resources.  The renderer
         * may mutate other slots while this recording remains immutable, but
         * must not recycle the source slot before its prior submission token
         * completes.
         */
        [[nodiscard]] std::shared_ptr<GPUCullingRecordedState> SealForGraph(
            const GPUCullingRecordingIdentity& identity) const;

        /** @brief Seal immutable culling resources bound to one exact GPU-scene lease. */
        [[nodiscard]] std::shared_ptr<GPUCullingRecordedState>
            SealForGPUSceneGraph(
                const GPUCullingRecordingIdentity& identity,
                const GPUSceneResidentGraphLease& lease);

        /** @brief Publish realized access back to the exact source frame slot. */
        [[nodiscard]] bool CommitFrameSlotAccessSnapshots(
            uint32 frameSlot,
            const GPUCullingAccessSnapshots& snapshots);

        /** @brief Whether the separate GPU-scene descriptor/pipeline path is available. */
        [[nodiscard]] bool IsGPUSceneExecutionReady() const;

        // =========================================================================
        // Instance Management
        // =========================================================================

        /**
         * @brief Begin a new frame of instance collection
         */
        void BeginFrame();

        /**
         * @brief Begin a mesh-compatible draw group for subsequent instances
         * @return Group index, or RVX_INVALID_INDEX when the group cannot be created
         */
        uint32 BeginDrawGroup(uint64 meshId,
                              uint64 materialId = 0,
                              MaterialPipelineVariant pipelineVariant = MaterialPipelineVariant::Opaque,
                              RenderResourceHandle mesh = {},
                              RenderResourceHandle material = {},
                              RenderDrawGroupKey batchKey = {});

        /**
         * @brief End the current draw group
         */
        void EndDrawGroup();

        /**
         * @brief Add an instance to be culled
         * @return Instance index
         */
        uint32 AddInstance(const GPUInstanceData& instance);

        /**
         * @brief Batch add instances
         */
        void AddInstances(const GPUInstanceData* instances, uint32 count);

        /**
         * @brief Add a render draw item as a cullable GPU instance
         * @param scene Render scene containing the draw item object
         * @param drawItem Material-aware draw item to map back after culling
         * @param drawDesc Indexed draw arguments for the item submesh
         * @param sourceIndex Caller-owned draw item index returned by GetVisibleSourceIndices()
         * @return Instance index, or RVX_INVALID_INDEX when the item cannot be represented
         */
        uint32 AddDrawItemInstance(const RenderScene& scene,
                                   const RenderDrawItem& drawItem,
                                   const GPUIndexedDrawDesc& drawDesc,
                                   uint32 sourceIndex);

        /**
         * @brief Add one stable frame/view candidate without ordinal lookup.
         * sourceIndex is the pass-local sourcePacketIndex.
         */
        uint32 AddVisibilityCandidateInstance(
            const RenderScene& scene,
            const RenderVisibilityCandidate& candidate,
            const RenderDrawPacket& packet,
            const GPUIndexedDrawDesc& drawDesc);

        /** @brief Append one exact GPU-scene candidate matching the last raster instance. */
        [[nodiscard]] bool AddGPUSceneCandidate(
            const GPUSceneCullingCandidate& candidate,
            uint64 committedVersion);
        /** @brief Discard incomplete/stale GPU-scene candidates while preserving Tier 1 inputs. */
        void InvalidateGPUSceneCandidates() noexcept;
        /** @brief True only when every Tier 1 instance has a companion at the exact frozen version. */
        [[nodiscard]] bool HasCompleteGPUSceneCandidates(
            uint64 requiredVersion) const noexcept;
        /** @brief True when every Tier 1 instance has a companion at the collected version. */
        [[nodiscard]] bool HasCompleteGPUSceneCandidates() const noexcept;

        /**
         * @brief End instance collection and upload to GPU
         */
        void EndFrame();

        /**
         * @brief Get current instance count
         */
        uint32 GetInstanceCount() const { return m_instanceCount; }

        /**
         * @brief Get mesh-compatible indirect draw groups
         */
        const std::vector<GPUCullingDrawGroup>& GetDrawGroups() const { return m_drawGroups; }

        // =========================================================================
        // Culling
        // =========================================================================

        /**
         * @brief Perform GPU culling
         * @param ctx Command context
         * @param viewMatrix Camera view matrix
         * @param projMatrix Camera projection matrix
         * @param hiZTexture HiZ pyramid for occlusion culling (optional)
         */
        void Cull(RHICommandContext& ctx,
                  const Mat4& viewMatrix,
                  const Mat4& projMatrix,
                  RHITexture* hiZTexture = nullptr);

        /**
         * @brief Dispatch the separately sealed GPU-scene culling pipelines.
         *
         * This path never falls back to CPU or normal Tier 1 culling after a
         * recording-time failure. Callers must choose that fallback before
         * graph recording by sealing the normal path instead.
         */
        [[nodiscard]] bool CullGPUScene(RHICommandContext& ctx,
                                        const Mat4& viewMatrix,
                                        const Mat4& projMatrix);

        /**
         * @brief Perform CPU fallback culling and upload the same output buffers
         *
         * This is used until compute culling/compaction pipelines are available,
         * and by higher-level render paths that need draw-list filtering before
         * command recording.
         */
        void CullCpuFallback(const Mat4& viewMatrix, const Mat4& projMatrix);

        /**
         * @brief Get the indirect draw buffer
         */
        RHIBuffer* GetIndirectBuffer() const { return m_indirectBuffer.Get(); }

        /**
         * @brief Get the instance input buffer
         */
        RHIBuffer* GetInstanceBuffer() const;

        /** @brief GPU-scene candidate input for a sealed GPU-scene compute pass. */
        RHIBuffer* GetGPUSceneCandidateBuffer() const;

        /** @brief Get the exact sealed GPU-scene resources required by raster. */
        [[nodiscard]] GPUSceneRasterResourceSnapshot
            GetGPUSceneRasterResourceSnapshot() const;

        /** @brief Identity per-instance vertex input used to resolve indirect firstInstance. */
        RHIBuffer* GetInstanceIndexBuffer() const { return m_instanceIndexBuffer.Get(); }

        /**
         * @brief Get the culling constants buffer
         */
        RHIBuffer* GetCullingConstantsBuffer() const;

        /**
         * @brief Get visibility flag buffer
         */
        RHIBuffer* GetVisibilityBuffer() const { return m_visibilityBuffer.Get(); }

        /**
         * @brief Get the draw count buffer (for indirect count)
         */
        RHIBuffer* GetDrawCountBuffer() const { return m_drawCountBuffer.Get(); }

        /**
         * @brief Get visible instance buffer
         */
        RHIBuffer* GetVisibleInstanceBuffer() const { return m_visibleInstanceBuffer.Get(); }

        /**
         * @brief Get CPU-visible indices generated by the last cull pass
         */
        const std::vector<uint32>& GetVisibleInstanceIndices() const { return m_visibleInstanceIndices; }

        /**
         * @brief Get caller-owned source indices for visible draw-item instances
         */
        const std::vector<uint32>& GetVisibleSourceIndices() const { return m_visibleSourceIndices; }

        /**
         * @brief Get CPU-visible indirect commands generated by the last cull pass
         */
        const std::vector<IndirectDrawIndexedCommand>& GetIndirectCommands() const { return m_indirectCommands; }

        /**
         * @brief Get draw count generated by the last cull pass
         */
        uint32 GetDrawCount() const { return m_drawCount; }

        /**
         * @brief Whether the last cull used the CPU fallback path
         */
        bool WasCpuFallbackUsedLastCull() const { return m_usedCpuFallbackLastCull; }

        /**
         * @brief Whether the last cull recorded GPU compute culling/compaction work
         */
        bool WasGpuExecutionUsedLastCull() const { return m_usedGpuExecutionLastCull; }

        /**
         * @brief Current GPU compute execution decision and fallback reason
         */
        GPUCullingExecutionDecision GetExecutionDecision() const;

        const GPUCullingAccessSnapshots& GetAccessSnapshots() const;

        /** @brief Commit RenderGraph's realized final buffer accesses. */
        void CommitAccessSnapshots(const GPUCullingAccessSnapshots& snapshots);

        /**
         * @brief Whether the current device/capability/pipeline state can execute GPU culling
         */
        bool IsGpuExecutionReady() const { return GetExecutionDecision().mode == GPUCullingExecutionMode::GpuCompute; }

        /**
         * @brief Fallback reason recorded by the last Cull() call
         */
        GPUCullingFallbackReason GetLastFallbackReason() const { return m_lastFallbackReason; }

        /** @brief Build a backend-neutral submission for the whole compatible range. */
        GPUCullingIndexedIndirectSubmission BuildIndexedIndirectSubmission(
            uint32 maxDrawCount = 0) const;

        /** @brief Build a backend-neutral submission for one mesh-compatible group. */
        GPUCullingIndexedIndirectSubmission BuildIndexedIndirectGroupSubmission(
            uint32 groupIndex) const;

        // =========================================================================
        // Statistics
        // =========================================================================

        struct Statistics
        {
            uint32 totalInstances = 0;
            uint32 visibleInstances = 0;
            uint32 frustumCulled = 0;
            uint32 occlusionCulled = 0;
            uint32 distanceCulled = 0;
            float cullingTimeMs = 0.0f;
        };

        /**
         * @brief Get culling statistics (may require GPU readback)
         */
        Statistics GetStatistics() const { return m_stats; }

        /**
         * @brief Enable statistics collection (has performance cost)
         */
        void SetStatisticsEnabled(bool enabled) { m_statsEnabled = enabled; }

    private:
        friend class GPUCullingRecordedState;
        struct GPUCullingFrameInputs
        {
            RHIBufferRef instanceBuffer;
            RHIBufferRef gpuSceneCandidateBuffer;
            RHIBufferRef constantsBuffer;
            RHIBufferRef instanceIndexBuffer;
            RHIBufferRef visibilityBuffer;
            RHIBufferRef visibleInstanceBuffer;
            RHIBufferRef indirectBuffer;
            RHIBufferRef drawCountBuffer;
            RHIBufferRef statsBuffer;
            RHIDescriptorSetRef descriptorSet;
            RHIDescriptorSetRef gpuSceneDescriptorSet;
            RHIBufferAccessSnapshot instanceAccess;
            RHIBufferAccessSnapshot gpuSceneCandidateAccess;
            RHIBufferAccessSnapshot constantsAccess;
            GPUCullingAccessSnapshots accessSnapshots;
            std::array<RHIBufferRef, RVX_GPU_SCENE_CULLING_TABLE_COUNT>
                gpuSceneTableBuffers;
            std::array<uint32, RVX_GPU_SCENE_CULLING_TABLE_COUNT>
                gpuSceneTableCapacities{};
            uint64 gpuSceneLeaseVersion = 0;
        };

        void CreateResources();
        void CreatePipelineResources();
        void QueueFrameInputRetirements();
        [[nodiscard]] bool RetainSealedSubmissionResources(
            RenderSubmissionResourceBatch& batch);
        [[nodiscard]] GPUCullingFrameInputs* GetActiveFrameInputs();
        [[nodiscard]] const GPUCullingFrameInputs* GetActiveFrameInputs() const;
        void RefreshActiveInputAccessSnapshots();
        GPUCullingExecutionDecision EvaluateGpuExecution(bool requirePipelineResources) const;
        bool SupportsGpuExecution() const;
        uint32 EnsureDefaultDrawGroup();
        void UploadInstances();
        void UploadGPUSceneCandidates();
        [[nodiscard]] bool ConfigureGPUSceneRecording(
            const GPUSceneResidentGraphLease& lease);
        void ExtractFrustumPlanes(const Mat4& viewProj, Vec4* planes);
        void BuildCpuCullResults(const Mat4& viewMatrix, const Vec4* frustumPlanes);
        void UploadCullOutputs(RHICommandContext* ctx = nullptr);
        bool UploadBufferData(RHIBuffer* buffer,
                              const void* data,
                              uint64 size,
                              RHICommandContext* ctx);

        IRHIDevice* m_device = nullptr;
        GPUCullingConfig m_config;
        bool m_occlusionRequested = false;
        // CPU-side instance data
        std::vector<GPUInstanceData> m_instances;
        std::vector<GPUSceneCullingCandidate> m_gpuSceneCandidates;
        uint64 m_gpuSceneCandidateVersion = 0;
        std::vector<uint32> m_visibleInstanceIndices;
        std::vector<uint32> m_rasterVisibleInstanceIndices;
        std::vector<uint32> m_visibleSourceIndices;
        std::vector<IndirectDrawIndexedCommand> m_indirectCommands;
        std::vector<uint32> m_groupDrawCounts;
        std::vector<GPUCullingDrawGroup> m_drawGroups;
        uint32 m_instanceCount = 0;
        uint32 m_drawCount = 0;
        uint32 m_activeDrawGroupIndex = RVX_INVALID_INDEX;
        bool m_usedCpuFallbackLastCull = false;
        bool m_usedGpuExecutionLastCull = false;
        GPUCullingFallbackReason m_lastFallbackReason = GPUCullingFallbackReason::None;
        GPUCullingFallbackReason m_pipelineFallbackReason = GPUCullingFallbackReason::None;
        std::vector<RHIBufferRef> m_transientUploadBuffers;
        std::vector<Ref<RefCounted>> m_pendingOwnerRetirements;

        // Active-slot aliases. GPU ownership lives in GPUCullingFrameInputs;
        // these refs are rebound only after RenderContext waits that slot.
        RHIBufferRef m_instanceIndexBuffer;      // Identity uint index fetched through IA firstInstance
        RHIBufferRef m_visibilityBuffer;         // Per-instance visibility flags
        RHIBufferRef m_visibleInstanceBuffer;    // Visible instance indices
        RHIBufferRef m_indirectBuffer;           // Indirect draw commands
        RHIBufferRef m_drawCountBuffer;          // Number of draws
        std::vector<GPUCullingFrameInputs> m_frameInputs;
        uint32 m_activeFrameSlot = 0;
        GPUCullingAccessSnapshots m_accessSnapshots;

        // Pipelines
        RHIShaderRef m_frustumCullShader;
        RHIShaderRef m_compactShader;
        RHIShaderRef m_finalizeShader;
        RHIDescriptorSetLayoutRef m_cullingDescriptorSetLayout;
        RHIPipelineLayoutRef m_cullingPipelineLayout;
        RHIPipelineRef m_frustumCullPipeline;
        RHIPipelineRef m_occlusionCullPipeline;
        RHIPipelineRef m_compactPipeline;
        RHIPipelineRef m_finalizePipeline;
        RHIShaderRef m_gpuSceneFrustumCullShader;
        RHIShaderRef m_gpuSceneCompactShader;
        RHIShaderRef m_gpuSceneFinalizeShader;
        RHIDescriptorSetLayoutRef m_gpuSceneDescriptorSetLayout;
        RHIPipelineLayoutRef m_gpuScenePipelineLayout;
        RHIPipelineRef m_gpuSceneFrustumCullPipeline;
        RHIPipelineRef m_gpuSceneCompactPipeline;
        RHIPipelineRef m_gpuSceneFinalizePipeline;
        RHIDescriptorSetRef m_gpuSceneDescriptorSet;
        std::array<RHIBufferRef, RVX_GPU_SCENE_CULLING_TABLE_COUNT>
            m_gpuSceneTableBuffers;
        std::array<uint32, RVX_GPU_SCENE_CULLING_TABLE_COUNT>
            m_gpuSceneTableCapacities{};
        uint64 m_gpuSceneLeaseVersion = 0;
        bool m_gpuSceneEnabled = false;

        // Statistics
        Statistics m_stats;
        bool m_statsEnabled = false;
        RHIBufferRef m_statsBuffer;
    };

    /** @brief Strongly-owned GPU culling state captured for one graph recording. */
    class GPUCullingRecordedState
    {
    public:
        [[nodiscard]] bool IsValid() const noexcept { return m_identity.IsValid(); }
        [[nodiscard]] bool Matches(const GPUCullingRecordingIdentity& identity) const noexcept
        {
            return m_identity == identity;
        }
        [[nodiscard]] uint32 GetSourceFrameSlot() const noexcept
        {
            return m_sourceFrameSlot;
        }
        [[nodiscard]] const GPUCulling& GetCulling() const noexcept { return m_culling; }
        [[nodiscard]] GPUCulling& GetCulling() noexcept { return m_culling; }
        [[nodiscard]] const GPUCullingAccessSnapshots& GetAccessSnapshots() const
        {
            return m_culling.GetAccessSnapshots();
        }
        [[nodiscard]] GPUSceneRasterResourceSnapshot
            GetGPUSceneRasterResourceSnapshot() const
        {
            return m_culling.GetGPUSceneRasterResourceSnapshot();
        }
        void CommitAccessSnapshots(const GPUCullingAccessSnapshots& snapshots)
        {
            m_culling.CommitAccessSnapshots(snapshots);
        }
        void Cull(RHICommandContext& ctx,
                  const Mat4& viewMatrix,
                  const Mat4& projectionMatrix,
                  RHITexture* hiZTexture = nullptr)
        {
            m_culling.Cull(ctx, viewMatrix, projectionMatrix, hiZTexture);
        }
        [[nodiscard]] bool CullGPUScene(RHICommandContext& ctx,
                                        const Mat4& viewMatrix,
                                        const Mat4& projectionMatrix)
        {
            return m_culling.CullGPUScene(ctx, viewMatrix, projectionMatrix);
        }
        [[nodiscard]] bool RetainSubmissionResources(
            RenderSubmissionResourceBatch& batch)
        {
            return m_culling.RetainSealedSubmissionResources(batch);
        }

    private:
        friend class GPUCulling;

        GPUCullingRecordingIdentity m_identity{};
        uint32 m_sourceFrameSlot = 0;
        GPUCulling m_culling{};
    };

    /**
     * @brief Meshlet-based rendering for nanite-style geometry
     */
    struct Meshlet
    {
        uint32 vertexOffset;
        uint32 triangleOffset;
        uint32 vertexCount;
        uint32 triangleCount;
        Vec4 boundingSphere;
        Vec4 coneApex;      // For backface cone culling
        Vec4 coneAxis;      // xyz = axis, w = cos(cone angle)
    };

    /**
     * @brief Meshlet renderer for GPU-driven geometry processing
     */
    class MeshletRenderer
    {
    public:
        MeshletRenderer() = default;
        ~MeshletRenderer();

        void Initialize(IRHIDevice* device);
        void Shutdown();

        /**
         * @brief Generate meshlets from a mesh
         * @param vertices Vertex positions
         * @param vertexCount Number of vertices
         * @param indices Triangle indices
         * @param indexCount Number of indices
         * @param maxVertices Maximum vertices per meshlet (typically 64)
         * @param maxTriangles Maximum triangles per meshlet (typically 124)
         * @param outMeshlets Output meshlet array
         */
        static void GenerateMeshlets(
            const Vec3* vertices,
            uint32 vertexCount,
            const uint32* indices,
            uint32 indexCount,
            uint32 maxVertices,
            uint32 maxTriangles,
            std::vector<Meshlet>& outMeshlets);

        /**
         * @brief Render meshlets with GPU culling
         */
        void Render(RHICommandContext& ctx,
                    const Mat4& viewMatrix,
                    const Mat4& projMatrix);

    private:
        IRHIDevice* m_device = nullptr;

        RHIBufferRef m_meshletBuffer;
        RHIBufferRef m_vertexBuffer;
        RHIBufferRef m_indexBuffer;
        RHIBufferRef m_visibleMeshletBuffer;

        RHIPipelineRef m_meshletCullPipeline;
        RHIPipelineRef m_meshletDrawPipeline;
    };

} // namespace RVX
