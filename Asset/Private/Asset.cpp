#include "Asset/Asset.h"
#include <functional>

namespace RVX::Asset
{

Asset::Asset() = default;
Asset::~Asset() = default;

std::vector<AssetId> Asset::GetAllDependencies() const
{
    auto required = GetRequiredDependencies();
    auto optional = GetOptionalDependencies();
    required.insert(required.end(), optional.begin(), optional.end());
    return required;
}

void Asset::SetState(AssetState state)
{
    m_state.store(state, std::memory_order_release);
}

void Asset::NotifyLoaded()
{
    SetState(AssetState::Loaded);
    if (m_onLoaded)
    {
        m_onLoaded(this);
    }
}

void Asset::NotifyUnloaded()
{
    SetState(AssetState::Unloaded);
    if (m_onUnloaded)
    {
        m_onUnloaded(this);
    }
}

AssetId GenerateAssetId(const std::string& path)
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

const char* GetAssetTypeName(AssetType type)
{
    switch (type)
    {
        case AssetType::Unknown:    return "Unknown";
        case AssetType::Mesh:       return "Mesh";
        case AssetType::Texture:    return "Texture";
        case AssetType::Material:   return "Material";
        case AssetType::Shader:     return "Shader";
        case AssetType::Skeleton:   return "Skeleton";
        case AssetType::Animation:  return "Animation";
        case AssetType::Audio:      return "Audio";
        case AssetType::Scene:      return "Scene";
        case AssetType::Model:      return "Model";
        case AssetType::Prefab:     return "Prefab";
        case AssetType::Script:     return "Script";
        default:                    return "Custom";
    }
}

} // namespace RVX::Asset
