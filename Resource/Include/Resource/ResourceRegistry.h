#pragma once

/**
 * @file ResourceRegistry.h
 * @brief Resource metadata registry
 */

#include "Resource/IResource.h"
#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>

namespace RVX::Resource
{
    /**
     * @brief Resource metadata entry
     */
    struct ResourceMetadata
    {
        ResourceId id = InvalidResourceId;
        std::string path;
        std::string name;
        ResourceType type = ResourceType::Unknown;
        size_t fileSize = 0;
        uint64_t lastModified = 0;
        std::vector<ResourceId> dependencies;
    };

    /**
     * @brief Registry for resource metadata
     * 
     * Maintains a database of all known resources and their metadata.
     * Does not store the actual resource data - that's handled by ResourceCache.
     */
    class ResourceRegistry
    {
    public:
        ResourceRegistry();
        ~ResourceRegistry();

        // =====================================================================
        // Registration
        // =====================================================================

        /// Register a resource
        void Register(const ResourceMetadata& metadata);

        /// Unregister a resource
        void Unregister(ResourceId id);

        /// Update resource metadata
        void Update(const ResourceMetadata& metadata);

        // =====================================================================
        // Lookup
        // =====================================================================

        /// Find resource by ID
        std::optional<ResourceMetadata> FindById(ResourceId id) const;

        /// Find resource by path
        std::optional<ResourceMetadata> FindByPath(const std::string& path) const;

        /// Check if resource exists
        bool Contains(ResourceId id) const;
        bool ContainsPath(const std::string& path) const;

        /// Get resource ID from path
        ResourceId GetIdByPath(const std::string& path) const;

        // =====================================================================
        // Enumeration
        // =====================================================================

        /// Get all resource IDs
        std::vector<ResourceId> GetAllIds() const;

        /// Get all resources of a specific type
        std::vector<ResourceId> GetIdsByType(ResourceType type) const;

        /// Get total count
        size_t GetCount() const;

        // =====================================================================
        // Persistence
        // =====================================================================

        /// Save registry to file
        bool SaveToFile(const std::string& path) const;

        /// Load registry from file
        bool LoadFromFile(const std::string& path);

        /// Clear all entries
        void Clear();

    private:
        mutable std::mutex m_mutex;
        std::unordered_map<ResourceId, ResourceMetadata> m_entries;
        std::unordered_map<std::string, ResourceId> m_pathToId;
    };

} // namespace RVX::Resource
