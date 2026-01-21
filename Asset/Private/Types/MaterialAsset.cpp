#include "Asset/Types/MaterialAsset.h"

namespace RVX::Asset
{

MaterialAsset::MaterialAsset() = default;
MaterialAsset::~MaterialAsset() = default;

void MaterialAsset::SetMaterialData(std::shared_ptr<Material> material)
{
    m_material = std::move(material);
}

const std::string& MaterialAsset::GetMaterialName() const
{
    static std::string empty;
    return m_material ? m_material->GetName() : empty;
}

MaterialWorkflow MaterialAsset::GetWorkflow() const
{
    return m_material ? m_material->GetWorkflow() : MaterialWorkflow::MetallicRoughness;
}

void MaterialAsset::SetTexture(const std::string& slot, AssetHandle<TextureAsset> texture)
{
    m_textures[slot] = std::move(texture);
}

AssetHandle<TextureAsset> MaterialAsset::GetTexture(const std::string& slot) const
{
    auto it = m_textures.find(slot);
    return it != m_textures.end() ? it->second : AssetHandle<TextureAsset>();
}

const std::unordered_map<std::string, AssetHandle<TextureAsset>>& MaterialAsset::GetTextures() const
{
    return m_textures;
}

size_t MaterialAsset::GetMemoryUsage() const
{
    size_t size = sizeof(*this);
    
    if (m_material)
    {
        size += sizeof(Material);
    }
    
    return size;
}

std::vector<AssetId> MaterialAsset::GetRequiredDependencies() const
{
    std::vector<AssetId> deps;
    
    for (const auto& [slot, texture] : m_textures)
    {
        if (texture.IsValid())
        {
            deps.push_back(texture.GetId());
        }
    }
    
    return deps;
}

} // namespace RVX::Asset
