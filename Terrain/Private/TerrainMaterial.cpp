/**
 * @file TerrainMaterial.cpp
 * @brief Implementation of terrain material state.
 */

#include "Terrain/TerrainMaterial.h"
#include "Core/Log.h"

#include <utility>

namespace RVX
{

void TerrainMaterial::MarkLayerDataDirty(const char* reason)
{
    m_needsUpdate = true;
    m_layerBufferDataUploaded = false;
    m_layerBufferDiagnostic = reason ? reason :
        "Terrain material layer constants changed; Render-owned buffers need refresh.";
}

uint32 TerrainMaterial::AddLayer(const std::string& name,
                                 std::string albedoTexture,
                                 std::string normalTexture,
                                 float tilingScale)
{
    if (m_layers.size() >= RVX_TERRAIN_MAX_LAYERS)
    {
        RVX_CORE_WARN("TerrainMaterial: Maximum layer count reached ({})", RVX_TERRAIN_MAX_LAYERS);
        return RVX_INVALID_INDEX;
    }

    TerrainLayer layer;
    layer.name = name;
    layer.albedoTexture = std::move(albedoTexture);
    layer.normalTexture = std::move(normalTexture);
    layer.tilingScale = tilingScale;

    const uint32 index = static_cast<uint32>(m_layers.size());
    m_layers.push_back(std::move(layer));
    MarkLayerDataDirty("Terrain layer data has pending Render-owned buffer upload.");

    RVX_CORE_INFO("TerrainMaterial: Added layer '{}' at index {}", name, index);
    return index;
}

TerrainLayer* TerrainMaterial::GetLayer(uint32 index)
{
    if (index >= m_layers.size())
        return nullptr;

    return &m_layers[index];
}

const TerrainLayer* TerrainMaterial::GetLayer(uint32 index) const
{
    if (index >= m_layers.size())
        return nullptr;

    return &m_layers[index];
}

TerrainLayer* TerrainMaterial::GetLayerByName(const std::string& name)
{
    for (auto& layer : m_layers)
    {
        if (layer.name == name)
            return &layer;
    }

    return nullptr;
}

void TerrainMaterial::RemoveLayer(uint32 index)
{
    if (index >= m_layers.size())
        return;

    m_layers.erase(m_layers.begin() + index);
    MarkLayerDataDirty("Terrain layer data has pending Render-owned buffer upload.");

    RVX_CORE_INFO("TerrainMaterial: Removed layer at index {}", index);
}

void TerrainMaterial::SetSplatmap(std::string splatmap)
{
    m_splatmaps.clear();
    if (!splatmap.empty())
    {
        m_splatmaps.push_back(std::move(splatmap));
    }
}

void TerrainMaterial::SetSplatmaps(std::vector<std::string> splatmaps)
{
    m_splatmaps = std::move(splatmaps);
}

const std::string* TerrainMaterial::GetSplatmap(uint32 index) const
{
    if (index >= m_splatmaps.size())
        return nullptr;

    return &m_splatmaps[index];
}

void TerrainMaterial::BuildLayerRenderData(std::vector<TerrainLayerRenderData>& outData) const
{
    outData.assign(RVX_TERRAIN_MAX_LAYERS, {});

    for (size_t i = 0; i < m_layers.size(); ++i)
    {
        const auto& layer = m_layers[i];
        outData[i].tilingAndStrength = Vec4(
            layer.tilingScale,
            layer.normalStrength,
            layer.roughnessValue,
            layer.metallicValue
        );
        outData[i].tintColor = Vec4(layer.tintColor, 1.0f);
    }
}

} // namespace RVX
