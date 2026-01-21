#include "Resource/IResource.h"
#include <functional>

namespace RVX::Resource
{

IResource::IResource() = default;
IResource::~IResource() = default;

std::vector<ResourceId> IResource::GetAllDependencies() const
{
    auto required = GetRequiredDependencies();
    auto optional = GetOptionalDependencies();
    required.insert(required.end(), optional.begin(), optional.end());
    return required;
}

void IResource::SetState(ResourceState state)
{
    m_state.store(state, std::memory_order_release);
}

void IResource::NotifyLoaded()
{
    SetState(ResourceState::Loaded);
    if (m_onLoaded)
    {
        m_onLoaded(this);
    }
}

void IResource::NotifyUnloaded()
{
    SetState(ResourceState::Unloaded);
    if (m_onUnloaded)
    {
        m_onUnloaded(this);
    }
}

ResourceId GenerateResourceId(const std::string& path)
{
    // FNV-1a hash
    constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME = 1099511628211ULL;

    uint64_t hash = FNV_OFFSET_BASIS;
    for (char c : path)
    {
        hash ^= static_cast<uint64_t>(c);
        hash *= FNV_PRIME;
    }

    return hash;
}

const char* GetResourceTypeName(ResourceType type)
{
    switch (type)
    {
        case ResourceType::Unknown:    return "Unknown";
        case ResourceType::Mesh:       return "Mesh";
        case ResourceType::Texture:    return "Texture";
        case ResourceType::Material:   return "Material";
        case ResourceType::Shader:     return "Shader";
        case ResourceType::Skeleton:   return "Skeleton";
        case ResourceType::Animation:  return "Animation";
        case ResourceType::Audio:      return "Audio";
        case ResourceType::Scene:      return "Scene";
        case ResourceType::Model:      return "Model";
        case ResourceType::Prefab:     return "Prefab";
        case ResourceType::Script:     return "Script";
        default:                       return "Custom";
    }
}

} // namespace RVX::Resource
