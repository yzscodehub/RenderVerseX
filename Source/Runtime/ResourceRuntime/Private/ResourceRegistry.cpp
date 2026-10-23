#include "Resource/ResourceRegistry.h"
#include <algorithm>
#include <fstream>

namespace RVX::Resource
{

ResourceRegistry::ResourceRegistry() = default;
ResourceRegistry::~ResourceRegistry() = default;

void ResourceRegistry::Register(const ResourceMetadata& metadata)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const auto existing = m_entries.find(metadata.id);
    if (existing != m_entries.end())
    {
        RemovePathMappingLocked(existing->second.path, metadata.id);
    }
    m_entries[metadata.id] = metadata;
    AddPathMappingLocked(metadata.path, metadata.id);
}

void ResourceRegistry::Unregister(ResourceId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_entries.find(id);
    if (it != m_entries.end())
    {
        RemovePathMappingLocked(it->second.path, id);
        m_entries.erase(it);
    }
}

void ResourceRegistry::Update(const ResourceMetadata& metadata)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_entries.find(metadata.id);
    if (it != m_entries.end())
    {
        RemovePathMappingLocked(it->second.path, metadata.id);
        it->second = metadata;
        AddPathMappingLocked(metadata.path, metadata.id);
    }
    else
    {
        m_entries[metadata.id] = metadata;
        AddPathMappingLocked(metadata.path, metadata.id);
    }
}

std::optional<ResourceMetadata> ResourceRegistry::FindById(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_entries.find(id);
    if (it != m_entries.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<ResourceMetadata> ResourceRegistry::FindByPath(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto idIt = m_pathToIds.find(path);
    if (idIt != m_pathToIds.end())
    {
        for (auto candidate = idIt->second.rbegin(); candidate != idIt->second.rend(); ++candidate)
        {
            auto entryIt = m_entries.find(*candidate);
            if (entryIt != m_entries.end())
            {
                return entryIt->second;
            }
        }
    }
    return std::nullopt;
}

bool ResourceRegistry::Contains(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.find(id) != m_entries.end();
}

bool ResourceRegistry::ContainsPath(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_pathToIds.find(path);
    return it != m_pathToIds.end() && !it->second.empty();
}

ResourceId ResourceRegistry::GetIdByPath(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_pathToIds.find(path);
    if (it != m_pathToIds.end())
    {
        for (auto candidate = it->second.rbegin(); candidate != it->second.rend(); ++candidate)
        {
            if (m_entries.contains(*candidate))
            {
                return *candidate;
            }
        }
    }
    return InvalidResourceId;
}

std::vector<ResourceId> ResourceRegistry::GetAllIds() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<ResourceId> ids;
    ids.reserve(m_entries.size());

    for (const auto& [id, metadata] : m_entries)
    {
        ids.push_back(id);
    }

    return ids;
}

std::vector<ResourceId> ResourceRegistry::GetIdsByType(ResourceType type) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<ResourceId> ids;

    for (const auto& [id, metadata] : m_entries)
    {
        if (metadata.type == type)
        {
            ids.push_back(id);
        }
    }

    return ids;
}

size_t ResourceRegistry::GetCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

bool ResourceRegistry::SaveToFile(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    // Simple binary format
    size_t count = m_entries.size();
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& [id, metadata] : m_entries)
    {
        file.write(reinterpret_cast<const char*>(&metadata.id), sizeof(metadata.id));
        
        size_t pathLen = metadata.path.size();
        file.write(reinterpret_cast<const char*>(&pathLen), sizeof(pathLen));
        file.write(metadata.path.data(), pathLen);

        size_t nameLen = metadata.name.size();
        file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        file.write(metadata.name.data(), nameLen);

        file.write(reinterpret_cast<const char*>(&metadata.type), sizeof(metadata.type));
        file.write(reinterpret_cast<const char*>(&metadata.fileSize), sizeof(metadata.fileSize));
        file.write(reinterpret_cast<const char*>(&metadata.lastModified), sizeof(metadata.lastModified));

        size_t depCount = metadata.dependencies.size();
        file.write(reinterpret_cast<const char*>(&depCount), sizeof(depCount));
        for (ResourceId dep : metadata.dependencies)
        {
            file.write(reinterpret_cast<const char*>(&dep), sizeof(dep));
        }
    }

    return true;
}

bool ResourceRegistry::LoadFromFile(const std::string& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    m_entries.clear();
    m_pathToIds.clear();

    size_t count = 0;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    for (size_t i = 0; i < count; ++i)
    {
        ResourceMetadata metadata;

        file.read(reinterpret_cast<char*>(&metadata.id), sizeof(metadata.id));

        size_t pathLen = 0;
        file.read(reinterpret_cast<char*>(&pathLen), sizeof(pathLen));
        metadata.path.resize(pathLen);
        file.read(metadata.path.data(), pathLen);

        size_t nameLen = 0;
        file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        metadata.name.resize(nameLen);
        file.read(metadata.name.data(), nameLen);

        file.read(reinterpret_cast<char*>(&metadata.type), sizeof(metadata.type));
        file.read(reinterpret_cast<char*>(&metadata.fileSize), sizeof(metadata.fileSize));
        file.read(reinterpret_cast<char*>(&metadata.lastModified), sizeof(metadata.lastModified));

        size_t depCount = 0;
        file.read(reinterpret_cast<char*>(&depCount), sizeof(depCount));
        metadata.dependencies.resize(depCount);
        for (size_t j = 0; j < depCount; ++j)
        {
            file.read(reinterpret_cast<char*>(&metadata.dependencies[j]), sizeof(ResourceId));
        }

        m_entries[metadata.id] = metadata;
        AddPathMappingLocked(metadata.path, metadata.id);
    }

    return true;
}

void ResourceRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_pathToIds.clear();
}

void ResourceRegistry::RemovePathMappingLocked(const std::string& path, ResourceId id)
{
    auto it = m_pathToIds.find(path);
    if (it == m_pathToIds.end())
    {
        return;
    }

    std::erase(it->second, id);
    if (it->second.empty())
    {
        m_pathToIds.erase(it);
    }
}

void ResourceRegistry::AddPathMappingLocked(const std::string& path, ResourceId id)
{
    std::vector<ResourceId>& ids = m_pathToIds[path];
    std::erase(ids, id);
    ids.push_back(id);
}

} // namespace RVX::Resource
