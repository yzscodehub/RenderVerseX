#include "Scene/Material.h"

namespace RVX
{
    Material::Material(const std::string& name)
        : m_name(name)
    {
    }

    bool Material::IsTransparent() const
    {
        return m_alphaMode == AlphaMode::Blend || m_baseColor.a < 1.0f;
    }

    Vec4 Material::GetEffectiveBaseColor() const
    {
        return m_baseColor;
    }

    Material::Ptr Material::Clone(bool generateNewId) const
    {
        auto clone = std::make_shared<Material>(m_name);
        
        if (!generateNewId)
        {
            clone->m_materialId = m_materialId;
        }
        
        clone->m_workflow = m_workflow;
        clone->m_doubleSided = m_doubleSided;
        clone->m_alphaMode = m_alphaMode;
        clone->m_alphaCutoff = m_alphaCutoff;
        clone->m_baseColor = m_baseColor;
        clone->m_baseColorTexture = m_baseColorTexture;
        clone->m_metallicFactor = m_metallicFactor;
        clone->m_roughnessFactor = m_roughnessFactor;
        clone->m_metallicRoughnessTexture = m_metallicRoughnessTexture;
        clone->m_normalTexture = m_normalTexture;
        clone->m_normalScale = m_normalScale;
        clone->m_occlusionTexture = m_occlusionTexture;
        clone->m_occlusionStrength = m_occlusionStrength;
        clone->m_emissiveColor = m_emissiveColor;
        clone->m_emissiveTexture = m_emissiveTexture;
        clone->m_emissiveStrength = m_emissiveStrength;
        
        return clone;
    }

} // namespace RVX
