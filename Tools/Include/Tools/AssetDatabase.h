/**
 * @file AssetDatabase.h
 * @brief Asset database for tracking and managing project assets
 */

#pragma once

#include "Core/Types.h"
#include "Tools/AssetPipeline.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <functional>

namespace RVX::Tools
{

namespace fs = std::filesystem;

/**
 * @brief Unique asset identifier
 */
struct AssetGUID
{
    uint64 high = 0;
    uint64 low = 0;

    bool IsValid() const { return high != 0 || low != 0; }
    bool operator==(const AssetGUID& other) const { return high == other.high && low == other.low; }
    bool operator!=(const AssetGUID& other) const { return !(*this == other); }

    std::string ToString() const;
    static AssetGUID FromString(const std::string& str);
    static AssetGUID Generate();
};

/**
 * @brief Asset metadata entry
 */
struct AssetEntry
{
    AssetGUID guid;
    std::string path;           // Relative path from asset root
    std::string name;
    AssetType type = AssetType::Unknown;
    uint64 sourceModTime = 0;   // Source file modification time
    uint64 importedModTime = 0; // Imported asset modification time
    std::string importerName;
    std::vector<AssetGUID> dependencies;
    bool isDirty = false;
};

/**
 * @brief Asset database manages asset metadata and import state
 */
class AssetDatabase
{
public:
    using AssetChangeCallback = std::function<void(const AssetEntry&)>;

    AssetDatabase() = default;

    // =========================================================================
    // Initialization
    // =========================================================================

    /**
     * @brief Initialize with asset directories
     * @param sourceRoot Source assets directory
     * @param importedRoot Imported/processed assets directory
     */
    bool Initialize(const fs::path& sourceRoot, const fs::path& importedRoot);

    /**
     * @brief Save database to disk
     */
    bool Save();

    /**
     * @brief Load database from disk
     */
    bool Load();

    // =========================================================================
    // Asset Operations
    // =========================================================================

    /**
     * @brief Refresh and scan for changes
     */
    void Refresh();

    /**
     * @brief Import all dirty assets
     */
    void ImportAll(AssetPipeline& pipeline);

    /**
     * @brief Import a specific asset
     */
    bool ImportAsset(const AssetGUID& guid, AssetPipeline& pipeline);

    /**
     * @brief Reimport an asset
     */
    bool ReimportAsset(const AssetGUID& guid, AssetPipeline& pipeline);

    // =========================================================================
    // Queries
    // =========================================================================

    /**
     * @brief Get asset by GUID
     */
    const AssetEntry* GetAsset(const AssetGUID& guid) const;

    /**
     * @brief Get asset by path
     */
    const AssetEntry* GetAssetByPath(const std::string& path) const;

    /**
     * @brief Find assets by type
     */
    std::vector<const AssetEntry*> FindAssetsByType(AssetType type) const;

    /**
     * @brief Find assets matching pattern
     */
    std::vector<const AssetEntry*> FindAssets(const std::string& pattern) const;

    /**
     * @brief Get all assets
     */
    const std::unordered_map<uint64, AssetEntry>& GetAllAssets() const { return m_assets; }

    /**
     * @brief Get dirty asset count
     */
    size_t GetDirtyAssetCount() const;

    // =========================================================================
    // Paths
    // =========================================================================

    const fs::path& GetSourceRoot() const { return m_sourceRoot; }
    const fs::path& GetImportedRoot() const { return m_importedRoot; }

    fs::path GetSourcePath(const AssetEntry& entry) const;
    fs::path GetImportedPath(const AssetEntry& entry) const;

    // =========================================================================
    // Callbacks
    // =========================================================================

    void SetOnAssetAdded(AssetChangeCallback callback) { m_onAssetAdded = std::move(callback); }
    void SetOnAssetModified(AssetChangeCallback callback) { m_onAssetModified = std::move(callback); }
    void SetOnAssetRemoved(AssetChangeCallback callback) { m_onAssetRemoved = std::move(callback); }

private:
    void ScanDirectory(const fs::path& dir, const fs::path& root);
    void UpdateAssetEntry(const fs::path& filePath, const fs::path& root);
    uint64 GetGuidHash(const AssetGUID& guid) const;

    fs::path m_sourceRoot;
    fs::path m_importedRoot;
    fs::path m_databasePath;

    std::unordered_map<uint64, AssetEntry> m_assets;
    std::unordered_map<std::string, uint64> m_pathToGuid;

    AssetChangeCallback m_onAssetAdded;
    AssetChangeCallback m_onAssetModified;
    AssetChangeCallback m_onAssetRemoved;
};

} // namespace RVX::Tools
