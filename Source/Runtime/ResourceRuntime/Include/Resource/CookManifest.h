#pragma once

/**
 * @file CookManifest.h
 * @brief Resource-owned admission contract for RVX_COOK_MANIFEST_V2 packages.
 *
 * This deliberately reads the portable v2 wire format without depending on the
 * Tools module.  A manifest's root observations are never authority to access a
 * machine path: callers supply the two mounted package roots at admission time.
 */

#include "Core/Types.h"
#include "Resource/ResourceContentIdentity.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace RVX::Resource
{
    namespace fs = std::filesystem;

    inline constexpr uint32 RVX_COOK_MANIFEST_SCHEMA_VERSION = 2;

    enum class CookAssetType : uint8
    {
        Unknown = 0,
        Texture = 1,
        Mesh = 2,
        Material = 3,
        Shader = 4,
        Animation = 5,
        Audio = 6,
        Font = 7,
        Prefab = 8,
        Scene = 9,
        Script = 10,
        Model = 11,
        Environment = 12,
    };

    /** @brief One canonical relative file record embedded in a v2 manifest. */
    struct CookFileIdentity
    {
        std::string relativePath;
        uint64 byteCount = 0;
        std::string sha256;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const CookFileIdentity& other) const;
    };

    struct CookSemanticMetrics
    {
        uint64 entryCount = 0;
        uint64 successCount = 0;
        uint64 failureCount = 0;
        uint64 dependencyCount = 0;
        uint64 artifactCount = 0;
        uint64 artifactByteCount = 0;

        [[nodiscard]] bool operator==(const CookSemanticMetrics& other) const;
    };

    struct CookManifestEntry
    {
        std::string sourcePath;
        std::string outputPath;
        CookAssetType type = CookAssetType::Unknown;
        bool success = false;
        std::string importerName;
        CookFileIdentity sourceContent;
        /** @brief Source-side external dependency closure (not cooked products). */
        std::vector<CookFileIdentity> sourceDependencies;
        /** @brief False only for a legacy v2 entry without source closure records. */
        bool sourceDependencyClosureRecorded = false;
        std::string canonicalCookSettings;
        std::string cookSettingsHash;
        std::string recipeHash;
        std::vector<std::string> warnings;
        std::vector<CookFileIdentity> dependencies;
        std::vector<CookFileIdentity> artifacts;
        std::string error;
    };

    /** @brief Parsed logical v2 manifest.  Root strings are observations only. */
    struct CookManifest
    {
        static constexpr const char* Header = "RVX_COOK_MANIFEST_V2";
        static constexpr const char* Footer = "RVX_COOK_MANIFEST_END";
        static constexpr const char* SchemaName = "RVX_COOK_MANIFEST";
        static constexpr const char* DefaultToolName = "RVXCook";
        static constexpr const char* DefaultToolVersion = "2.0.0";

        std::string schema;
        uint32 version = 0;
        std::string toolName;
        std::string toolVersion;
        std::string observedSourceRoot;
        std::string observedOutputRoot;
        bool recursive = false;
        CookSemanticMetrics declaredMetrics;
        /** @brief Metrics derived from the parsed/validated entries. */
        CookSemanticMetrics observedMetrics;
        std::vector<CookManifestEntry> entries;
        ResourceContentIdentity manifestContentIdentity;

        [[nodiscard]] bool IsSemanticallyValid(std::string& outError) const;
    };

    struct CookAssetSelector
    {
        std::string sourcePath;
        std::string outputPath;
        CookAssetType type = CookAssetType::Unknown;

        [[nodiscard]] bool IsEmpty() const noexcept;
    };

    /** @brief Expectations supplied by a runtime/catalog entry, never by the manifest. */
    struct CookManifestExpectation
    {
        CookAssetSelector selector;
        std::string requiredToolName = CookManifest::DefaultToolName;
        std::string requiredToolVersion = CookManifest::DefaultToolVersion;
        ResourceContentIdentity expectedSourceContentIdentity;
        ResourceContentIdentity expectedCookedContentIdentity;
        ResourceContentIdentity expectedManifestContentIdentity;
        std::string expectedCookSettingsHash;
        std::string expectedRecipeHash;
    };

    enum class CookManifestAdmissionCode : uint8
    {
        Accepted = 0,
        InvalidManifest,
        ToolMismatch,
        SelectorNotFound,
        SelectorAmbiguous,
        SourceIdentityMismatch,
        CookedIdentityMismatch,
        ManifestIdentityMismatch,
        CookSettingsMismatch,
        RecipeMismatch,
        MountedContentMismatch,
    };

    struct CookManifestAdmissionReceipt
    {
        CookManifestAdmissionCode code = CookManifestAdmissionCode::InvalidManifest;
        std::string detail;
        uint32 selectedEntryIndex = UINT32_MAX;
        ResourceContentIdentity observedSourceContentIdentity;
        ResourceContentIdentity observedCookedContentIdentity;
        ResourceContentIdentity observedManifestContentIdentity;

        [[nodiscard]] bool IsAccepted() const noexcept
        {
            return code == CookManifestAdmissionCode::Accepted;
        }
    };

    [[nodiscard]] const char* GetCookAssetTypeName(CookAssetType type);
    [[nodiscard]] const char* GetCookManifestAdmissionCodeName(CookManifestAdmissionCode code);

    [[nodiscard]] bool ParseCookManifest(std::string_view text,
                                         CookManifest& outManifest,
                                         std::string& outError);
    [[nodiscard]] bool LoadCookManifest(const fs::path& manifestPath,
                                        CookManifest& outManifest,
                                        std::string& outError);
    [[nodiscard]] bool SaveCookManifest(const fs::path& manifestPath,
                                        const CookManifest& manifest,
                                        std::string& outError);

    /**
     * @brief Hashes the logical cooked artifact closure, not the manifest bytes.
     *
     * The closure is sorted by entry identity and artifact record, and includes
     * tool/settings/recipe metadata so a product cannot be admitted under a
     * different cook recipe with coincidentally identical payload bytes.
     */
    [[nodiscard]] bool ComputeCookedContentIdentity(const CookManifest& manifest,
                                                     ResourceContentIdentity& outIdentity,
                                                     std::string& outError);

    /** @brief Re-hashes mounted inputs/products and evaluates one exact selector. */
    [[nodiscard]] CookManifestAdmissionReceipt VerifyCookedAssetAdmission(
        const CookManifest& manifest,
        const fs::path& mountedSourceRoot,
        const fs::path& mountedCookedRoot,
        const CookManifestExpectation& expectation = {});
} // namespace RVX::Resource
