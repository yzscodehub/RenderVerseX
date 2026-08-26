/**
 * @file SampleCookedCatalogWriter.cpp
 * @brief Emits one schema-v3 cooked-only Sample catalog entry from RVXCook output.
 *
 * This tool deliberately derives every identity from the runtime manifest and
 * mounted cooked bytes.  It is a build-time publisher: it never edits a cooked
 * artifact and it never accepts caller-provided hashes.
 */

#include "Core/Hash/SHA256.h"
#include "Resource/CookManifest.h"
#include "Resource/ResourceContentIdentity.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    struct Options
    {
        fs::path inputCatalog;
        fs::path outputCatalog;
        fs::path assetRoot;
        fs::path sourceRoot;
        fs::path manifestPath;
        fs::path cookedRoot;
        std::string cookedRootRelative;
        std::string assetId;
        std::string assetPathRelative;
        std::string sourcePath;
        std::string sourceName;
        std::string sourceUri;
        std::string sourceVersion;
        std::string attribution;
        std::string modificationNotice;
        std::string derivedFrom;
        std::string generationRecipe;
    };

    struct PackageFile
    {
        std::string relativePath;
        std::string purpose;
        uint64_t byteCount = 0;
        std::string sha256;
    };

    void PrintUsage()
    {
        std::cout
            << "RVXSampleCookedCatalogWriter\n"
            << "  --input-catalog <path>\n"
            << "  --output-catalog <path>\n"
            << "  --asset-root <path>\n"
            << "  --source-root <path>\n"
            << "  --manifest <path>\n"
            << "  --cooked-root <path>\n"
            << "  --cooked-root-relative <path>\n"
            << "  --asset-id <id>\n"
            << "  --asset-path-relative <path>\n"
            << "  --source-path <path>\n"
            << "  --source-name <name>\n"
            << "  --source-uri <uri>\n"
            << "  --source-version <version>\n"
            << "  --attribution <text>\n"
            << "  --modification-notice <text>\n"
            << "  --derived-from <id>\n"
            << "  --generation-recipe <id>\n";
    }

    bool ParseOptions(int argc, char** argv, Options& outOptions)
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index];
            const auto requireValue = [&]() -> const char*
            {
                if (index + 1 >= argc)
                {
                    std::cerr << "Missing value for " << argument << "\n";
                    return nullptr;
                }
                return argv[++index];
            };
            const char* value = requireValue();
            if (!value)
            {
                return false;
            }
            if (argument == "--input-catalog") outOptions.inputCatalog = value;
            else if (argument == "--output-catalog") outOptions.outputCatalog = value;
            else if (argument == "--asset-root") outOptions.assetRoot = value;
            else if (argument == "--source-root") outOptions.sourceRoot = value;
            else if (argument == "--manifest") outOptions.manifestPath = value;
            else if (argument == "--cooked-root") outOptions.cookedRoot = value;
            else if (argument == "--cooked-root-relative") outOptions.cookedRootRelative = value;
            else if (argument == "--asset-id") outOptions.assetId = value;
            else if (argument == "--asset-path-relative") outOptions.assetPathRelative = value;
            else if (argument == "--source-path") outOptions.sourcePath = value;
            else if (argument == "--source-name") outOptions.sourceName = value;
            else if (argument == "--source-uri") outOptions.sourceUri = value;
            else if (argument == "--source-version") outOptions.sourceVersion = value;
            else if (argument == "--attribution") outOptions.attribution = value;
            else if (argument == "--modification-notice") outOptions.modificationNotice = value;
            else if (argument == "--derived-from") outOptions.derivedFrom = value;
            else if (argument == "--generation-recipe") outOptions.generationRecipe = value;
            else
            {
                std::cerr << "Unknown argument: " << argument << "\n";
                return false;
            }
        }

        return !outOptions.inputCatalog.empty() && !outOptions.outputCatalog.empty() &&
               !outOptions.assetRoot.empty() && !outOptions.sourceRoot.empty() &&
               !outOptions.manifestPath.empty() && !outOptions.cookedRoot.empty() &&
               !outOptions.cookedRootRelative.empty() && !outOptions.assetId.empty() &&
               !outOptions.assetPathRelative.empty() && !outOptions.sourcePath.empty() &&
               !outOptions.sourceName.empty() && !outOptions.sourceUri.empty() &&
               !outOptions.sourceVersion.empty() && !outOptions.attribution.empty() &&
               !outOptions.modificationNotice.empty() && !outOptions.derivedFrom.empty() &&
               !outOptions.generationRecipe.empty();
    }

    bool IsStrictRelativePath(const std::string& path)
    {
        const fs::path parsed(path);
        if (path.empty() || path.find('\\') != std::string::npos ||
            parsed.is_absolute() || parsed.has_root_path())
        {
            return false;
        }
        for (const fs::path& component : parsed)
        {
            if (component == "." || component == "..") return false;
        }
        return parsed.lexically_normal().generic_string() == path;
    }

    bool ResolveContained(const fs::path& root,
                          const fs::path& candidate,
                          fs::path& outPath,
                          std::string& outError)
    {
        std::error_code error;
        const fs::path canonicalRoot = fs::weakly_canonical(root, error);
        if (error || !fs::is_directory(canonicalRoot, error) || error)
        {
            outError = "Asset root is not an accessible directory: " + root.string();
            return false;
        }
        const fs::path canonicalCandidate = fs::weakly_canonical(candidate, error);
        if (error)
        {
            outError = "Failed to canonicalize package path: " + candidate.string();
            return false;
        }
        auto rootIt = canonicalRoot.begin();
        auto candidateIt = canonicalCandidate.begin();
        for (; rootIt != canonicalRoot.end(); ++rootIt, ++candidateIt)
        {
            if (candidateIt == canonicalCandidate.end() || *rootIt != *candidateIt)
            {
                outError = "Package path escapes staged asset root: " + candidate.string();
                return false;
            }
        }
        outPath = canonicalCandidate;
        return true;
    }

    bool HashFile(const fs::path& path,
                  uint64_t& outByteCount,
                  std::string& outHash,
                  std::string& outError)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            outError = "Failed to open package file: " + path.string();
            return false;
        }
        RVX::Hash::SHA256Hasher hasher;
        std::array<char, 64u * 1024u> buffer{};
        uint64_t total = 0;
        while (input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count <= 0) continue;
            const uint64_t read = static_cast<uint64_t>(count);
            if (total > std::numeric_limits<uint64_t>::max() - read)
            {
                outError = "Package file is too large to hash: " + path.string();
                return false;
            }
            hasher.Update(buffer.data(), static_cast<size_t>(count));
            total += read;
        }
        if (!input.eof())
        {
            outError = "Failed while hashing package file: " + path.string();
            return false;
        }
        outByteCount = total;
        outHash = hasher.FinalizeHex();
        return true;
    }

    Json ToIdentityJson(const RVX::Resource::ResourceContentIdentity& identity)
    {
        return {
            {"schemaVersion", identity.schemaVersion},
            {"domain", RVX::Resource::GetResourceContentIdentityDomainName(identity.domain)},
            {"scope", RVX::Resource::GetResourceContentIdentityScopeName(identity.scope)},
            {"algorithm", RVX::Resource::GetResourceContentHashAlgorithmName(identity.algorithm)},
            {"digest", identity.digest},
            {"byteCount", identity.byteCount},
            {"fileCount", identity.fileCount},
        };
    }

    RVX::Resource::ResourceContentIdentity MakeArtifactIdentity(
        const RVX::Resource::CookFileIdentity& artifact)
    {
        RVX::Resource::ResourceContentIdentity identity;
        identity.schemaVersion = RVX::Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = RVX::Resource::ResourceContentIdentityDomain::CookedArtifact;
        identity.scope = RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = RVX::Resource::ResourceContentHashAlgorithm::SHA256;
        identity.digest = artifact.sha256;
        identity.byteCount = artifact.byteCount;
        identity.fileCount = 1;
        return identity;
    }

    bool WriteCatalog(const Options& options, std::string& outError)
    {
        if (!IsStrictRelativePath(options.cookedRootRelative) ||
            !IsStrictRelativePath(options.assetPathRelative) ||
            !IsStrictRelativePath(options.sourcePath))
        {
            outError = "Catalog writer requires canonical relative package paths";
            return false;
        }

        std::ifstream catalogInput(options.inputCatalog, std::ios::binary);
        Json catalog = Json::parse(catalogInput, nullptr, false);
        if (!catalogInput || catalog.is_discarded() || !catalog.is_object() ||
            !catalog.contains("schemaId") ||
            catalog.value("schemaId", std::string{}) != "RVX.SampleAssetCatalog" ||
            catalog.value("schemaVersion", 0u) != 3u ||
            !catalog.contains("assets") || !catalog["assets"].is_array())
        {
            outError = "Input Sample catalog is not schema-v3 RVX.SampleAssetCatalog";
            return false;
        }
        // Windows does not permit renaming the temporary output over this
        // file while its input handle remains open.
        catalogInput.close();
        for (const Json& existing : catalog["assets"])
        {
            if (existing.is_object() && existing.value("id", std::string{}) == options.assetId)
            {
                outError = "Input Sample catalog already contains cooked asset id: " + options.assetId;
                return false;
            }
        }

        RVX::Resource::CookManifest manifest;
        if (!RVX::Resource::LoadCookManifest(options.manifestPath, manifest, outError))
        {
            return false;
        }

        RVX::Resource::CookManifestExpectation expectation;
        expectation.selector.sourcePath = options.sourcePath;
        expectation.selector.outputPath = fs::path(options.assetPathRelative)
            .lexically_relative(options.cookedRootRelative).generic_string();
        expectation.selector.type = RVX::Resource::CookAssetType::Model;
        const RVX::Resource::CookManifestAdmissionReceipt cookReceipt =
            RVX::Resource::VerifyCookedAssetAdmission(
                manifest, options.sourceRoot, options.cookedRoot, expectation);
        if (!cookReceipt.IsAccepted())
        {
            outError = "RVXCook output admission failed: " + cookReceipt.detail;
            return false;
        }
        if (cookReceipt.selectedEntryIndex >= manifest.entries.size())
        {
            outError = "RVXCook output admission did not select a manifest entry";
            return false;
        }
        const RVX::Resource::CookManifestEntry& selected =
            manifest.entries[cookReceipt.selectedEntryIndex];

        const std::string primaryArtifactPath = expectation.selector.outputPath;
        const auto primaryArtifact = std::find_if(
            selected.artifacts.begin(), selected.artifacts.end(),
            [&primaryArtifactPath](const RVX::Resource::CookFileIdentity& artifact)
            {
                return artifact.relativePath == primaryArtifactPath;
            });
        if (primaryArtifact == selected.artifacts.end())
        {
            outError = "Selected RVXCook model entry has no primary model artifact";
            return false;
        }

        fs::path canonicalManifest;
        if (!ResolveContained(options.assetRoot, options.manifestPath,
                              canonicalManifest, outError))
        {
            return false;
        }
        std::error_code error;
        const fs::path normalizedAssetRoot = fs::weakly_canonical(options.assetRoot, error);
        if (error)
        {
            outError = "Unable to canonicalize staged asset root";
            return false;
        }

        std::vector<PackageFile> packageFiles;
        const std::string manifestRelative = fs::relative(options.manifestPath,
                                                          normalizedAssetRoot,
                                                          error).generic_string();
        if (error || !IsStrictRelativePath(manifestRelative))
        {
            outError = "Cook manifest does not remain below staged asset root";
            return false;
        }
        PackageFile manifestFile;
        manifestFile.relativePath = manifestRelative;
        manifestFile.purpose = "cook-manifest";
        if (!HashFile(options.manifestPath, manifestFile.byteCount, manifestFile.sha256, outError))
        {
            return false;
        }
        packageFiles.push_back(std::move(manifestFile));

        for (const RVX::Resource::CookManifestEntry& entry : manifest.entries)
        {
            for (const RVX::Resource::CookFileIdentity& artifact : entry.artifacts)
            {
                const fs::path physical = options.cookedRoot / artifact.relativePath;
                fs::path canonicalArtifact;
                if (!ResolveContained(options.assetRoot, physical, canonicalArtifact, outError))
                {
                    return false;
                }
                uint64_t byteCount = 0;
                std::string hash;
                if (!HashFile(canonicalArtifact, byteCount, hash, outError))
                {
                    return false;
                }
                if (byteCount != artifact.byteCount || hash != artifact.sha256)
                {
                    outError = "Cooked artifact does not match RVXCook manifest: " +
                               artifact.relativePath;
                    return false;
                }
                const fs::path relative = fs::relative(canonicalArtifact,
                                                        normalizedAssetRoot,
                                                        error);
                const std::string relativeText = relative.generic_string();
                if (error || !IsStrictRelativePath(relativeText))
                {
                    outError = "Cooked artifact escapes staged asset root";
                    return false;
                }
                packageFiles.push_back({relativeText,
                                        relativeText == options.assetPathRelative
                                            ? "runtime-root"
                                            : "cooked-artifact",
                                        byteCount,
                                        std::move(hash)});
            }
        }
        std::sort(packageFiles.begin(), packageFiles.end(),
                  [](const PackageFile& left, const PackageFile& right)
                  {
                      return left.relativePath < right.relativePath;
                  });
        if (packageFiles.empty() ||
            std::adjacent_find(packageFiles.begin(), packageFiles.end(),
                               [](const PackageFile& left, const PackageFile& right)
                               {
                                   return left.relativePath == right.relativePath;
                               }) != packageFiles.end())
        {
            outError = "Cooked Sample package file inventory is empty or non-unique";
            return false;
        }

        RVX::Hash::SHA256Hasher packageHasher;
        uint64_t packageBytes = 0;
        Json files = Json::array();
        for (const PackageFile& file : packageFiles)
        {
            if (packageBytes > std::numeric_limits<uint64_t>::max() - file.byteCount)
            {
                outError = "Cooked Sample package byte count overflows uint64";
                return false;
            }
            packageBytes += file.byteCount;
            packageHasher.Update(file.sha256);
            packageHasher.Update(" ");
            packageHasher.Update(std::to_string(file.byteCount));
            packageHasher.Update(" ");
            packageHasher.Update(file.relativePath);
            packageHasher.Update("\n");
            files.push_back({
                {"path", file.relativePath},
                {"purpose", file.purpose},
                {"byteCount", file.byteCount},
                {"sha256", file.sha256},
            });
        }

        Json entry = {
            {"id", options.assetId},
            {"kind", "model"},
            {"path", options.assetPathRelative},
            {"license", {
                {"spdxId", "LicenseRef-RenderVerseX-Generated"},
                {"file", "LICENSE.generated-fixtures.txt"},
                {"coverage", "complete-declared-package"},
            }},
            {"source", {
                {"name", options.sourceName},
                {"uri", options.sourceUri},
                {"author", "RenderVerseX contributors"},
                {"version", options.sourceVersion},
            }},
            {"contentIdentity", ToIdentityJson(MakeArtifactIdentity(*primaryArtifact))},
            {"assetContentId", {
                {"schemaVersion", 1},
                {"algorithm", "sha256"},
                {"digest", packageHasher.FinalizeHex()},
                {"byteCount", packageBytes},
                {"fileCount", static_cast<uint32_t>(packageFiles.size())},
            }},
            {"files", std::move(files)},
            {"attribution", options.attribution},
            {"modificationNotice", options.modificationNotice},
            {"redistributable", true},
            {"derivedFrom", Json::array({options.derivedFrom})},
            {"generation", {
                {"recipe", options.generationRecipe},
                {"toolVersion", manifest.toolVersion},
                {"recipeHash", selected.recipeHash},
            }},
            {"cook", {
                {"packageOnly", true},
                {"manifestPath", manifestRelative},
                {"cookedRoot", options.cookedRootRelative},
                {"selector", {
                    {"sourcePath", selected.sourcePath},
                    {"outputPath", selected.outputPath},
                    {"type", RVX::Resource::GetCookAssetTypeName(selected.type)},
                }},
                {"sourceContentIdentity", ToIdentityJson(cookReceipt.observedSourceContentIdentity)},
                {"cookedContentIdentity", ToIdentityJson(cookReceipt.observedCookedContentIdentity)},
                {"manifestContentIdentity", ToIdentityJson(cookReceipt.observedManifestContentIdentity)},
                {"cookSettingsHash", selected.cookSettingsHash},
                {"recipeHash", selected.recipeHash},
                {"tool", {
                    {"name", manifest.toolName},
                    {"version", manifest.toolVersion},
                }},
            }},
        };
        catalog["assets"].push_back(std::move(entry));

        const fs::path temporary = options.outputCatalog.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                outError = "Failed to open temporary catalog output: " + temporary.string();
                return false;
            }
            output << catalog.dump(2) << '\n';
            if (!output)
            {
                outError = "Failed to write temporary catalog output: " + temporary.string();
                return false;
            }
        }
        fs::rename(temporary, options.outputCatalog, error);
        if (error)
        {
            fs::remove(options.outputCatalog, error);
            error.clear();
            fs::rename(temporary, options.outputCatalog, error);
        }
        if (error)
        {
            outError = "Failed to publish cooked Sample catalog: " + error.message();
            return false;
        }
        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        PrintUsage();
        return 1;
    }
    std::string error;
    if (!WriteCatalog(options, error))
    {
        std::cerr << error << '\n';
        return 2;
    }
    return 0;
}
