#include "Resource/Types/EnvironmentResource.h"

#include <utility>

namespace RVX::Resource
{

bool EnvironmentResourceData::IsStructurallyValid() const noexcept
{
    return environment.IsValid() && irradiance.IsValid() &&
           prefiltered.IsValid() && brdfLUT.IsValid() &&
           environment.GetId() != InvalidResourceId &&
           irradiance.GetId() != InvalidResourceId &&
           prefiltered.GetId() != InvalidResourceId &&
           brdfLUT.GetId() != InvalidResourceId &&
           environmentResolution != 0 && irradianceResolution != 0 &&
           prefilteredResolution != 0 && prefilteredMipLevels != 0 &&
           brdfLUTResolution != 0 && intensity > 0.0f;
}

bool EnvironmentResourceData::IsValid() const noexcept
{
    return IsStructurallyValid() && environment.IsLoaded() &&
           irradiance.IsLoaded() && prefiltered.IsLoaded() && brdfLUT.IsLoaded();
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

bool EnvironmentResource::SetPreparedData(EnvironmentResourceData data)
{
    if (!data.IsStructurallyValid() || IsLoaded())
    {
        return false;
    }
    m_data = std::move(data);
    return true;
}

} // namespace RVX::Resource
