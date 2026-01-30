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

void SkyboxComponent::SetCubemap(Resource::ResourceHandle<Resource::TextureResource> texture)
{
    m_cubemap = texture;
    if (texture.IsValid())
    {
        m_type = SkyboxType::Cubemap;
    }
}

void SkyboxComponent::SetEquirectangular(Resource::ResourceHandle<Resource::TextureResource> texture)
{
    m_equirectangular = texture;
    if (texture.IsValid())
    {
        m_type = SkyboxType::Equirectangular;
    }
}

void SkyboxComponent::SetPrefilteredMap(Resource::ResourceHandle<Resource::TextureResource> texture)
{
    m_prefilteredMap = texture;
}

void SkyboxComponent::SetIrradianceMap(Resource::ResourceHandle<Resource::TextureResource> texture)
{
    m_irradianceMap = texture;
}

void SkyboxComponent::SetBRDFLUT(Resource::ResourceHandle<Resource::TextureResource> texture)
{
    m_brdfLUT = texture;
}

} // namespace RVX
