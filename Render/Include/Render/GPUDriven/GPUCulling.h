/**
 * @file GPUCulling.h
 * @brief GPU-driven visibility culling
 * 
 * Implements GPU-based frustum and occlusion culling using compute shaders.
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class RenderScene;

    /**
     * @brief GPU instance data for culling
     */
    struct GPUInstanceData
    {
        Mat4 worldMatrix;
        Vec4 boundingSphere;  // xyz = center, w = radius
        Vec4 aabbMin;         // xyz = min, w = unused
        Vec4 aabbMax;         // xyz = max, w = unused
        uint32 meshId;
        uint32 materialId;
        uint32 flags;
        uint32 pad;
    };

    /**
     * @brief Indirect draw command (matches D3D12/Vulkan structures)
     */
    struct IndirectDrawCommand
    {
        uint32 indexCount;
        uint32 instanceCount;
        uint32 firstIndex;
        int32 vertexOffset;
        uint32 firstInstance;
    };

    /**
     * @brief Indirect indexed draw command with instance ID
     */
    struct IndirectDrawIndexedCommand
    {
        uint32 indexCount;
        uint32 instanceCount;
        uint32 firstIndex;
        int32 vertexOffset;
        uint32 firstInstance;
        uint32 instanceId;  // Custom: index into instance buffer
        uint32 pad[2];
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
         * @brief Add an instance to be culled
         * @return Instance index
         */
        uint32 AddInstance(const GPUInstanceData& instance);

        /**
         * @brief Batch add instances
         */
        void AddInstances(const GPUInstanceData* instances, uint32 count);

        /**
         * @brief End instance collection and upload to GPU
         */
        void EndFrame();

        /**
         * @brief Get current instance count
         */
        uint32 GetInstanceCount() const { return m_instanceCount; }

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
         * @brief Get the indirect draw buffer
         */
        RHIBuffer* GetIndirectBuffer() const { return m_indirectBuffer.Get(); }

        /**
         * @brief Get the draw count buffer (for indirect count)
         */
        RHIBuffer* GetDrawCountBuffer() const { return m_drawCountBuffer.Get(); }

        /**
         * @brief Get visible instance buffer
         */
        RHIBuffer* GetVisibleInstanceBuffer() const { return m_visibleInstanceBuffer.Get(); }

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
        void UploadInstances();
        void ExtractFrustumPlanes(const Mat4& viewProj, Vec4* planes);

        IRHIDevice* m_device = nullptr;
        GPUCullingConfig m_config;

        // CPU-side instance data
        std::vector<GPUInstanceData> m_instances;
        uint32 m_instanceCount = 0;

        // GPU buffers
        RHIBufferRef m_instanceBuffer;           // All instance data
        RHIBufferRef m_visibleInstanceBuffer;    // Visible instance indices
        RHIBufferRef m_indirectBuffer;           // Indirect draw commands
        RHIBufferRef m_drawCountBuffer;          // Number of draws
        RHIBufferRef m_cullingConstantsBuffer;   // View/proj, frustum planes

        // Pipelines
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
