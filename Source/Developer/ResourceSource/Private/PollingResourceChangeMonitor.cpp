/** @file PollingResourceChangeMonitor.cpp @brief Portable source file monitor */

#include "ResourceSource/PollingResourceChangeMonitor.h"

#include <stdexcept>
#include <system_error>
#include <utility>

namespace RVX::ResourceSource
{
namespace
{
    [[nodiscard]] std::string NormalizePath(const std::string& path)
    {
        if (path.empty())
        {
            throw std::invalid_argument("Source change monitor path is empty");
        }
        std::error_code error;
        std::filesystem::path absolute = std::filesystem::absolute(path, error);
        if (error)
        {
            throw std::invalid_argument(
                "Source change monitor path cannot be resolved: " +
                error.message());
        }
        return absolute.lexically_normal().string();
    }

    void ReadFileState(
        const std::string& path,
        bool& outExists,
        std::filesystem::file_time_type& outLastWriteTime)
    {
        std::error_code error;
        outExists = std::filesystem::is_regular_file(path, error);
        if (error || !outExists)
        {
            outExists = false;
            outLastWriteTime = {};
            return;
        }
        outLastWriteTime = std::filesystem::last_write_time(path, error);
        if (error)
        {
            outExists = false;
            outLastWriteTime = {};
        }
    }
} // namespace

void PollingResourceChangeMonitor::Watch(
    ResourceId resourceId,
    std::string path)
{
    if (resourceId == InvalidResourceId)
    {
        throw std::invalid_argument(
            "Source change monitor resource id is invalid");
    }

    Entry entry;
    entry.path = NormalizePath(path);
    ReadFileState(entry.path, entry.exists, entry.lastWriteTime);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries[resourceId] = std::move(entry);
}

void PollingResourceChangeMonitor::Unwatch(ResourceId resourceId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(resourceId);
}

std::vector<ResourceSourceChange> PollingResourceChangeMonitor::Poll()
{
    std::vector<ResourceSourceChange> changes;
    std::lock_guard<std::mutex> lock(m_mutex);
    changes.reserve(m_entries.size());
    for (auto& [resourceId, entry] : m_entries)
    {
        bool exists = false;
        std::filesystem::file_time_type lastWriteTime{};
        ReadFileState(entry.path, exists, lastWriteTime);
        if (exists == entry.exists &&
            (!exists || lastWriteTime == entry.lastWriteTime))
        {
            continue;
        }

        entry.exists = exists;
        entry.lastWriteTime = lastWriteTime;
        changes.push_back({resourceId, entry.path});
    }
    return changes;
}
} // namespace RVX::ResourceSource
