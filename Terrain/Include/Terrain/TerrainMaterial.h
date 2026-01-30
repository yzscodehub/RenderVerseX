#pragma once

/**
 * @file TerrainMaterial.h
 * @brief Multi-layer terrain material system
 * 
 * Provides texture splatting with multiple layers for realistic
 * terrain surface rendering.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIBuffer.h"

#include <memory>
#include <vector>
#include <string>

namespace RVX
{
    class IRHIDevice;

    /**
     * @brief Maximum number of terrain texture layers
     */
    constexpr uint32 RVX_TERRAIN_MAX_LAYERS = 8;

    /**
     * @brief Terrain texture layer
     */
    struct TerrainLayer
    {
        std::string name;                   ///< Layer name
        RHITextureRef albedoTexture;        ///< Albedo/diffuse texture
        RHITextureRef normalTexture;        ///< Normal map texture
        RHITextureRef roughnessTexture;     ///< Roughness texture (optional)
        RHITextureRef aoTexture;            ///< Ambient occlusion texture (optional)
        
        float tilingScale = 10.0f;          ///< UV tiling scale
        float normalStrength = 1.0f;        ///< Normal map strength
        float roughnessValue = 0.5f;        ///< Default roughness if no texture
        float metallicValue = 0.0f;         ///< Metallic value
        
        Vec3 tintColor{1.0f, 1.0f, 1.0f};   ///< Color tint
    };

    /**
     * @brief GPU-compatible terrain layer data
     */
    struct TerrainLayerGPUData
    {
        Vec4 tilingAndStrength;     ///< (tilingScale, normalStrength, roughness, metallic)
        Vec4 tintColor;             ///< (r, g, b, unused)
    };

    /**
     * @brief Terrain material with multi-layer texture splatting
     * 
     * Supports up to 8 texture layers blended using a splatmap.
     * Each layer can have albedo, normal, roughness, and AO textures.
     * 
     * Features:
     * - Multi-layer texture splatting
     * - Triplanar mapping support
     * - Height-based blending
     * - Per-layer tiling and tinting
     * 
     * Usage:
     * @code
     * auto material = std::make_shared<TerrainMaterial>();
     * 
     * // Add texture layers
     * material->AddLayer("Grass", grassAlbedo, grassNormal, 10.0f);
     * material->AddLayer("Rock", rockAlbedo, rockNormal, 8.0f);
     * material->AddLayer("Sand", sandAlbedo, sandNormal, 15.0f);
     * 
     * // Set splatmap (RGBA texture, each channel controls one layer)
     * material->SetSplatmap(splatmapTexture);
     * 
     * // Initialize GPU resources
     * material->InitializeGPU(device);
     * @endcode
     */
    class TerrainMaterial
    {
    public:
        using Ptr = std::shared_ptr<TerrainMaterial>;

        TerrainMaterial() = default;
        ~TerrainMaterial() = default;

        // Non-copyable
        TerrainMaterial(const TerrainMaterial&) = delete;
        TerrainMaterial& operator=(const TerrainMaterial&) = delete;

        // =====================================================================
        // Layer Management
        // =====================================================================

        /**
         * @brief Add a texture layer
         * @param name Layer name
         * @param albedo Albedo texture
         * @param normal Normal map texture (optional)
         * @param tilingScale UV tiling scale
         * @return Layer index
         */
        uint32 AddLayer(const std::string& name, RHITextureRef albedo, 
                        RHITextureRef normal = nullptr, float tilingScale = 10.0f);

        /**
         * @brief Get layer by index
         * @param index Layer index
         * @return Pointer to layer, or nullptr if invalid index
         */
        TerrainLayer* GetLayer(uint32 index);
        const TerrainLayer* GetLayer(uint32 index) const;

        /**
         * @brief Get layer by name
         * @param name Layer name
         * @return Pointer to layer, or nullptr if not found
         */
        TerrainLayer* GetLayerByName(const std::string& name);

        /**
         * @brief Remove a layer
         * @param index Layer index
         */
        void RemoveLayer(uint32 index);

        /**
         * @brief Get number of layers
         */
        uint32 GetLayerCount() const { return static_cast<uint32>(m_layers.size()); }

        // =====================================================================
        // Splatmap
        // =====================================================================

        /**
         * @brief Set the splatmap texture
         * @param splatmap Splatmap texture (RGBA, each channel = layer weight)
         * 
         * For more than 4 layers, use SetSplatmaps() with multiple textures.
         */
        void SetSplatmap(RHITextureRef splatmap);

        /**
         * @brief Set multiple splatmap textures
         * @param splatmaps Array of splatmap textures
         * 
         * First splatmap controls layers 0-3, second controls 4-7.
         */
        void SetSplatmaps(const std::vector<RHITextureRef>& splatmaps);

        /**
         * @brief Get splatmap texture
         * @param index Splatmap index (0 or 1)
         */
        RHITexture* GetSplatmap(uint32 index = 0) const;

        // =====================================================================
        // Blending Options
        // =====================================================================

        /**
         * @brief Enable/disable triplanar mapping
         * @param enabled True to enable triplanar mapping
         */
        void SetTriplanarEnabled(bool enabled) { m_triplanarEnabled = enabled; }
        bool IsTriplanarEnabled() const { return m_triplanarEnabled; }

        /**
         * @brief Set triplanar blending sharpness
         * @param sharpness Blend sharpness (higher = sharper transitions)
         */
        void SetTriplanarSharpness(float sharpness) { m_triplanarSharpness = sharpness; }
        float GetTriplanarSharpness() const { return m_triplanarSharpness; }

        /**
         * @brief Enable/disable height-based blending
         * @param enabled True to enable height-based blending
         */
        void SetHeightBlendEnabled(bool enabled) { m_heightBlendEnabled = enabled; }
        bool IsHeightBlendEnabled() const { return m_heightBlendEnabled; }

        /**
         * @brief Set height blend sharpness
         * @param sharpness Blend sharpness
         */
        void SetHeightBlendSharpness(float sharpness) { m_heightBlendSharpness = sharpness; }
        float GetHeightBlendSharpness() const { return m_heightBlendSharpness; }

        // =====================================================================
        // GPU Resources
        // =====================================================================

        /**
         * @brief Initialize GPU resources
         * @param device RHI device
         * @return true if initialization succeeded
         */
        bool InitializeGPU(IRHIDevice* device);

        /**
         * @brief Update GPU buffer with layer data
         */
        void UpdateGPUData();

        /**
         * @brief Get layer data GPU buffer
         */
        RHIBuffer* GetLayerBuffer() const { return m_layerBuffer.Get(); }

        /**
         * @brief Check if GPU resources are initialized
         */
        bool IsGPUInitialized() const { return m_gpuInitialized; }

    private:
        std::vector<TerrainLayer> m_layers;
        std::vector<RHITextureRef> m_splatmaps;

        // Blending options
        bool m_triplanarEnabled = false;
        float m_triplanarSharpness = 1.0f;
        bool m_heightBlendEnabled = true;
        float m_heightBlendSharpness = 0.5f;

        // GPU resources
        RHIBufferRef m_layerBuffer;
        bool m_gpuInitialized = false;
        bool m_needsUpdate = true;
    };

} // namespace RVX
