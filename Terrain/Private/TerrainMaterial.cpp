/**
 * @file TerrainMaterial.cpp
 * @brief Implementation of terrain material system
 */

#include "Terrain/TerrainMaterial.h"
#include "RHI/RHIDevice.h"
#include "Core/Log.h"

namespace RVX
{

uint32 TerrainMaterial::AddLayer(const std::string& name, RHITextureRef albedo,
                                  RHITextureRef normal, float tilingScale)
{
    if (m_layers.size() >= RVX_TERRAIN_MAX_LAYERS)
    {
        RVX_CORE_WARN("TerrainMaterial: Maximum layer count reached ({})", RVX_TERRAIN_MAX_LAYERS);
        return RVX_INVALID_INDEX;
    }

    TerrainLayer layer;
    layer.name = name;
    layer.albedoTexture = std::move(albedo);
    layer.normalTexture = std::move(normal);
    layer.tilingScale = tilingScale;

    uint32 index = static_cast<uint32>(m_layers.size());
    m_layers.push_back(std::move(layer));
    m_needsUpdate = true;

    RVX_CORE_INFO("TerrainMaterial: Added layer '{}' at index {}", name, index);
    return index;
}

TerrainLayer* TerrainMaterial::GetLayer(uint32 index)
{
    if (index >= m_layers.size()) return nullptr;
    return &m_layers[index];
}

const TerrainLayer* TerrainMaterial::GetLayer(uint32 index) const
{
    if (index >= m_layers.size()) return nullptr;
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
    if (index >= m_layers.size()) return;

    m_layers.erase(m_layers.begin() + index);
    m_needsUpdate = true;

    RVX_CORE_INFO("TerrainMaterial: Removed layer at index {}", index);
}

void TerrainMaterial::SetSplatmap(RHITextureRef splatmap)
{
    m_splatmaps.clear();
    if (splatmap)
    {
        m_splatmaps.push_back(std::move(splatmap));
    }
}

void TerrainMaterial::SetSplatmaps(const std::vector<RHITextureRef>& splatmaps)
{
    m_splatmaps = splatmaps;
}

RHITexture* TerrainMaterial::GetSplatmap(uint32 index) const
{
    if (index >= m_splatmaps.size()) return nullptr;
    return m_splatmaps[index].Get();
}

bool TerrainMaterial::InitializeGPU(IRHIDevice* device)
{
    if (!device)
    {
        RVX_CORE_ERROR("TerrainMaterial: Invalid device");
        return false;
    }

    // Create layer data buffer
    RHIBufferDesc bufferDesc;
    bufferDesc.size = RVX_TERRAIN_MAX_LAYERS * sizeof(TerrainLayerGPUData);
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.debugName = "TerrainLayerData";

    m_layerBuffer = device->CreateBuffer(bufferDesc);
    if (!m_layerBuffer)
    {
        RVX_CORE_ERROR("TerrainMaterial: Failed to create layer buffer");
        return false;
    }

    m_gpuInitialized = true;
    UpdateGPUData();

    RVX_CORE_INFO("TerrainMaterial: GPU resources initialized with {} layers", m_layers.size());
    return true;
}

void TerrainMaterial::UpdateGPUData()
{
    if (!m_gpuInitialized || !m_needsUpdate) return;

    std::vector<TerrainLayerGPUData> gpuData(RVX_TERRAIN_MAX_LAYERS);

    for (size_t i = 0; i < m_layers.size(); ++i)
    {
        const auto& layer = m_layers[i];
        gpuData[i].tilingAndStrength = Vec4(
            layer.tilingScale,
            layer.normalStrength,
            layer.roughnessValue,
            layer.metallicValue
        );
        gpuData[i].tintColor = Vec4(layer.tintColor, 1.0f);
    }

    // Upload to GPU buffer
    // Note: Actual upload would be done through upload buffer or map
    // This is a simplified placeholder

    m_needsUpdate = false;
}

} // namespace RVX
