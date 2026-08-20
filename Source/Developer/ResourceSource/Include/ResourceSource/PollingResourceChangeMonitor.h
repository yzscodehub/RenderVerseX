#pragma once

/** @file PollingResourceChangeMonitor.h @brief Portable source file monitor */

#include "ResourceRuntime/ResourceChangeMonitor.h"

#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace RVX::ResourceSource
{
    class PollingResourceChangeMonitor final
        : public Resource::IResourceChangeMonitor
    {
    public:
        void Watch(ResourceId resourceId, std::string path) override;
        void Unwatch(ResourceId resourceId) override;
        std::vector<ResourceSourceChange> Poll() override;

    private:
        struct Entry
        {
            std::string path;
            std::filesystem::file_time_type lastWriteTime{};
            bool exists = false;
        };

        std::mutex m_mutex;
        std::unordered_map<ResourceId, Entry> m_entries;
    };
} // namespace RVX::ResourceSource
