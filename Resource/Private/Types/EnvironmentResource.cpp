#include "Resource/Types/EnvironmentResource.h"

#include <utility>

namespace RVX::Resource
{

bool EnvironmentResourceData::IsValid() const noexcept
{
    return environment.IsLoaded() && irradiance.IsLoaded() &&
           prefiltered.IsLoaded() && brdfLUT.IsLoaded() &&
           environmentResolution != 0 && irradianceResolution != 0 &&
           prefilteredResolution != 0 && prefilteredMipLevels != 0 &&
           brdfLUTResolution != 0 && intensity > 0.0f;
}

size_t EnvironmentResource::GetMemoryUsage() const
{
    return sizeof(*this) + m_data.sourcePath.native().size() *
                               sizeof(std::filesystem::path::value_type);
}

size_t EnvironmentResource::GetGPUMemoryUsage() const
{
    size_t bytes = 0;
    const TextureHandle textures[] = {
        m_data.environment,
        m_data.irradiance,
        m_data.prefiltered,
        m_data.brdfLUT};
    for (const TextureHandle& texture : textures)
    {
        if (texture.IsValid())
            bytes += texture->GetGPUMemoryUsage();
    }
    return bytes;
}

std::vector<ResourceId> EnvironmentResource::GetRequiredDependencies() const
{
    std::vector<ResourceId> dependencies;
    dependencies.reserve(4);
    const TextureHandle textures[] = {
        m_data.environment,
        m_data.irradiance,
        m_data.prefiltered,
        m_data.brdfLUT};
    for (const TextureHandle& texture : textures)
    {
        if (texture.IsValid())
            dependencies.push_back(texture->GetId());
    }
    return dependencies;
}

bool EnvironmentResource::SetData(EnvironmentResourceData data)
{
    if (!data.IsValid())
    {
        SetState(ResourceState::Failed);
        return false;
    }
    m_data = std::move(data);
    SetState(ResourceState::Loaded);
    return true;
}

} // namespace RVX::Resource
