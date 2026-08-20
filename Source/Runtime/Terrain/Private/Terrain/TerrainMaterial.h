#pragma once

/**
 * @file TerrainMaterial.h
 * @brief Multi-layer terrain material state.
 *
 * Provides CPU texture-splatting configuration exported to Render-owned terrain
 * passes.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    /**
     * @brief Maximum number of terrain texture layers.
     */
    constexpr uint32 RVX_TERRAIN_MAX_LAYERS = 8;

    /**
     * @brief Terrain texture layer description without renderer backend handles.
     */
    struct TerrainLayer
    {
        std::string name;
        std::string albedoTexture;
        std::string normalTexture;
        std::string roughnessTexture;
        std::string aoTexture;

        float tilingScale = 10.0f;
        float normalStrength = 1.0f;
        float roughnessValue = 0.5f;
        float metallicValue = 0.0f;

        Vec3 tintColor{1.0f, 1.0f, 1.0f};
    };

    /**
     * @brief Terrain layer constants exported to Render.
     */
    struct TerrainLayerRenderData
    {
        Vec4 tilingAndStrength;
        Vec4 tintColor;
    };

    /**
     * @brief Terrain material with multi-layer texture splatting state.
     */
    class TerrainMaterial
    {
    public:
        using Ptr = std::shared_ptr<TerrainMaterial>;

        TerrainMaterial() = default;
        ~TerrainMaterial() = default;

        TerrainMaterial(const TerrainMaterial&) = delete;
        TerrainMaterial& operator=(const TerrainMaterial&) = delete;

        uint32 AddLayer(const std::string& name,
                        std::string albedoTexture = {},
                        std::string normalTexture = {},
                        float tilingScale = 10.0f);

        TerrainLayer* GetLayer(uint32 index);
        const TerrainLayer* GetLayer(uint32 index) const;
        TerrainLayer* GetLayerByName(const std::string& name);
        void RemoveLayer(uint32 index);
        uint32 GetLayerCount() const { return static_cast<uint32>(m_layers.size()); }

        void SetSplatmap(std::string splatmap);
        void SetSplatmaps(std::vector<std::string> splatmaps);
        const std::string* GetSplatmap(uint32 index = 0) const;

        void SetTriplanarEnabled(bool enabled) { m_triplanarEnabled = enabled; }
        bool IsTriplanarEnabled() const { return m_triplanarEnabled; }

        void SetTriplanarSharpness(float sharpness) { m_triplanarSharpness = sharpness; }
        float GetTriplanarSharpness() const { return m_triplanarSharpness; }

        void SetHeightBlendEnabled(bool enabled) { m_heightBlendEnabled = enabled; }
        bool IsHeightBlendEnabled() const { return m_heightBlendEnabled; }

        void SetHeightBlendSharpness(float sharpness) { m_heightBlendSharpness = sharpness; }
        float GetHeightBlendSharpness() const { return m_heightBlendSharpness; }

        void BuildLayerRenderData(std::vector<TerrainLayerRenderData>& outData) const;

        bool IsGPUInitialized() const { return false; }
        bool IsLayerBufferDataUploaded() const { return m_layerBufferDataUploaded; }
        const std::string& GetLayerBufferDiagnostic() const { return m_layerBufferDiagnostic; }

    private:
        void MarkLayerDataDirty(const char* reason);

        std::vector<TerrainLayer> m_layers;
        std::vector<std::string> m_splatmaps;

        bool m_triplanarEnabled = false;
        float m_triplanarSharpness = 1.0f;
        bool m_heightBlendEnabled = true;
        float m_heightBlendSharpness = 0.5f;

        bool m_needsUpdate = true;
        bool m_layerBufferDataUploaded = false;
        std::string m_layerBufferDiagnostic =
            "Terrain material layer constants have not been exported to Render-owned buffers.";
    };

} // namespace RVX
