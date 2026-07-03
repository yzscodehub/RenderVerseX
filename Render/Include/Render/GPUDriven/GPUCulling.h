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
#include "RenderContracts/RenderResource.h"
#include "RHI/RHI.h"
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class RenderScene;
    struct RenderDrawItem;

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
     * @brief GPU instance data for culling
     */
    struct GPUInstanceData
    {
        Mat4 worldMatrix;
        Mat4 normalMatrix;
        Vec4 boundingSphere;  // xyz = center, w = radius
        Vec4 aabbMin;         // xyz = min, w = unused
        Vec4 aabbMax;         // xyz = max, w = unused
        uint32 meshId;
        uint32 materialId;
        uint32 indexCount;
        uint32 firstIndex;
        int32 vertexOffset;
        uint32 sourceIndex = RVX_INVALID_INDEX;
        uint32 drawGroupIndex;
        uint32 drawGroupCommandOffset;
    };

    /**
     * @brief Contiguous indirect command range for a mesh-compatible draw group
     */
    struct GPUCullingDrawGroup
    {
        uint64 meshId = 0;
        uint64 materialId = 0;
        const IRenderMaterialSource* materialResource = nullptr;
        MaterialPipelineVariant pipelineVariant = MaterialPipelineVariant::Opaque;
        uint32 commandOffset = 0;
        uint32 countBufferOffset = 0;
        uint32 maxDrawCount = 0;
        uint32 visibleDrawCount = 0;
    };

    /**
     * @brief GPU culling configuration
     */
    struct GPUCullingConfig
    {
        uint32 maxInstances = 65536;
        bool enableFrustumCulling = true;
        bool enableOcclusionCulling = true;
        bool enableDistanceCulling = true;
        float maxDrawDistance = 1000.0f;
        bool twoPhaseOcclusion = true;  // Re-test with HiZ from current frame
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
    };

    struct GPUCullingExecutionDecision
    {
        GPUCullingExecutionMode mode = GPUCullingExecutionMode::CpuFallback;
        GPUCullingFallbackReason fallbackReason = GPUCullingFallbackReason::DeviceMissing;
        bool gpuCapable = false;
        bool pipelineReady = false;
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

        void Initialize(IRHIDevice* device, const GPUCullingConfig& config = {});
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Configuration
        // =========================================================================

        const GPUCullingConfig& GetConfig() const { return m_config; }
        void SetConfig(const GPUCullingConfig& config);

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
                              const IRenderMaterialSource* materialResource = nullptr);

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
        RHIBuffer* GetInstanceBuffer() const { return m_instanceBuffer.Get(); }

        /**
         * @brief Get the culling constants buffer
         */
        RHIBuffer* GetCullingConstantsBuffer() const { return m_cullingConstantsBuffer.Get(); }

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

        /**
         * @brief Whether the current device/capability/pipeline state can execute GPU culling
         */
        bool IsGpuExecutionReady() const { return GetExecutionDecision().mode == GPUCullingExecutionMode::GpuCompute; }

        /**
         * @brief Fallback reason recorded by the last Cull() call
         */
        GPUCullingFallbackReason GetLastFallbackReason() const { return m_lastFallbackReason; }

        /**
         * @brief Submit the generated indexed indirect draw buffer
         * @return Number of indirect draw commands submitted
         */
        uint32 DrawIndexedIndirect(RHICommandContext& ctx, uint32 maxDrawCount = 0) const;

        /**
         * @brief Submit one mesh-compatible indirect draw group
         * @return Number of indirect draw commands submitted, or max submitted for GPU count draws
         */
        uint32 DrawIndexedIndirectGroup(RHICommandContext& ctx, uint32 groupIndex) const;

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
        void CreateResources();
        void CreatePipelineResources();
        GPUCullingExecutionDecision EvaluateGpuExecution(bool requirePipelineResources) const;
        bool SupportsGpuExecution() const;
        uint32 EnsureDefaultDrawGroup();
        void UploadInstances();
        void ExtractFrustumPlanes(const Mat4& viewProj, Vec4* planes);
        void BuildCpuCullResults(const Mat4& viewMatrix, const Vec4* frustumPlanes);
        void UploadCullOutputs(RHICommandContext* ctx = nullptr);
        bool UploadBufferData(RHIBuffer* buffer,
                              const void* data,
                              uint64 size,
                              RHICommandContext* ctx);

        IRHIDevice* m_device = nullptr;
        GPUCullingConfig m_config;
        // CPU-side instance data
        std::vector<GPUInstanceData> m_instances;
        std::vector<uint32> m_visibleInstanceIndices;
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

        // GPU buffers
        RHIBufferRef m_instanceBuffer;           // All instance data
        RHIBufferRef m_visibilityBuffer;         // Per-instance visibility flags
        RHIBufferRef m_visibleInstanceBuffer;    // Visible instance indices
        RHIBufferRef m_indirectBuffer;           // Indirect draw commands
        RHIBufferRef m_drawCountBuffer;          // Number of draws
        RHIBufferRef m_cullingConstantsBuffer;   // View/proj, frustum planes

        // Pipelines
        RHIShaderRef m_frustumCullShader;
        RHIShaderRef m_compactShader;
        RHIDescriptorSetLayoutRef m_cullingDescriptorSetLayout;
        RHIPipelineLayoutRef m_cullingPipelineLayout;
        RHIDescriptorSetRef m_cullingDescriptorSet;
        RHIPipelineRef m_frustumCullPipeline;
        RHIPipelineRef m_occlusionCullPipeline;
        RHIPipelineRef m_compactPipeline;

        // Statistics
        Statistics m_stats;
        bool m_statsEnabled = false;
        RHIBufferRef m_statsBuffer;
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
