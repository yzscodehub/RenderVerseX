#include "Resource/Types/MaterialResource.h"

namespace RVX::Resource
{

MaterialResource::MaterialResource() = default;
MaterialResource::~MaterialResource() = default;

void MaterialResource::SetMaterialData(std::shared_ptr<Material> material)
{
    m_material = std::move(material);
}

const std::string& MaterialResource::GetMaterialName() const
{
    static std::string empty;
    return m_material ? m_material->GetName() : empty;
}

MaterialWorkflow MaterialResource::GetWorkflow() const
{
    return m_material ? m_material->GetWorkflow() : MaterialWorkflow::MetallicRoughness;
}

void MaterialResource::SetTexture(const std::string& slot, ResourceHandle<TextureResource> texture)
{
    m_textures[slot] = std::move(texture);
}

ResourceHandle<TextureResource> MaterialResource::GetTexture(const std::string& slot) const
{
    auto it = m_textures.find(slot);
    return it != m_textures.end() ? it->second : ResourceHandle<TextureResource>();
}

const std::unordered_map<std::string, ResourceHandle<TextureResource>>& MaterialResource::GetTextures() const
{
    return m_textures;
}

size_t MaterialResource::GetMemoryUsage() const
{
    size_t size = sizeof(*this);
    
    if (m_material)
    {
        size += sizeof(Material);
    }
    
    return size;
}

std::vector<ResourceId> MaterialResource::GetRequiredDependencies() const
{
    std::vector<ResourceId> deps;
    
    for (const auto& [slot, texture] : m_textures)
    {
        if (texture.IsValid())
        {
            deps.push_back(texture.GetId());
        }
    }
    
    return deps;
}

} // namespace RVX::Resource
