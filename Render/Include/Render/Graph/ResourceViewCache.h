#pragma once

/**
 * @file ResourceViewCache.h
 * @brief Automatic resource view creation and caching for RenderGraph
 * 
 * ResourceViewCache manages the creation and lifecycle of RHITextureView objects.
 * Views are cached by their description hash and automatically cleaned up when
 * no longer needed.
 */

#include "RHI/RHI.h"
#include <unordered_map>

namespace RVX
{
    /**
     * @brief Cache for GPU resource views
     * 
     * Provides automatic view creation and caching to avoid redundant view creation
     * each frame. Views are indexed by a combination of resource pointer and view description.
     */
    class ResourceViewCache
    {
    public:
        ResourceViewCache() = default;
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
        void Initialize(IRHIDevice* device);

        /**
         * @brief Shutdown and release all cached views
         */
        void Shutdown();

        /**
         * @brief Check if the cache is initialized
         */
        bool IsInitialized() const { return m_device != nullptr; }

        /**
         * @brief Monotonic version incremented when cached view pointers may be invalidated
         */
        uint64 GetGeneration() const { return m_generation; }

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
        // Cache identity uses GPU-visible view parameters. debugName is intentionally
        // excluded because it does not change the created view semantics.
        struct TextureViewKey
        {
            RHITexture* texture = nullptr;
            RHIFormat format = RHIFormat::Unknown;
            RHITextureDimension dimension = RHITextureDimension::Texture2D;
            RHISubresourceRange subresourceRange;

            bool operator==(const TextureViewKey& other) const
            {
                return texture == other.texture &&
                       format == other.format &&
                       dimension == other.dimension &&
                       subresourceRange.baseMipLevel == other.subresourceRange.baseMipLevel &&
                       subresourceRange.mipLevelCount == other.subresourceRange.mipLevelCount &&
                       subresourceRange.baseArrayLayer == other.subresourceRange.baseArrayLayer &&
                       subresourceRange.arrayLayerCount == other.subresourceRange.arrayLayerCount &&
                       subresourceRange.aspect == other.subresourceRange.aspect;
            }
        };

        struct TextureViewKeyHash
        {
            size_t operator()(const TextureViewKey& key) const;
        };

        static TextureViewKey MakeTextureViewKey(RHITexture* texture, const RHITextureViewDesc& desc);

        struct CachedTextureView
        {
            RHITextureViewRef view;
            RHITexture* texture = nullptr;
            uint32 lastUsedFrame = 0;
        };

        IRHIDevice* m_device = nullptr;
        uint32 m_currentFrame = 0;
        uint64 m_generation = 0;

        // Cache maps: hash -> cached view
        std::unordered_map<TextureViewKey, CachedTextureView, TextureViewKeyHash> m_textureViews;

        // Statistics
        mutable Stats m_stats;
    };

} // namespace RVX
