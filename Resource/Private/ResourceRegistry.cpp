#include "Resource/ResourceRegistry.h"
#include <fstream>

namespace RVX::Resource
{

ResourceRegistry::ResourceRegistry() = default;
ResourceRegistry::~ResourceRegistry() = default;

void ResourceRegistry::Register(const ResourceMetadata& metadata)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_entries[metadata.id] = metadata;
    m_pathToId[metadata.path] = metadata.id;
}

void ResourceRegistry::Unregister(ResourceId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_entries.find(id);
    if (it != m_entries.end())
    {
        m_pathToId.erase(it->second.path);
        m_entries.erase(it);
    }
}

void ResourceRegistry::Update(const ResourceMetadata& metadata)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_entries.find(metadata.id);
    if (it != m_entries.end())
    {
        // Remove old path mapping if changed
        if (it->second.path != metadata.path)
        {
            m_pathToId.erase(it->second.path);
            m_pathToId[metadata.path] = metadata.id;
        }
        it->second = metadata;
    }
    else
    {
        m_entries[metadata.id] = metadata;
        m_pathToId[metadata.path] = metadata.id;
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

    auto idIt = m_pathToId.find(path);
    if (idIt != m_pathToId.end())
    {
        auto entryIt = m_entries.find(idIt->second);
        if (entryIt != m_entries.end())
        {
            return entryIt->second;
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
    return m_pathToId.find(path) != m_pathToId.end();
}

ResourceId ResourceRegistry::GetIdByPath(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_pathToId.find(path);
    if (it != m_pathToId.end())
    {
        return it->second;
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

    Clear();

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
        m_pathToId[metadata.path] = metadata.id;
    }

    return true;
}

void ResourceRegistry::Clear()
{
    m_entries.clear();
    m_pathToId.clear();
}

} // namespace RVX::Resource
