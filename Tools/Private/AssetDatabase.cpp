/**
 * @file AssetDatabase.cpp
 * @brief Asset database implementation
 */

#include "Tools/AssetDatabase.h"
#include "Core/Log.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>

namespace RVX::Tools
{

// ============================================================================
// AssetGUID
// ============================================================================

std::string AssetGUID::ToString() const
{
    std::stringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(16) << high
       << std::setw(16) << low;
    return ss.str();
}

AssetGUID AssetGUID::FromString(const std::string& str)
{
    AssetGUID guid;
    if (str.length() >= 32)
    {
        guid.high = std::stoull(str.substr(0, 16), nullptr, 16);
        guid.low = std::stoull(str.substr(16, 16), nullptr, 16);
    }
    return guid;
}

AssetGUID AssetGUID::Generate()
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64> dist;

    AssetGUID guid;
    guid.high = dist(gen);
    guid.low = dist(gen);
    return guid;
}

// ============================================================================
// AssetDatabase
// ============================================================================

bool AssetDatabase::Initialize(const fs::path& sourceRoot, const fs::path& importedRoot)
{
    m_sourceRoot = sourceRoot;
    m_importedRoot = importedRoot;
    m_databasePath = importedRoot / "AssetDatabase.json";

    // Create directories if needed
    if (!fs::exists(importedRoot))
    {
        fs::create_directories(importedRoot);
    }

    // Try to load existing database
    Load();

    // Scan for changes
    Refresh();

    return true;
}

bool AssetDatabase::Save()
{
    // TODO: Serialize to JSON
    std::ofstream file(m_databasePath);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("Failed to save asset database: {}", m_databasePath.string());
        return false;
    }

    file << "{\n";
    file << "  \"version\": 1,\n";
    file << "  \"assets\": [\n";

    bool first = true;
    for (const auto& [hash, entry] : m_assets)
    {
        if (!first) file << ",\n";
        first = false;

        file << "    {\n";
        file << "      \"guid\": \"" << entry.guid.ToString() << "\",\n";
        file << "      \"path\": \"" << entry.path << "\",\n";
        file << "      \"name\": \"" << entry.name << "\",\n";
        file << "      \"type\": " << static_cast<int>(entry.type) << "\n";
        file << "    }";
    }

    file << "\n  ]\n";
    file << "}\n";

    RVX_CORE_INFO("Asset database saved: {} assets", m_assets.size());
    return true;
}

bool AssetDatabase::Load()
{
    if (!fs::exists(m_databasePath))
    {
        return false;
    }

    // TODO: Parse JSON and load entries
    RVX_CORE_INFO("Asset database loaded from: {}", m_databasePath.string());
    return true;
}

void AssetDatabase::Refresh()
{
    if (!fs::exists(m_sourceRoot))
    {
        return;
    }

    ScanDirectory(m_sourceRoot, m_sourceRoot);
}

void AssetDatabase::ScanDirectory(const fs::path& dir, const fs::path& root)
{
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (entry.is_directory())
        {
            ScanDirectory(entry.path(), root);
        }
        else if (entry.is_regular_file())
        {
            UpdateAssetEntry(entry.path(), root);
        }
    }
}

void AssetDatabase::UpdateAssetEntry(const fs::path& filePath, const fs::path& root)
{
    std::string relativePath = fs::relative(filePath, root).string();
    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');

    // Check if already tracked
    auto it = m_pathToGuid.find(relativePath);
    if (it != m_pathToGuid.end())
    {
        // Check if modified
        auto& entry = m_assets[it->second];
        auto modTime = fs::last_write_time(filePath).time_since_epoch().count();
        if (static_cast<uint64>(modTime) > entry.sourceModTime)
        {
            entry.sourceModTime = modTime;
            entry.isDirty = true;
            if (m_onAssetModified) m_onAssetModified(entry);
        }
    }
    else
    {
        // New asset
        AssetEntry entry;
        entry.guid = AssetGUID::Generate();
        entry.path = relativePath;
        entry.name = filePath.filename().string();
        entry.type = AssetPipeline::GetAssetTypeFromExtension(filePath.extension().string());
        entry.sourceModTime = fs::last_write_time(filePath).time_since_epoch().count();
        entry.isDirty = true;

        uint64 hash = GetGuidHash(entry.guid);
        m_assets[hash] = entry;
        m_pathToGuid[relativePath] = hash;

        if (m_onAssetAdded) m_onAssetAdded(entry);
    }
}

void AssetDatabase::ImportAll(AssetPipeline& pipeline)
{
    for (auto& [hash, entry] : m_assets)
    {
        if (entry.isDirty)
        {
            ImportAsset(entry.guid, pipeline);
        }
    }
    Save();
}

bool AssetDatabase::ImportAsset(const AssetGUID& guid, AssetPipeline& pipeline)
{
    const AssetEntry* entry = GetAsset(guid);
    if (!entry) return false;

    fs::path sourcePath = GetSourcePath(*entry);
    fs::path outputPath = GetImportedPath(*entry);

    fs::create_directories(outputPath.parent_path());

    ImportResult result = pipeline.ImportAsset(sourcePath, outputPath);
    if (result.success)
    {
        auto& mutableEntry = m_assets[GetGuidHash(guid)];
        mutableEntry.isDirty = false;
        mutableEntry.importedModTime = fs::last_write_time(outputPath).time_since_epoch().count();
    }

    return result.success;
}

bool AssetDatabase::ReimportAsset(const AssetGUID& guid, AssetPipeline& pipeline)
{
    m_assets[GetGuidHash(guid)].isDirty = true;
    return ImportAsset(guid, pipeline);
}

const AssetEntry* AssetDatabase::GetAsset(const AssetGUID& guid) const
{
    uint64 hash = GetGuidHash(guid);
    auto it = m_assets.find(hash);
    return (it != m_assets.end()) ? &it->second : nullptr;
}

const AssetEntry* AssetDatabase::GetAssetByPath(const std::string& path) const
{
    auto it = m_pathToGuid.find(path);
    if (it != m_pathToGuid.end())
    {
        auto assetIt = m_assets.find(it->second);
        return (assetIt != m_assets.end()) ? &assetIt->second : nullptr;
    }
    return nullptr;
}

std::vector<const AssetEntry*> AssetDatabase::FindAssetsByType(AssetType type) const
{
    std::vector<const AssetEntry*> result;
    for (const auto& [hash, entry] : m_assets)
    {
        if (entry.type == type)
        {
            result.push_back(&entry);
        }
    }
    return result;
}

std::vector<const AssetEntry*> AssetDatabase::FindAssets(const std::string& pattern) const
{
    std::vector<const AssetEntry*> result;
    for (const auto& [hash, entry] : m_assets)
    {
        if (entry.name.find(pattern) != std::string::npos ||
            entry.path.find(pattern) != std::string::npos)
        {
            result.push_back(&entry);
        }
    }
    return result;
}

size_t AssetDatabase::GetDirtyAssetCount() const
{
    size_t count = 0;
    for (const auto& [hash, entry] : m_assets)
    {
        if (entry.isDirty) count++;
    }
    return count;
}

fs::path AssetDatabase::GetSourcePath(const AssetEntry& entry) const
{
    return m_sourceRoot / entry.path;
}

fs::path AssetDatabase::GetImportedPath(const AssetEntry& entry) const
{
    fs::path path = m_importedRoot / entry.path;
    path.replace_extension(".rva");
    return path;
}

uint64 AssetDatabase::GetGuidHash(const AssetGUID& guid) const
{
    return guid.high ^ guid.low;
}

} // namespace RVX::Tools
