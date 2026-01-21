#include "Asset/AssetRegistry.h"
#include <fstream>

namespace RVX::Asset
{

AssetRegistry::AssetRegistry() = default;
AssetRegistry::~AssetRegistry() = default;

void AssetRegistry::Register(const AssetMetadata& metadata)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries[metadata.id] = metadata;
    m_pathToId[metadata.path] = metadata.id;
}

void AssetRegistry::Unregister(AssetId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(id);
    if (it != m_entries.end())
    {
        m_pathToId.erase(it->second.path);
        m_entries.erase(it);
    }
}

void AssetRegistry::Update(const AssetMetadata& metadata)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Remove old path mapping if path changed
    auto it = m_entries.find(metadata.id);
    if (it != m_entries.end() && it->second.path != metadata.path)
    {
        m_pathToId.erase(it->second.path);
    }

    m_entries[metadata.id] = metadata;
    m_pathToId[metadata.path] = metadata.id;
}

std::optional<AssetMetadata> AssetRegistry::FindById(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_entries.find(id);
    if (it != m_entries.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<AssetMetadata> AssetRegistry::FindByPath(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pathToId.find(path);
    if (it != m_pathToId.end())
    {
        auto entryIt = m_entries.find(it->second);
        if (entryIt != m_entries.end())
        {
            return entryIt->second;
        }
    }
    return std::nullopt;
}

bool AssetRegistry::Contains(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.find(id) != m_entries.end();
}

bool AssetRegistry::ContainsPath(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pathToId.find(path) != m_pathToId.end();
}

AssetId AssetRegistry::GetIdByPath(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pathToId.find(path);
    return it != m_pathToId.end() ? it->second : InvalidAssetId;
}

std::vector<AssetId> AssetRegistry::GetAllIds() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<AssetId> ids;
    ids.reserve(m_entries.size());
    for (const auto& [id, _] : m_entries)
    {
        ids.push_back(id);
    }
    return ids;
}

std::vector<AssetId> AssetRegistry::GetIdsByType(AssetType type) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<AssetId> ids;
    for (const auto& [id, metadata] : m_entries)
    {
        if (metadata.type == type)
        {
            ids.push_back(id);
        }
    }
    return ids;
}

size_t AssetRegistry::GetCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

bool AssetRegistry::SaveToFile(const std::string& path) const
{
    // TODO: Implement serialization
    (void)path;
    return false;
}

bool AssetRegistry::LoadFromFile(const std::string& path)
{
    // TODO: Implement deserialization
    (void)path;
    return false;
}

void AssetRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_pathToId.clear();
}

} // namespace RVX::Asset
