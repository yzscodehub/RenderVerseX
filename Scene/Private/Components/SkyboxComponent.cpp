#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"

namespace RVX
{

void SkyboxComponent::OnAttach()
{
    // Nothing special needed
}

void SkyboxComponent::OnDetach()
{
    // Nothing special needed
}

void SkyboxComponent::SetCubemap(SceneTextureHandle texture)
{
    m_cubemap = texture;
    if (texture.IsValid())
    {
        m_type = SkyboxType::Cubemap;
    }
}

void SkyboxComponent::SetEquirectangular(SceneTextureHandle texture)
{
    m_equirectangular = texture;
    if (texture.IsValid())
    {
        m_type = SkyboxType::Equirectangular;
    }
}

void SkyboxComponent::SetPrefilteredMap(SceneTextureHandle texture)
{
    m_prefilteredMap = texture;
}

void SkyboxComponent::SetIrradianceMap(SceneTextureHandle texture)
{
    m_irradianceMap = texture;
}

void SkyboxComponent::SetBRDFLUT(SceneTextureHandle texture)
{
    m_brdfLUT = texture;
}

} // namespace RVX
