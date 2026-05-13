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

#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include "Render/GPUUploadService.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIDevice.h"
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RVX
{
    class RHICommandContext;

    // Forward declarations
    namespace Resource
    {
        class MeshResource;
        class TextureResource;
    }

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
     */
    struct MeshGPUBuffers
    {
        RHIBuffer* positionBuffer = nullptr;  // Slot 0 - required
        RHIBuffer* normalBuffer = nullptr;    // Slot 1 - optional
        RHIBuffer* uvBuffer = nullptr;        // Slot 2 - optional
        RHIBuffer* tangentBuffer = nullptr;   // Slot 3 - optional
        RHIBuffer* indexBuffer = nullptr;
        std::vector<SubmeshGPUInfo> submeshes;
        bool isResident = false;
        
        bool IsValid() const { return positionBuffer && indexBuffer && isResident; }
    };

    /**
     * @brief Internal data for a mesh in GPU memory (separate buffers per attribute)
     */
    struct MeshGPUData
    {
        // Separate buffers for each attribute (matches glTF storage)
        RHIBufferRef positionBuffer;    // Slot 0 - required
        RHIBufferRef normalBuffer;      // Slot 1 - optional
        RHIBufferRef uvBuffer;          // Slot 2 - optional
        RHIBufferRef tangentBuffer;     // Slot 3 - optional
        RHIBufferRef indexBuffer;
        
        std::vector<SubmeshGPUInfo> submeshes;
        std::vector<uint64> pendingUploadIds;
        uint64_t lastUsedFrame = 0;
        size_t gpuMemorySize = 0;
        bool isResident = false;
        
        // Track which attributes are available
        bool hasNormals = false;
        bool hasUVs = false;
        bool hasTangents = false;
    };

    /**
     * @brief Internal data for a texture in GPU memory
     */
    struct TextureGPUData
    {
        RHITextureRef texture;
        std::vector<uint64> pendingUploadIds;
        uint64_t lastUsedFrame = 0;
        size_t gpuMemorySize = 0;
        RHIResourceState currentState = RHIResourceState::Common;
        bool isResident = false;
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
        bool IsInitialized() const { return m_device != nullptr; }

        // =====================================================================
        // Upload Requests
        // =====================================================================

        /// Request async upload of a mesh
        void RequestUpload(Resource::MeshResource* mesh, UploadPriority priority = UploadPriority::Normal);

        /// Request async upload of a texture
        void RequestUpload(Resource::TextureResource* texture, UploadPriority priority = UploadPriority::Normal);

        /// Upload a mesh immediately (blocking)
        void UploadImmediate(Resource::MeshResource* mesh);

        /// Upload a texture immediately (blocking)
        void UploadImmediate(Resource::TextureResource* texture);

        /**
         * @brief Called before a resident GPU texture object is released or replaced.
         *
         * Render-side caches that hold RHITextureView pointers must invalidate views
         * derived from the texture before the texture reference is dropped.
         */
        using TextureInvalidatedCallback = std::function<void(RHITexture*)>;
        void SetTextureInvalidatedCallback(TextureInvalidatedCallback callback)
        {
            m_textureInvalidatedCallback = std::move(callback);
        }

        // =====================================================================
        // Resource Query
        // =====================================================================

        /// Get GPU buffers for a mesh (returns empty if not resident)
        MeshGPUBuffers GetMeshBuffers(Resource::ResourceId meshId) const;

        /// Get GPU texture (returns nullptr if not resident)
        RHITexture* GetTexture(Resource::ResourceId textureId) const;

        /// Transition a resident texture to the requested state if needed
        bool TransitionTexture(Resource::ResourceId textureId, RHICommandContext& ctx, RHIResourceState desiredState);

        /// Check if a resource is GPU-resident
        bool IsResident(Resource::ResourceId id) const;

        /// Get the resource upload/residency state
        GPUResourceState GetResourceState(Resource::ResourceId id) const;

        /// Check if a resource is ready for rendering
        bool IsGPUReady(Resource::ResourceId id) const { return GetResourceState(id) == GPUResourceState::GPUReady; }

        // =====================================================================
        // Per-Frame Processing
        // =====================================================================

        /// Process pending uploads with time budget
        /// @param timeBudgetMs Maximum time to spend uploading (in milliseconds)
        void ProcessPendingUploads(float timeBudgetMs = 2.0f);

        /// Mark a resource as used this frame (for eviction tracking)
        void MarkUsed(Resource::ResourceId id);

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
        size_t GetUsedMemory() const { return m_usedMemory; }

        /// Get memory budget
        size_t GetMemoryBudget() const { return m_memoryBudget; }

        /// Check if we're over budget
        bool IsOverBudget() const { return m_usedMemory > m_memoryBudget; }

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
        struct PendingUpload
        {
            Resource::ResourceId id;
            UploadPriority priority;
            Resource::ResourceHandle<Resource::IResource> retainedResource;

            bool operator<(const PendingUpload& other) const
            {
                // std::priority_queue is a max-heap, so larger priority values run first.
                return static_cast<int>(priority) < static_cast<int>(other.priority);
            }
        };

        void UploadMesh(Resource::MeshResource* mesh);
        void UploadTexture(Resource::TextureResource* texture);
        void UpdateCompletedResourceUploads();
        void AbandonUploadIds(const std::vector<uint64>& uploadIds);
        void NotifyTextureInvalidated(RHITexture* texture);
        void SetResourceState(Resource::ResourceId id, GPUResourceState state);

        IRHIDevice* m_device = nullptr;

        // Pending upload queue (priority queue)
        std::priority_queue<PendingUpload> m_pendingQueue;

        // Resident resources
        std::unordered_map<Resource::ResourceId, MeshGPUData> m_meshGPUData;
        std::unordered_map<Resource::ResourceId, TextureGPUData> m_textureGPUData;
        std::unordered_map<Resource::ResourceId, GPUResourceState> m_resourceStates;
        std::unordered_set<Resource::ResourceId> m_pendingMeshUploadCompletions;
        std::unordered_set<Resource::ResourceId> m_pendingTextureUploadCompletions;
        std::unique_ptr<GPUUploadService> m_uploadService;
        TextureInvalidatedCallback m_textureInvalidatedCallback;

        // Memory tracking
        size_t m_usedMemory = 0;
        size_t m_memoryBudget = 512 * 1024 * 1024;  // Default 512MB

        // Frame counter for eviction
        uint64_t m_currentFrame = 0;
    };

} // namespace RVX
