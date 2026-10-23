#include "Geometry/Asset/Material.h"

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
        clone->m_specularFactor = m_specularFactor;
        clone->m_specularColor = m_specularColor;
        clone->m_specularTexture = m_specularTexture;
        clone->m_specularColorTexture = m_specularColorTexture;
        clone->m_clearcoatFactor = m_clearcoatFactor;
        clone->m_clearcoatTexture = m_clearcoatTexture;
        clone->m_clearcoatRoughness = m_clearcoatRoughness;
        clone->m_clearcoatRoughnessTexture = m_clearcoatRoughnessTexture;
        clone->m_clearcoatNormalScale = m_clearcoatNormalScale;
        clone->m_clearcoatNormalTexture = m_clearcoatNormalTexture;
        clone->m_transmissionFactor = m_transmissionFactor;
        clone->m_transmissionTexture = m_transmissionTexture;
        clone->m_indexOfRefraction = m_indexOfRefraction;
        clone->m_thicknessFactor = m_thicknessFactor;
        clone->m_thicknessTexture = m_thicknessTexture;
        clone->m_attenuationColor = m_attenuationColor;
        clone->m_attenuationDistance = m_attenuationDistance;

        return clone;
    }

} // namespace RVX
