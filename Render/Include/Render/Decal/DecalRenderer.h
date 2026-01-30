#pragma once

/**
 * @file DecalRenderer.h
 * @brief Deferred decal rendering system
 * 
 * Renders decals onto scene geometry using deferred projection.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <vector>
#include <memory>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class RenderGraph;
    struct RGTextureHandle;
    class Material;

    /**
     * @brief Decal blend mode
     */
    enum class DecalBlendMode : uint8
    {
        Normal,         ///< Normal alpha blend
        Additive,       ///< Additive blend
        Multiply,       ///< Multiply blend
        Stain           ///< Color stain (modulates albedo only)
    };

    /**
     * @brief Decal data structure
     */
    struct DecalData
    {
        Mat4 transform;                     ///< World transform (includes scale)
        
        // Textures (can be null for solid color)
        RHITexture* albedoMap = nullptr;
        RHITexture* normalMap = nullptr;
        RHITexture* roughnessMap = nullptr;
        
        // Properties
        Vec4 color = Vec4(1.0f);            ///< Base color/tint (RGBA)
        float normalStrength = 1.0f;        ///< Normal map strength
        float roughness = 0.5f;             ///< Roughness (if no map)
        float metallic = 0.0f;              ///< Metallic value
        
        // Blending
        DecalBlendMode blendMode = DecalBlendMode::Normal;
        float albedoContribution = 1.0f;    ///< How much to affect albedo
        float normalContribution = 1.0f;    ///< How much to affect normal
        float roughnessContribution = 1.0f; ///< How much to affect roughness
        
        // Fade
        float fadeDistance = 0.0f;          ///< Distance to start fading (0 = no fade)
        float angleFade = 0.0f;             ///< Angle fade threshold (0 = no angle fade)
        
        // Sorting
        int32 sortOrder = 0;                ///< Sort order (lower = rendered first)
        uint32 layerMask = 0xFFFFFFFF;      ///< Which layers this decal affects
        
        // Debug
        bool debugDraw = false;             ///< Draw decal bounds for debugging
    };

    /**
     * @brief Decal renderer configuration
     */
    struct DecalRendererConfig
    {
        uint32 maxDecals = 1024;            ///< Maximum decals per frame
        bool sortDecals = true;             ///< Sort decals by priority
        bool enableNormalMapping = true;    ///< Enable normal map decals
        bool enableAngleFade = true;        ///< Enable angle-based fading
        bool clusterDecals = false;         ///< Use clustered decal rendering
    };

    /**
     * @brief Deferred decal renderer
     * 
     * Renders decals by projecting textures onto existing geometry.
     * 
     * Features:
     * - Deferred decal projection
     * - Normal map decals
     * - Multiple blend modes
     * - Distance and angle fading
     * - Layer masking
     * - Clustered rendering (optional)
     * 
     * Usage:
     * 1. Add decals with AddDecal() before rendering
     * 2. Call Render() after the GBuffer pass
     * 3. Decals modify the GBuffer in-place
     */
    class DecalRenderer
    {
    public:
        DecalRenderer() = default;
        ~DecalRenderer();

        // Non-copyable
        DecalRenderer(const DecalRenderer&) = delete;
        DecalRenderer& operator=(const DecalRenderer&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        void Initialize(IRHIDevice* device, const DecalRendererConfig& config = {});
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Configuration
        // =========================================================================

        const DecalRendererConfig& GetConfig() const { return m_config; }
        void SetConfig(const DecalRendererConfig& config);

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // =========================================================================
        // Decal Management
        // =========================================================================

        /**
         * @brief Add a decal to be rendered this frame
         * @return Decal index (for removal/modification)
         */
        uint32 AddDecal(const DecalData& decal);

        /**
         * @brief Update an existing decal
         */
        void UpdateDecal(uint32 index, const DecalData& decal);

        /**
         * @brief Remove a decal
         */
        void RemoveDecal(uint32 index);

        /**
         * @brief Clear all decals
         */
        void ClearDecals();

        /**
         * @brief Get current decal count
         */
        uint32 GetDecalCount() const { return static_cast<uint32>(m_decals.size()); }

        // =========================================================================
        // Rendering
        // =========================================================================

        /**
         * @brief Render decals to the GBuffer
         * @param ctx Command context
         * @param gBufferAlbedo GBuffer albedo texture
         * @param gBufferNormal GBuffer normal texture
         * @param gBufferRoughness GBuffer roughness/metallic texture
         * @param depthBuffer Scene depth buffer
         * @param viewMatrix Camera view matrix
         * @param projMatrix Camera projection matrix
         */
        void Render(RHICommandContext& ctx,
                    RHITexture* gBufferAlbedo,
                    RHITexture* gBufferNormal,
                    RHITexture* gBufferRoughness,
                    RHITexture* depthBuffer,
                    const Mat4& viewMatrix,
                    const Mat4& projMatrix);

        /**
         * @brief Add decal rendering to render graph
         */
        void AddToGraph(RenderGraph& graph,
                        RGTextureHandle gBufferAlbedo,
                        RGTextureHandle gBufferNormal,
                        RGTextureHandle gBufferRoughness,
                        RGTextureHandle depthBuffer,
                        const Mat4& viewMatrix,
                        const Mat4& projMatrix);

        // =========================================================================
        // Utility
        // =========================================================================

        /**
         * @brief Create a decal transform from position, rotation, and size
         */
        static Mat4 CreateDecalTransform(const Vec3& position, 
                                          const Quat& rotation,
                                          const Vec3& size);

        /**
         * @brief Check if a point is inside the decal volume
         */
        static bool IsPointInDecal(const Vec3& point, const DecalData& decal);

    private:
        void SortDecals();
        void UploadDecalData(RHICommandContext& ctx);
        void RenderDecalBatch(RHICommandContext& ctx, uint32 startIndex, uint32 count);

        IRHIDevice* m_device = nullptr;
        DecalRendererConfig m_config;
        bool m_enabled = true;

        // Decal list
        std::vector<DecalData> m_decals;
        std::vector<uint32> m_sortedIndices;
        bool m_needsSort = true;

        // GPU resources
        RHIBufferRef m_decalBuffer;         // Instance data
        RHIBufferRef m_constantBuffer;
        RHIPipelineRef m_decalPipeline;
        RHIPipelineRef m_decalNormalPipeline;
        RHIPipelineRef m_decalStainPipeline;

        // View data
        Mat4 m_viewMatrix;
        Mat4 m_projMatrix;
        Mat4 m_invViewProj;
    };

} // namespace RVX
