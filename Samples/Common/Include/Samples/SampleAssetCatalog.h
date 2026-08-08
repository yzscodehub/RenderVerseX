#pragma once

/**
 * @file SampleAssetCatalog.h
 * @brief Strict, deterministic asset catalog used by sample applications.
 */

#include "Core/Types.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace RVX
{
    inline constexpr const char* RVX_SAMPLE_ASSET_CATALOG_SCHEMA_ID =
        "RVX.SampleAssetCatalog";
    inline constexpr uint32 RVX_SAMPLE_ASSET_CATALOG_SCHEMA_VERSION = 1;

    enum class SampleAssetKind : uint8
    {
        Model = 0,
        Texture,
        Environment
    };

    struct SampleAssetLicense
    {
        std::string spdxId;
        std::filesystem::path file;
        std::filesystem::path resolvedFile;
    };

    struct SampleAssetSource
    {
        std::string name;
        std::string uri;
        std::string author;
    };

    struct SampleAssetEntry
    {
        std::string id;
        SampleAssetKind kind = SampleAssetKind::Model;
        std::filesystem::path path;
        std::filesystem::path resolvedPath;
        SampleAssetLicense license;
        SampleAssetSource source;
        std::string attribution;
        bool redistributable = false;
    };

    const char* GetSampleAssetKindName(SampleAssetKind kind) noexcept;

    /**
     * @brief Loads a complete catalog and validates every path and license.
     *
     * Catalog-relative paths must remain under assetRoot after canonicalization.
     * Missing files, traversal, duplicate ids, and incomplete provenance fail.
     */
    class SampleAssetCatalog final
    {
    public:
        bool Load(const std::filesystem::path& catalogPath,
                  const std::filesystem::path& assetRoot,
                  std::string* outError = nullptr);

        [[nodiscard]] const SampleAssetEntry* Find(
            std::string_view id) const;
        [[nodiscard]] std::vector<SampleAssetEntry> List() const;
        [[nodiscard]] const std::filesystem::path& GetCatalogPath() const noexcept
        {
            return m_catalogPath;
        }
        [[nodiscard]] const std::filesystem::path& GetAssetRoot() const noexcept
        {
            return m_assetRoot;
        }

    private:
        std::filesystem::path m_catalogPath;
        std::filesystem::path m_assetRoot;
        std::map<std::string, SampleAssetEntry, std::less<>> m_entries;
    };
} // namespace RVX
