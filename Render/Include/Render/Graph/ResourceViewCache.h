#pragma once

/**
 * @file ResourceViewCache.h
 * @brief Completion-safe views for imported and persistent resources
 * 
 * Transient RenderGraph views live with their transient pool entries. This cache
 * is only for imported/persistent resources and strongly owns every cached view.
 */

#include "RHI/RHI.h"
#include <memory>

namespace RVX
{
    struct GPUCompletionToken;
    class RenderRetirementQueue;
    class RenderSubmissionTracker;

    /**
     * @brief Cache for GPU resource views
     * 
     * Provides automatic view creation and caching to avoid redundant persistent
     * view creation. Views are indexed by the resource instance identity and the
     * complete view description; a recycled C++ address can never alias an old key.
     */
    class ResourceViewCache
    {
    public:
        ResourceViewCache();
        ~ResourceViewCache();

        // Non-copyable
        ResourceViewCache(const ResourceViewCache&) = delete;
        ResourceViewCache& operator=(const ResourceViewCache&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize the cache with a device
         * @param device The RHI device for view creation
         */
        void Initialize(IRHIDevice* device,
                        RenderSubmissionTracker* submissionTracker = nullptr,
                        RenderRetirementQueue* retirementQueue = nullptr);

        /**
         * @brief Shutdown and release all cached views
         */
        void Shutdown();

        /**
         * @brief Check if the cache is initialized
         */
        bool IsInitialized() const;

        /**
         * @brief Monotonic version incremented when cached view pointers may be invalidated
         */
        uint64 GetGeneration() const;

        /** @brief Stamp views touched since the previous submission with its actual token. */
        void NotifySubmission(const GPUCompletionToken& completion);

        // =========================================================================
        // View Acquisition
        // =========================================================================

        /**
         * @brief Get or create a texture view
         * @param texture The texture to create a view for
         * @param desc The view description
         * @return Cached or newly created texture view, or nullptr on failure
         */
        RHITextureView* GetTextureView(RHITexture* texture, const RHITextureViewDesc& desc);

        /**
         * @brief Get or create a default SRV for a texture
         * @param texture The texture
         * @return Default shader resource view
         */
        RHITextureView* GetDefaultSRV(RHITexture* texture);

        /**
         * @brief Get or create a default RTV for a texture
         * @param texture The texture
         * @return Default render target view
         */
        RHITextureView* GetDefaultRTV(RHITexture* texture);

        /**
         * @brief Get or create a default DSV for a texture
         * @param texture The texture
         * @return Default depth-stencil view
         */
        RHITextureView* GetDefaultDSV(RHITexture* texture);

        /**
         * @brief Get or create a default UAV for a texture
         * @param texture The texture
         * @return Default unordered access view
         */
        RHITextureView* GetDefaultUAV(RHITexture* texture);

        // =========================================================================
        // Cache Management
        // =========================================================================

        /**
         * @brief Mark the beginning of a new frame
         * 
         * Called each frame to track view usage and enable cleanup.
         */
        void BeginFrame();

        /**
         * @brief Invalidate all views for a specific texture
         * @param texture The texture whose views should be invalidated
         * 
         * Call this when a texture is destroyed or recreated.
         */
        void InvalidateTexture(RHITexture* texture);

        /**
         * @brief Clear all cached views
         */
        void Clear();

        // =========================================================================
        // Statistics
        // =========================================================================

        struct Stats
        {
            uint32 textureViewCount = 0;
            uint32 cacheHits = 0;
            uint32 cacheMisses = 0;
        };

        /**
         * @brief Get cache statistics
         */
        Stats GetStats() const;

        /**
         * @brief Reset per-frame statistics
         */
        void ResetFrameStats();

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };

} // namespace RVX
