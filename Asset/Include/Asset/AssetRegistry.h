#pragma once

/**
 * @file AssetRegistry.h
 * @brief Asset metadata registry
 */

#include "Asset/Asset.h"
#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>

namespace RVX::Asset
{
    /**
     * @brief Asset metadata entry
     */
    struct AssetMetadata
    {
        AssetId id = InvalidAssetId;
        std::string path;
        std::string name;
        AssetType type = AssetType::Unknown;
        size_t fileSize = 0;
        uint64_t lastModified = 0;
        std::vector<AssetId> dependencies;
    };

    /**
     * @brief Registry for asset metadata
     * 
     * Maintains a database of all known assets and their metadata.
     * Does not store the actual asset data - that's handled by AssetCache.
     */
    class AssetRegistry
    {
    public:
        AssetRegistry();
        ~AssetRegistry();

        // =====================================================================
        // Registration
        // =====================================================================

        /// Register an asset
        void Register(const AssetMetadata& metadata);

        /// Unregister an asset
        void Unregister(AssetId id);

        /// Update asset metadata
        void Update(const AssetMetadata& metadata);

        // =====================================================================
        // Lookup
        // =====================================================================

        /// Find asset by ID
        std::optional<AssetMetadata> FindById(AssetId id) const;

        /// Find asset by path
        std::optional<AssetMetadata> FindByPath(const std::string& path) const;

        /// Check if asset exists
        bool Contains(AssetId id) const;
        bool ContainsPath(const std::string& path) const;

        /// Get asset ID from path
        AssetId GetIdByPath(const std::string& path) const;

        // =====================================================================
        // Enumeration
        // =====================================================================

        /// Get all asset IDs
        std::vector<AssetId> GetAllIds() const;

        /// Get all assets of a specific type
        std::vector<AssetId> GetIdsByType(AssetType type) const;

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
        std::unordered_map<AssetId, AssetMetadata> m_entries;
        std::unordered_map<std::string, AssetId> m_pathToId;
    };

} // namespace RVX::Asset
