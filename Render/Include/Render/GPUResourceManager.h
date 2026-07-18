#pragma once

/**
 * @file GPUResourceManager.h
 * @brief Manages GPU resources (vertex buffers, index buffers, textures)
 * 
 * GPUResourceManager handles:
 * - Async upload of CPU resources to GPU
 * - GPU resource caching and lookup by ResourceId
 * - Memory budget management
 * - Resource eviction for unused resources
 */

#include "RenderContracts/RenderResource.h"
#include "RHI/RHIDefinitions.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace RVX
{
    class RHICommandContext;
    class RHIBuffer;
    class RHITexture;
    class IRHIDevice;
    class RenderResourceRegistry;

    /**
     * @brief Upload priority levels
     */
    enum class UploadPriority : uint8_t
    {
        Low = 0,
        Normal = 1,
        High = 2,
        Immediate = 3
    };

    /**
     * @brief CPU-to-GPU residency lifecycle for render resources.
     */
    enum class GPUResourceState : uint8_t
    {
        Unloaded = 0,
        CPUReady,
        UploadQueued,
        Uploading,
        GPUReady,
        Failed
    };

    /**
     * @brief Information about a submesh in GPU memory
     */
    struct SubmeshGPUInfo
    {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        int32_t baseVertex = 0;
    };

    /**
     * @brief GPU buffers for a mesh (separate buffers per attribute)
     * 
     * Uses separate vertex buffer slots to match glTF storage:
     *   Slot 0: Position (float3)
     *   Slot 1: Normal (float3) - optional
     *   Slot 2: UV (float2) - optional
     *   Slot 3: Tangent (float4) - optional
     *   Slot 4: Bone indices (uint4) - optional
     *   Slot 5: Bone weights (float4) - optional
     */
    struct MeshGPUBuffers
    {
        RHIBuffer* positionBuffer = nullptr;  // Slot 0 - required
        RHIBuffer* normalBuffer = nullptr;    // Slot 1 - optional
        RHIBuffer* uvBuffer = nullptr;        // Slot 2 - optional
        RHIBuffer* tangentBuffer = nullptr;   // Slot 3 - optional
        RHIBuffer* boneIndicesBuffer = nullptr;  // Slot 4 - optional
        RHIBuffer* boneWeightsBuffer = nullptr;  // Slot 5 - optional
        RHIBuffer* indexBuffer = nullptr;
        std::vector<SubmeshGPUInfo> submeshes;
        bool isResident = false;
        bool hasNormals = false;
        bool hasUVs = false;
        bool hasTangents = false;
        bool hasBoneIndices = false;
        bool hasBoneWeights = false;

        bool IsValid() const { return positionBuffer && indexBuffer && isResident; }
        bool HasNormalMapTangentBasis() const
        {
            return normalBuffer && uvBuffer && tangentBuffer &&
                   hasNormals && hasUVs && hasTangents;
        }
        bool HasSkinningVertexData() const
        {
            return boneIndicesBuffer && boneWeightsBuffer &&
                   hasBoneIndices && hasBoneWeights;
        }
    };

    /**
     * @brief GPU Resource Manager
     * 
     * Manages the lifecycle of GPU resources, handling:
     * - Deferred upload with priority queue
     * - Resource residency tracking
     * - Memory budget management
     * - Automatic eviction of unused resources
     * 
     * Usage:
     * @code
     * // Request upload
     * gpuManager->RequestUpload(meshResource);
     * 
     * // Process uploads (called once per frame)
     * gpuManager->ProcessPendingUploads(2.0f);  // 2ms budget
     * 
     * // Get buffers for rendering
     * auto buffers = gpuManager->GetMeshBuffers(meshId);
     * if (buffers.IsValid()) {
     *     ctx->SetVertexBuffer(0, buffers.vertexBuffer);
     *     ...
     * }
     * @endcode
     */
    /**
     * @brief Temporary resource facade removed by task 18 after all callers
     * resolve exact-generation resources through the Render-owned registry.
     */
    class GPUResourceManager
    {
    public:
        GPUResourceManager();
        ~GPUResourceManager();

        // =====================================================================
        // Initialization
        // =====================================================================

        /// Initialize with RHI device
        void Initialize(IRHIDevice* device);

        /// Shutdown and release all GPU resources
        void Shutdown();

        /// Check if initialized
        bool IsInitialized() const;

        // =====================================================================
        // Upload Requests
        // =====================================================================

        /// Request async upload of a mesh
        void RequestUpload(IRenderMeshUploadSource* mesh, UploadPriority priority = UploadPriority::Normal);

        /// Request async upload of a texture
        void RequestUpload(IRenderTextureUploadSource* texture, UploadPriority priority = UploadPriority::Normal);

        /// Upload a mesh immediately (blocking)
        void UploadImmediate(IRenderMeshUploadSource* mesh);

        /// Upload a texture immediately (blocking)
        void UploadImmediate(IRenderTextureUploadSource* texture);

        /**
         * @brief Called before a resident GPU texture object is released or replaced.
         *
         * Render-side caches that hold RHITextureView pointers must invalidate views
         * derived from the texture before the texture reference is dropped.
         */
        using TextureInvalidatedCallback = std::function<void(RHITexture*)>;
        void SetTextureInvalidatedCallback(TextureInvalidatedCallback callback);

        // =====================================================================
        // Resource Query
        // =====================================================================

        /// Get GPU buffers for a mesh (returns empty if not resident)
        MeshGPUBuffers GetMeshBuffers(uint64 meshId) const;

        /// Get GPU texture (returns nullptr if not resident)
        RHITexture* GetTexture(uint64 textureId) const;
        RHITexture* GetTexture(IRenderTextureUploadSource* texture) const;

        /// Transition a resident texture to the requested state if needed
        bool TransitionTexture(uint64 textureId, RHICommandContext& ctx, RHIResourceState desiredState);

        /// Check if a resource is GPU-resident
        bool IsResident(uint64 id) const;
        bool IsResident(IRenderTextureUploadSource* texture) const;

        /// Get the resource upload/residency state
        GPUResourceState GetResourceState(uint64 id) const;

        /// Check if a resource is ready for rendering
        bool IsGPUReady(uint64 id) const { return GetResourceState(id) == GPUResourceState::GPUReady; }
        bool IsGPUReady(IRenderTextureUploadSource* texture) const;

        // =====================================================================
        // Per-Frame Processing
        // =====================================================================

        /// Process pending uploads with time budget
        /// @param timeBudgetMs Maximum time to spend uploading (in milliseconds)
        void ProcessPendingUploads(float timeBudgetMs = 2.0f);

        /// Mark a resource as used this frame (for eviction tracking)
        void MarkUsed(uint64 id);
        void MarkUsed(IRenderTextureUploadSource* texture);

        /// Evict unused resources
        /// @param currentFrame Current frame number
        /// @param frameThreshold Resources unused for this many frames will be evicted
        void EvictUnused(uint64_t currentFrame, uint64_t frameThreshold = 300);

        // =====================================================================
        // Memory Management
        // =====================================================================

        /// Set GPU memory budget
        void SetMemoryBudget(size_t bytes);

        /// Get current GPU memory usage
        size_t GetUsedMemory() const;

        /// Get memory budget
        size_t GetMemoryBudget() const;

        /// Check if we're over budget
        bool IsOverBudget() const;

        // =====================================================================
        // Statistics
        // =====================================================================

        struct Stats
        {
            size_t residentMeshCount = 0;
            size_t residentTextureCount = 0;
            size_t pendingUploadCount = 0;
            size_t queuedUploadCount = 0;
            size_t uploadingCount = 0;
            size_t failedUploadCount = 0;
            size_t usedMemory = 0;
            size_t memoryBudget = 0;
        };

        Stats GetStats() const;

    private:
        // Temporary task-18 migration adapter. All maps, queues, state, and
        // strong RHI references live in RenderResourceRegistry.
        std::unique_ptr<RenderResourceRegistry> m_registry;
    };

} // namespace RVX
