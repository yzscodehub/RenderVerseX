#pragma once

/**
 * @file SampleAssetCatalog.h
 * @brief Strict, deterministic asset catalog used by sample applications.
 */

#include "Core/Types.h"
#include "Resource/CookManifest.h"
#include "Resource/ResourceContentIdentity.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace RVX
{
    inline constexpr const char* RVX_SAMPLE_ASSET_CATALOG_SCHEMA_ID =
        "RVX.SampleAssetCatalog";
    inline constexpr uint32 RVX_SAMPLE_ASSET_CATALOG_SCHEMA_VERSION = 3;
    inline constexpr uint32 RVX_SAMPLE_ASSET_CONTENT_ID_SCHEMA_VERSION = 1;

    enum class SampleAssetKind : uint8
    {
        Model = 0,
        Texture,
        Environment,
        Animation
    };

    struct SampleAssetLicense
    {
        std::string spdxId;
        std::filesystem::path file;
        std::filesystem::path resolvedFile;
        std::string coverage;
    };

    struct SampleAssetSource
    {
        std::string name;
        std::string uri;
        std::string author;
        std::string version;
        std::string archiveSha256;
    };

    /**
     * @brief Catalog-owned identity for the complete declared asset package.
     *
     * This is intentionally distinct from ResourceContentIdentity: it covers
     * every file record in the catalog package rather than loader-consumed
     * source bytes.
     */
    struct AssetContentId
    {
        uint32 schemaVersion = 0;
        std::string algorithm;
        std::string digest;
        uint64 byteCount = 0;
        uint32 fileCount = 0;

        [[nodiscard]] bool IsEmpty() const noexcept;
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One independently hashed file in a schema-v3 asset package. */
    struct SampleAssetFile
    {
        std::filesystem::path path;
        std::filesystem::path resolvedPath;
        std::string purpose;
        uint64 byteCount = 0;
        std::string sha256;
    };

    /** @brief Reproducibility metadata required for a declared derived asset. */
    struct SampleAssetGeneration
    {
        std::string recipe;
        std::string toolVersion;
        std::string recipeHash;
    };

    /**
     * @brief Catalog declaration required to admit a cooked qualification asset.
     *
     * The source root is always the catalog's verified asset root.  All other
     * paths are catalog-relative and are re-resolved at every admission so a
     * post-load symlink swap cannot redirect Resource's mounted verification.
     */
    struct SampleAssetCook
    {
        bool declared = false;
        std::filesystem::path manifestPath;
        std::filesystem::path cookedRoot;
        Resource::CookAssetSelector selector;
        Resource::ResourceContentIdentity sourceContentIdentity;
        Resource::ResourceContentIdentity cookedContentIdentity;
        Resource::ResourceContentIdentity manifestContentIdentity;
        std::string cookSettingsHash;
        std::string recipeHash;
        std::string toolName;
        std::string toolVersion;
    };

    enum class SampleCookedAssetAdmissionCode : uint8
    {
        Accepted = 0,
        AssetNotFound,
        CookDataMissing,
        CookDataInvalid,
        ManifestLoadFailed,
        ResourceAdmissionFailed,
    };

    /** @brief Structured result for qualification-only cooked asset admission. */
    struct SampleCookedAssetAdmissionReceipt
    {
        SampleCookedAssetAdmissionCode code =
            SampleCookedAssetAdmissionCode::CookDataInvalid;
        std::string assetId;
        std::string detail;
        Resource::CookManifestAdmissionReceipt resourceReceipt;

        [[nodiscard]] bool IsAccepted() const noexcept
        {
            return code == SampleCookedAssetAdmissionCode::Accepted;
        }
    };

    struct SampleAssetEntry
    {
        std::string id;
        SampleAssetKind kind = SampleAssetKind::Model;
        std::filesystem::path path;
        std::filesystem::path resolvedPath;
        SampleAssetLicense license;
        SampleAssetSource source;
        /**
         * @brief Optional catalog declaration for the exact model source bytes.
         *
         * Schema-v1 catalogs leave this empty. A schema-v2 entry which
         * declares the field must contain a valid Resource identity.
         */
        Resource::ResourceContentIdentity contentIdentity;
        /** @brief Required complete-package identity for schema-v3 entries. */
        AssetContentId assetContentId;
        /** @brief Required, normalized, complete file manifest for schema-v3. */
        std::vector<SampleAssetFile> files;
        std::string attribution;
        std::string modificationNotice;
        std::vector<std::string> derivedFrom;
        SampleAssetGeneration generation;
        /** @brief Optional for browsing; mandatory for qualification admission. */
        SampleAssetCook cook;
        bool redistributable = false;
    };

    const char* GetSampleAssetKindName(SampleAssetKind kind) noexcept;
    const char* GetSampleCookedAssetAdmissionCodeName(
        SampleCookedAssetAdmissionCode code) noexcept;

    /**
     * @brief Loads a complete catalog and validates every path and license.
     *
     * Catalog-relative paths must remain under assetRoot after canonicalization.
     * Schema v3 independently rehashes each declared package file and verifies
     * its AssetContentId. Missing files, unsafe paths, duplicate ids/canonical
     * paths, incomplete provenance, malformed glTF dependencies, or any hash
     * drift fail. Schema v1/v2 remain readable for existing catalogs.
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
        /**
         * @brief Revalidates a cooked asset against its catalog declaration.
         *
         * Legacy schema v1/v2 entries and schema-v3 entries without a cook
         * block remain loadable for development, but fail closed here.  This
         * method is deliberately not cached: it detects manifest, artifact,
         * and mount-path tampering that occurs after Load().
         */
        [[nodiscard]] SampleCookedAssetAdmissionReceipt
        RequireCookedAdmission(std::string_view id) const;
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
