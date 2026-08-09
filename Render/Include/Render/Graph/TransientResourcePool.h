#pragma once

/**
 * @file TransientResourcePool.h
 * @brief Resource pool for transient RenderGraph resources
 *
 * TransientResourcePool caches GPU resources across frames to avoid
 * repeated allocation/deallocation overhead. Resources are matched
 * by their description hash and reused when available.
 */

#include "RHI/RHI.h"
#include <memory>
#include <unordered_map>
namespace RVX
{
    struct GPUCompletionToken;
    class RenderRetirementQueue;
    class RenderSubmissionTracker;
    class TransientResourceLeaseControl;

    enum class TransientResourceLeaseState : uint8
    {
        Free = 0,
        Recording,
        InFlight,
        Evicting,
    };

    class TransientTextureLease
    {
    public:
        TransientTextureLease() = default;
        ~TransientTextureLease();
        TransientTextureLease(TransientTextureLease&& other) noexcept;
        TransientTextureLease& operator=(TransientTextureLease&& other) noexcept;
        TransientTextureLease(const TransientTextureLease&) = delete;
        TransientTextureLease& operator=(const TransientTextureLease&) = delete;

        RHITexture* texture = nullptr;
        RHITextureAccessSnapshot accessSnapshot;
        bool reused = false;

        explicit operator bool() const
        {
            return texture != nullptr && !m_resolved;
        }

        [[nodiscard]] bool Commit(
            const GPUCompletionToken& completion,
            const RHITextureAccessSnapshot& finalAccess);
        [[nodiscard]] bool CanCommit(
            const GPUCompletionToken& completion) const;
        [[nodiscard]] bool AbortUnsubmitted();
        [[nodiscard]] bool MarkDeviceLost();
        [[nodiscard]] uint64 GetSlotId() const noexcept { return m_slotId; }
        [[nodiscard]] uint32 GetGeneration() const noexcept
        {
            return m_generation;
        }
        [[nodiscard]] bool IsResolved() const noexcept { return m_resolved; }

    private:
        friend class TransientResourcePool;
        TransientTextureLease(
            std::shared_ptr<TransientResourceLeaseControl> control,
            uint64 poolIdentity,
            uint64 slotId,
            uint32 generation,
            RHITexture* texture,
            RHITextureAccessSnapshot accessSnapshot,
            bool reused);
        void AutoAbort() noexcept;
        void Reset() noexcept;

        std::shared_ptr<TransientResourceLeaseControl> m_control;
        uint64 m_poolIdentity = 0;
        uint64 m_slotId = 0;
        uint32 m_generation = 0;
        bool m_resolved = true;
    };

    class TransientBufferLease
    {
    public:
        TransientBufferLease() = default;
        ~TransientBufferLease();
        TransientBufferLease(TransientBufferLease&& other) noexcept;
        TransientBufferLease& operator=(TransientBufferLease&& other) noexcept;
        TransientBufferLease(const TransientBufferLease&) = delete;
        TransientBufferLease& operator=(const TransientBufferLease&) = delete;

        RHIBuffer* buffer = nullptr;
        RHIBufferAccessSnapshot accessSnapshot;
        bool reused = false;

        explicit operator bool() const
        {
            return buffer != nullptr && !m_resolved;
        }

        [[nodiscard]] bool Commit(
            const GPUCompletionToken& completion,
            const RHIBufferAccessSnapshot& finalAccess);
        [[nodiscard]] bool CanCommit(
            const GPUCompletionToken& completion) const;
        [[nodiscard]] bool AbortUnsubmitted();
        [[nodiscard]] bool MarkDeviceLost();
        [[nodiscard]] uint64 GetSlotId() const noexcept { return m_slotId; }
        [[nodiscard]] uint32 GetGeneration() const noexcept
        {
            return m_generation;
        }
        [[nodiscard]] bool IsResolved() const noexcept { return m_resolved; }

    private:
        friend class TransientResourcePool;
        TransientBufferLease(
            std::shared_ptr<TransientResourceLeaseControl> control,
            uint64 poolIdentity,
            uint64 slotId,
            uint32 generation,
            RHIBuffer* buffer,
            RHIBufferAccessSnapshot accessSnapshot,
            bool reused);
        void AutoAbort() noexcept;
        void Reset() noexcept;

        std::shared_ptr<TransientResourceLeaseControl> m_control;
        uint64 m_poolIdentity = 0;
        uint64 m_slotId = 0;
        uint32 m_generation = 0;
        bool m_resolved = true;
    };

    /**
     * @brief Pool for transient GPU resources
     * 
     * Caches textures and buffers created by RenderGraph for reuse across frames.
     * Resources unused for a configurable number of frames are automatically evicted.
     * 
     * Usage:
     * @code
     * TransientResourcePool pool;
     * pool.Initialize(device);
     * 
     * // Each frame:
     * pool.BeginFrame();
     * RHITexture* tex = pool.AcquireTexture(desc);
     * // ... use texture in RenderGraph ...
     * pool.ReleaseTexture(tex);
     * pool.EndFrame();
     * pool.EvictUnused(3);  // Evict resources unused for 3 frames
     * @endcode
     */
    class TransientResourcePool
    {
    public:
        TransientResourcePool();
        ~TransientResourcePool();

        // Non-copyable
        TransientResourcePool(const TransientResourcePool&) = delete;
        TransientResourcePool& operator=(const TransientResourcePool&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize the pool with a device
         * @param device The RHI device for resource creation
         */
        void Initialize(IRHIDevice* device,
                        RenderSubmissionTracker* submissionTracker = nullptr,
                        RenderRetirementQueue* retirementQueue = nullptr);

        /**
         * @brief Shutdown and release all pooled resources
         */
        void Shutdown();

        /**
         * @brief Check if the pool is initialized
         */
        bool IsInitialized() const;

        /** @brief Publish the actual token for the most recently recorded pool use. */
        void NotifySubmission(const GPUCompletionToken& completion);

        // =========================================================================
        // Frame Management
        // =========================================================================

        /**
         * @brief Begin a new frame
         * 
         * Call at the start of each frame before acquiring resources.
         */
        void BeginFrame();

        /**
         * @brief End the current frame
         * 
         * Call at the end of each frame after all resources are released.
         */
        void EndFrame();

        // =========================================================================
        // Resource Acquisition
        // =========================================================================

        /**
         * @brief Acquire a texture from the pool
         * @param desc The texture description
         * @return A texture matching the description, or nullptr on failure
         * 
         * Returns a cached texture if one matches, otherwise creates a new one.
         * The returned texture is valid until ReleaseTexture is called.
         */
        RHITexture* AcquireTexture(const RHITextureDesc& desc);

        /** @brief Acquire a texture together with its last realized access snapshot. */
        TransientTextureLease AcquireTextureLease(const RHITextureDesc& desc);

        /**
         * @brief Acquire a buffer from the pool
         * @param desc The buffer description
         * @return A buffer matching the description, or nullptr on failure
         */
        RHIBuffer* AcquireBuffer(const RHIBufferDesc& desc);

        /** @brief Acquire a buffer together with its last realized access snapshot. */
        TransientBufferLease AcquireBufferLease(const RHIBufferDesc& desc);

        /**
         * @brief Release a texture back to the pool
         * @param texture The texture to release
         * 
         * The texture becomes available for reuse in subsequent frames.
         */
        void ReleaseTexture(RHITexture* texture);

        /** @brief Release a texture and commit its final realized access snapshot. */
        void ReleaseTexture(RHITexture* texture, const RHITextureAccessSnapshot& finalAccess);

        /**
         * @brief Release a buffer back to the pool
         * @param buffer The buffer to release
         */
        void ReleaseBuffer(RHIBuffer* buffer);

        /** @brief Release a buffer and commit its final realized access snapshot. */
        void ReleaseBuffer(RHIBuffer* buffer, const RHIBufferAccessSnapshot& finalAccess);

        // =========================================================================
        // Eviction
        // =========================================================================

        /**
         * @brief Evict resources unused for the specified number of frames
         * @param frameThreshold Resources unused for this many frames are evicted
         */
        void EvictUnused(uint32 frameThreshold = 3);

        // =========================================================================
        // Statistics
        // =========================================================================

        struct Stats
        {
            uint32 texturePoolSize = 0;      // Total textures in pool
            uint32 bufferPoolSize = 0;       // Total buffers in pool
            uint32 texturesInUse = 0;        // Textures currently acquired
            uint32 buffersInUse = 0;         // Buffers currently acquired
            uint32 textureHits = 0;          // Cache hits this frame
            uint32 textureMisses = 0;        // Cache misses this frame
            uint32 bufferHits = 0;
            uint32 bufferMisses = 0;
            uint64 totalPooledMemory = 0;    // Estimated memory in pool
            uint32 recordingTextureLeases = 0;
            uint32 recordingBufferLeases = 0;
            uint32 inFlightTextureLeases = 0;
            uint32 inFlightBufferLeases = 0;
            uint64 leaseCommitCount = 0;
            uint64 leaseAbortCount = 0;
            uint64 leaseDeviceLostCount = 0;
            uint64 leaseValidationFailureCount = 0;
        };

        /**
         * @brief Get pool statistics
         */
        Stats GetStats() const;

        /**
         * @brief Reset per-frame statistics (hits/misses)
         */
        void ResetFrameStats();

    private:
        // Short-lived source compatibility for the legacy raw-pointer API.
        // The adapter owns the real generation-safe lease until Release*().
        std::unordered_map<RHITexture*, TransientTextureLease>
            m_legacyTextureLeases;
        std::unordered_map<RHIBuffer*, TransientBufferLease>
            m_legacyBufferLeases;

        // Hash function for texture descriptions
        static uint64 HashTextureDesc(const RHITextureDesc& desc);
        static uint64 HashBufferDesc(const RHIBufferDesc& desc);

        // Estimate resource memory size
        static uint64 EstimateTextureMemory(const RHITextureDesc& desc);
        static uint64 EstimateBufferMemory(const RHIBufferDesc& desc);

        class Impl;
        std::shared_ptr<Impl> m_impl;
    };

} // namespace RVX
