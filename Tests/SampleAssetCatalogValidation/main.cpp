#include "Core/Hash/SHA256.h"
#include "Core/Log.h"
#include "Resource/Importer/GLTFImporter.h"
#include "Samples/SampleAssetCatalog.h"
#include "Samples/SampleContext.h"

#if defined(__has_include)
#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define RVX_SAMPLE_ASSET_CATALOG_HAS_NLOHMANN_JSON 1
#endif
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    class TemporaryCatalog
    {
    public:
        TemporaryCatalog()
        {
            const auto suffix =
                std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = std::filesystem::temp_directory_path() /
                     ("RVX_SampleAssetCatalog_" + std::to_string(suffix));
            std::filesystem::create_directories(m_root / "models");
        }

        ~TemporaryCatalog()
        {
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
        }

        void Write(const std::filesystem::path& relative,
                   const std::string& contents) const
        {
            const std::filesystem::path path = m_root / relative;
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << contents;
        }

        const std::filesystem::path& Root() const { return m_root; }
        std::filesystem::path CatalogPath() const
        {
            return m_root / "catalog.json";
        }

    private:
        std::filesystem::path m_root;
    };

    std::string MakeCatalog(const std::string& entries,
                            uint32_t schemaVersion = 1)
    {
        return "{\n"
               "  \"schemaId\": \"RVX.SampleAssetCatalog\",\n"
               "  \"schemaVersion\": " +
               std::to_string(schemaVersion) + ",\n"
               "  \"assets\": [" +
               entries + "]\n"
                         "}\n";
    }

    std::string MakeEntry(const std::string& id,
                          const std::string& path)
    {
        return "{"
               "\"id\":\"" + id + "\","
               "\"kind\":\"model\","
               "\"path\":\"" + path + "\","
               "\"license\":{"
               "\"spdxId\":\"LicenseRef-Test\","
               "\"file\":\"license.txt\"},"
               "\"source\":{"
               "\"name\":\"generated fixture\","
               "\"uri\":\"repo://fixture\","
               "\"author\":\"RenderVerseX tests\"},"
               "\"redistributable\":true"
               "}";
    }

    std::string MakeContentIdentity(const std::string& digest =
                                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
    {
        return "\"contentIdentity\":{"
               "\"schemaVersion\":1,"
               "\"domain\":\"source\","
               "\"scope\":\"self-contained-artifact\","
               "\"algorithm\":\"sha256\","
               "\"digest\":\"" + digest + "\","
               "\"byteCount\":4,"
               "\"fileCount\":1}";
    }

    struct ManifestFile
    {
        std::string path;
        std::string purpose;
        std::string contents;
    };

    std::string HashContents(const std::string& contents)
    {
        RVX::Hash::SHA256Hasher hasher;
        hasher.Update(contents);
        return hasher.FinalizeHex();
    }

    std::string ReadFileContents(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()};
    }

    std::string HashFile(const std::filesystem::path& path)
    {
        return HashContents(ReadFileContents(path));
    }

    std::string MakeV3Entry(const std::string& id,
                            const std::string& path,
                            const std::vector<ManifestFile>& files,
                            const std::string& additionalFields = {},
                            const std::string& aggregateDigestOverride = {},
                            const std::string& kind = "model")
    {
        RVX::Hash::SHA256Hasher aggregateHasher;
        uint64_t totalByteCount = 0;
        std::string filesJson;
        for (size_t index = 0; index < files.size(); ++index)
        {
            const ManifestFile& file = files[index];
            const std::string digest = HashContents(file.contents);
            const uint64_t byteCount = file.contents.size();
            totalByteCount += byteCount;
            aggregateHasher.Update(digest);
            aggregateHasher.Update(" ");
            aggregateHasher.Update(std::to_string(byteCount));
            aggregateHasher.Update(" ");
            aggregateHasher.Update(file.path);
            aggregateHasher.Update("\n");
            if (index != 0)
            {
                filesJson += ",";
            }
            filesJson += "{\"path\":\"" + file.path + "\","
                         "\"purpose\":\"" + file.purpose + "\","
                         "\"byteCount\":" + std::to_string(byteCount) + ","
                         "\"sha256\":\"" + digest + "\"}";
        }

        const std::string aggregateDigest = aggregateDigestOverride.empty()
                                                ? aggregateHasher.FinalizeHex()
                                                : aggregateDigestOverride;
        return "{"
               "\"id\":\"" + id + "\","
               "\"kind\":\"" + kind + "\","
               "\"path\":\"" + path + "\","
               "\"license\":{"
               "\"spdxId\":\"LicenseRef-Test\","
               "\"file\":\"license.txt\","
               "\"coverage\":\"full-package\"},"
               "\"source\":{"
               "\"name\":\"generated fixture\","
               "\"uri\":\"repo://fixture\","
               "\"author\":\"RenderVerseX tests\","
               "\"version\":\"fixture-v1\","
               "\"archiveSha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
               "\"attribution\":\"fixture attribution\","
               "\"modificationNotice\":\"No modifications.\","
               "\"redistributable\":true,"
               "\"assetContentId\":{"
               "\"schemaVersion\":1,"
               "\"algorithm\":\"sha256\","
               "\"digest\":\"" + aggregateDigest + "\","
               "\"byteCount\":" + std::to_string(totalByteCount) + ","
               "\"fileCount\":" + std::to_string(files.size()) + "},"
               "\"files\":[" + filesJson + "]" +
               (additionalFields.empty() ? std::string{} : "," + additionalFields) +
               "}";
    }

    std::string RemoveField(std::string value, const std::string& field)
    {
        const size_t position = value.find(field);
        if (position != std::string::npos)
        {
            value.erase(position, field.size());
        }
        return value;
    }

    RVX::Resource::CookFileIdentity MakeCookFileIdentity(
        const std::string& path,
        const std::string& contents)
    {
        RVX::Resource::CookFileIdentity identity;
        identity.relativePath = path;
        identity.byteCount = contents.size();
        identity.sha256 = HashContents(contents);
        return identity;
    }

    RVX::Resource::ResourceContentIdentity MakeContentIdentity(
        RVX::Resource::ResourceContentIdentityDomain domain,
        RVX::Resource::ResourceContentIdentityScope scope,
        const std::string& digest,
        const uint64_t byteCount,
        const uint32_t fileCount)
    {
        RVX::Resource::ResourceContentIdentity identity;
        identity.schemaVersion = RVX::Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = domain;
        identity.scope = scope;
        identity.algorithm = RVX::Resource::ResourceContentHashAlgorithm::SHA256;
        identity.digest = digest;
        identity.byteCount = byteCount;
        identity.fileCount = fileCount;
        return identity;
    }

    std::string BuildCookRecipeHash(const RVX::Resource::CookManifestEntry& entry)
    {
        std::string recipe = "recipeSchema=RVX_COOK_RECIPE_V1\n";
        recipe += "toolName=RVXCook\n";
        recipe += "toolVersion=2.0.0\n";
        recipe += "importer=" + entry.importerName + '\n';
        recipe += "assetType=" +
                  std::string(RVX::Resource::GetCookAssetTypeName(entry.type)) + '\n';
        recipe += "output.path=" + entry.outputPath + '\n';
        recipe += "source.path=" + entry.sourceContent.relativePath + '\n';
        recipe += "source.byteCount=" + std::to_string(entry.sourceContent.byteCount) + '\n';
        recipe += "source.sha256=" + entry.sourceContent.sha256 + '\n';
        recipe += "cookSettingsHash=" + entry.cookSettingsHash + '\n';
        recipe += "dependencyCount=" + std::to_string(entry.dependencies.size()) + '\n';
        for (const RVX::Resource::CookFileIdentity& dependency : entry.dependencies)
        {
            recipe += "dependency.path=" + dependency.relativePath + '\n';
            recipe += "dependency.byteCount=" + std::to_string(dependency.byteCount) + '\n';
            recipe += "dependency.sha256=" + dependency.sha256 + '\n';
        }
        return HashContents(recipe);
    }

    std::string MakeIdentityJson(
        const RVX::Resource::ResourceContentIdentity& identity)
    {
        return "{"
               "\"schemaVersion\":" + std::to_string(identity.schemaVersion) + ","
               "\"domain\":\"" +
               std::string(RVX::Resource::GetResourceContentIdentityDomainName(identity.domain)) +
               "\",\"scope\":\"" +
               std::string(RVX::Resource::GetResourceContentIdentityScopeName(identity.scope)) +
               "\",\"algorithm\":\"" +
               std::string(RVX::Resource::GetResourceContentHashAlgorithmName(identity.algorithm)) +
               "\",\"digest\":\"" + identity.digest +
               "\",\"byteCount\":" + std::to_string(identity.byteCount) +
               ",\"fileCount\":" + std::to_string(identity.fileCount) + "}";
    }

    struct CookAdmissionFixture
    {
        TemporaryCatalog catalog;
        std::filesystem::path manifestPath;
        RVX::Resource::ResourceContentIdentity sourceIdentity;
        RVX::Resource::ResourceContentIdentity cookedIdentity;
        RVX::Resource::ResourceContentIdentity manifestIdentity;
        std::string cookSettingsHash;
        std::string recipeHash;

        CookAdmissionFixture()
        {
            constexpr std::string_view SourceContents =
                "{\"asset\":{\"version\":\"2.0\"}}";
            constexpr std::string_view ArtifactContents = "cooked-model-v1";
            catalog.Write("models/model.gltf", std::string(SourceContents));
            catalog.Write("license.txt", "test license");
            catalog.Write("cooked/models/model.rva", std::string(ArtifactContents));

            RVX::Resource::CookManifest manifest;
            manifest.schema = RVX::Resource::CookManifest::SchemaName;
            manifest.version = RVX::Resource::RVX_COOK_MANIFEST_SCHEMA_VERSION;
            manifest.toolName = RVX::Resource::CookManifest::DefaultToolName;
            manifest.toolVersion = RVX::Resource::CookManifest::DefaultToolVersion;
            manifest.observedSourceRoot = "fixture-source";
            manifest.observedOutputRoot = "fixture-cooked";
            manifest.recursive = true;

            RVX::Resource::CookManifestEntry entry;
            entry.sourcePath = "models/model.gltf";
            entry.outputPath = "models/model.rva";
            entry.type = RVX::Resource::CookAssetType::Model;
            entry.success = true;
            entry.importerName = "FixtureImporter";
            entry.sourceContent = MakeCookFileIdentity(entry.sourcePath,
                                                        std::string(SourceContents));
            entry.canonicalCookSettings =
                "settingsSchema=RVX_COOK_SETTINGS_V1\nassetType=Model\noptions=fixture\n";
            entry.cookSettingsHash = HashContents(entry.canonicalCookSettings);
            entry.artifacts.push_back(MakeCookFileIdentity(entry.outputPath,
                                                            std::string(ArtifactContents)));
            entry.recipeHash = BuildCookRecipeHash(entry);
            cookSettingsHash = entry.cookSettingsHash;
            recipeHash = entry.recipeHash;
            sourceIdentity = MakeContentIdentity(
                RVX::Resource::ResourceContentIdentityDomain::Source,
                RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact,
                entry.sourceContent.sha256,
                entry.sourceContent.byteCount,
                1);
            manifest.entries.push_back(std::move(entry));
            manifest.declaredMetrics.entryCount = 1;
            manifest.declaredMetrics.successCount = 1;
            manifest.declaredMetrics.failureCount = 0;

            manifestPath = catalog.Root() / "cooked" / "CookManifest.rvxmanifest";
            std::string error;
            EXPECT_TRUE(RVX::Resource::SaveCookManifest(manifestPath, manifest, error))
                << error;

            RVX::Resource::CookManifest parsed;
            EXPECT_TRUE(RVX::Resource::LoadCookManifest(manifestPath, parsed, error))
                << error;
            manifestIdentity = parsed.manifestContentIdentity;
            EXPECT_TRUE(RVX::Resource::ComputeCookedContentIdentity(parsed,
                                                                      cookedIdentity,
                                                                      error))
                << error;
        }

        std::string MakeCookDeclaration(const std::string& manifestPathText =
                                            "cooked/CookManifest.rvxmanifest",
                                        const std::string& cookedRootText = "cooked") const
        {
            return "\"cook\":{"
                   "\"manifestPath\":\"" + manifestPathText + "\","
                   "\"cookedRoot\":\"" + cookedRootText + "\","
                   "\"selector\":{"
                   "\"sourcePath\":\"models/model.gltf\","
                   "\"outputPath\":\"models/model.rva\","
                   "\"type\":\"Model\"},"
                   "\"sourceContentIdentity\":" + MakeIdentityJson(sourceIdentity) + ","
                   "\"cookedContentIdentity\":" + MakeIdentityJson(cookedIdentity) + ","
                   "\"manifestContentIdentity\":" + MakeIdentityJson(manifestIdentity) + ","
                   "\"cookSettingsHash\":\"" + cookSettingsHash + "\","
                   "\"recipeHash\":\"" + recipeHash + "\","
                   "\"tool\":{\"name\":\"RVXCook\",\"version\":\"2.0.0\"}"
                   "}";
        }

        void WriteCatalog(const std::string& cookDeclaration)
        {
            const std::vector<ManifestFile> files{
                {"models/model.gltf",
                 "runtime-root",
                 "{\"asset\":{\"version\":\"2.0\"}}"}};
            catalog.Write("catalog.json",
                          MakeCatalog(MakeV3Entry("model",
                                                  "models/model.gltf",
                                                  files,
                                                  cookDeclaration),
                                      3));
        }
    };

    TEST(SampleAssetCatalogValidation, LoadsCompleteCatalogDeterministically)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/zeta.gltf", "zeta");
        fixture.Write("models/alpha.gltf", "alpha");
        fixture.Write("license.txt", "test license");
        fixture.Write("catalog.json",
                      MakeCatalog(MakeEntry("zeta", "models/zeta.gltf") +
                                  "," +
                                  MakeEntry("alpha", "models/alpha.gltf")));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error))
            << error;
        const auto entries = catalog.List();
        ASSERT_EQ(entries.size(), 2u);
        EXPECT_EQ(entries[0].id, "alpha");
        EXPECT_EQ(entries[1].id, "zeta");

        const RVX::SampleAssetEntry* alpha = catalog.Find("alpha");
        ASSERT_NE(alpha, nullptr);
        EXPECT_EQ(alpha->kind, RVX::SampleAssetKind::Model);
        EXPECT_EQ(alpha->license.spdxId, "LicenseRef-Test");
        EXPECT_EQ(alpha->source.uri, "repo://fixture");
        EXPECT_TRUE(alpha->redistributable);
        EXPECT_TRUE(std::filesystem::is_regular_file(alpha->resolvedPath));
        EXPECT_TRUE(std::filesystem::is_regular_file(
            alpha->license.resolvedFile));
    }

    TEST(SampleAssetCatalogValidation, RejectsTraversalAndMissingFiles)
    {
        TemporaryCatalog fixture;
        fixture.Write("license.txt", "test license");
        fixture.Write("catalog.json",
                      MakeCatalog(MakeEntry("escape", "../outside.gltf")));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("traversal-free"), std::string::npos) << error;

        fixture.Write("catalog.json",
                      MakeCatalog(MakeEntry("missing", "models/missing.gltf")));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("Missing asset file"), std::string::npos) << error;
    }

    TEST(SampleAssetCatalogValidation, RejectsDuplicateAndIncompleteProvenance)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/model.gltf", "model");
        fixture.Write("license.txt", "test license");
        const std::string entry = MakeEntry("model", "models/model.gltf");
        fixture.Write("catalog.json", MakeCatalog(entry + "," + entry));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("Duplicate"), std::string::npos);

        fixture.Write(
            "catalog.json",
            MakeCatalog(
                "{\"id\":\"model\",\"kind\":\"model\","
                "\"path\":\"models/model.gltf\","
                "\"redistributable\":true}"));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("requires license, source"), std::string::npos);
    }

    TEST(SampleAssetCatalogValidation, LoadsEnvironmentEntriesByDeclaredKind)
    {
        TemporaryCatalog fixture;
        fixture.Write("environments/studio.hdr", "hdr fixture");
        fixture.Write("license.txt", "test license");
        fixture.Write(
            "catalog.json",
            MakeCatalog(
                "{\"id\":\"studio\",\"kind\":\"environment\","
                "\"path\":\"environments/studio.hdr\","
                "\"license\":{\"spdxId\":\"CC0-1.0\","
                "\"file\":\"license.txt\"},"
                "\"source\":{\"name\":\"studio fixture\","
                "\"uri\":\"repo://studio\","
                "\"author\":\"RenderVerseX tests\"},"
                "\"redistributable\":true}"));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error))
            << error;
        const RVX::SampleAssetEntry* environment = catalog.Find("studio");
        ASSERT_NE(environment, nullptr);
        EXPECT_EQ(environment->kind, RVX::SampleAssetKind::Environment);
        EXPECT_TRUE(environment->redistributable);
    }

    TEST(SampleAssetCatalogValidation,
         PreservesV1CompatibilityAndParsesStrictV2ContentIdentity)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/model.gltf", "test");
        fixture.Write("license.txt", "test license");

        fixture.Write("catalog.json",
                      MakeCatalog(MakeEntry("model", "models/model.gltf")));
        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error))
            << error;
        ASSERT_NE(catalog.Find("model"), nullptr);
        EXPECT_TRUE(catalog.Find("model")->contentIdentity.IsEmpty());

        const std::string v2Entry =
            MakeEntry("model", "models/model.gltf");
        const std::string entryWithIdentity = v2Entry.substr(0, v2Entry.size() - 1) +
            "," + MakeContentIdentity() + "}";
        fixture.Write("catalog.json", MakeCatalog(entryWithIdentity, 2));
        ASSERT_TRUE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error))
            << error;
        const RVX::SampleAssetEntry* entry = catalog.Find("model");
        ASSERT_NE(entry, nullptr);
        EXPECT_TRUE(entry->contentIdentity.IsValid());
        EXPECT_EQ(entry->contentIdentity.byteCount, 4u);
        EXPECT_EQ(entry->contentIdentity.fileCount, 1u);
    }

    TEST(SampleAssetCatalogValidation,
         RejectsInvalidV2IdentityAndDuplicateCanonicalAssetPath)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/model.gltf", "test");
        fixture.Write("license.txt", "test license");

        const std::string entry = MakeEntry("model", "models/model.gltf");
        const std::string invalidEntry =
            entry.substr(0, entry.size() - 1) + "," +
            MakeContentIdentity("not-a-sha256-digest") + "}";
        fixture.Write("catalog.json", MakeCatalog(invalidEntry, 2));
        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("contentIdentity"), std::string::npos) << error;

        fixture.Write("catalog.json",
                      MakeCatalog(MakeEntry("first", "models/model.gltf") +
                                      "," +
                                      MakeEntry("second", "models/model.gltf")));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("Duplicate canonical"), std::string::npos) << error;
    }

    TEST(SampleAssetCatalogValidation,
         LoadsAndVerifiesCompleteSchemaV3Package)
    {
        EXPECT_EQ(RVX::RVX_SAMPLE_ASSET_CATALOG_SCHEMA_VERSION, 3u);
        TemporaryCatalog fixture;
        const std::string gltf =
            "{\"asset\":{\"version\":\"2.0\"},"
            "\"buffers\":[{\"uri\":\"buffer.bin\",\"byteLength\":13}],"
            "\"images\":[{\"uri\":\"data:image/png;base64,AA==\"}]}";
        const std::vector<ManifestFile> files{
            {"models/buffer.bin", "buffer", "binary fixture"},
            {"models/model.gltf", "root", gltf}};
        fixture.Write("models/buffer.bin", "binary fixture");
        fixture.Write("models/model.gltf", gltf);
        fixture.Write("license.txt", "test license");
        fixture.Write(
            "catalog.json",
            MakeCatalog(
                MakeV3Entry(
                    "model",
                    "models/model.gltf",
                    files,
                    "\"derivedFrom\":[\"fixture-source-v1\"],"
                    "\"generation\":{"
                    "\"recipe\":\"fixture-generator\","
                    "\"toolVersion\":\"1.0\","
                    "\"recipeHash\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}"),
                3));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error))
            << error;
        const RVX::SampleAssetEntry* entry = catalog.Find("model");
        ASSERT_NE(entry, nullptr);
        EXPECT_TRUE(entry->assetContentId.IsValid());
        EXPECT_EQ(entry->assetContentId.fileCount, 2u);
        EXPECT_EQ(entry->files.size(), 2u);
        EXPECT_EQ(entry->files.front().path.generic_string(), "models/buffer.bin");
        EXPECT_EQ(entry->license.coverage, "full-package");
        EXPECT_EQ(entry->source.version, "fixture-v1");
        EXPECT_EQ(entry->source.archiveSha256.size(), 64u);
        EXPECT_EQ(entry->attribution, "fixture attribution");
        EXPECT_EQ(entry->modificationNotice, "No modifications.");
        ASSERT_EQ(entry->derivedFrom.size(), 1u);
        EXPECT_EQ(entry->derivedFrom.front(), "fixture-source-v1");
        EXPECT_EQ(entry->generation.recipe, "fixture-generator");
    }

    TEST(SampleAssetCatalogValidation,
         ParsesSchemaV3AnimationAndRegistryRejectsWrongKind)
    {
        TemporaryCatalog fixture;
        constexpr std::string_view AnimationBytes = "animation fixture";
        fixture.Write("animations/walk.rvxanim", std::string(AnimationBytes));
        fixture.Write("license.txt", "test license");

        const std::vector<ManifestFile> files{
            {"animations/walk.rvxanim", "runtime-root",
             std::string(AnimationBytes)}};
        fixture.Write("catalog.json",
                      MakeCatalog(MakeV3Entry("walk",
                                              "animations/walk.rvxanim",
                                              files,
                                              {},
                                              {},
                                              "animation"),
                                  3));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error))
            << error;
        const RVX::SampleAssetEntry* entry = catalog.Find("walk");
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->kind, RVX::SampleAssetKind::Animation);
        EXPECT_EQ(entry->path.generic_string(), "animations/walk.rvxanim");
        EXPECT_EQ(entry->files.size(), 1u);
        EXPECT_TRUE(entry->assetContentId.IsValid());
        EXPECT_EQ(entry->files.front().resolvedPath, entry->resolvedPath);

        RVX::SampleAssetRegistry registry({
            {entry->id,
             entry->kind,
             entry->resolvedPath,
             entry->contentIdentity,
             entry->assetContentId}});
        const RVX::SampleAssetRegistryLookupResult animationLookup =
            registry.Lookup("walk", RVX::SampleAssetKind::Animation);
        ASSERT_TRUE(animationLookup.IsFound());
        EXPECT_EQ(animationLookup.entry->kind, RVX::SampleAssetKind::Animation);

        const RVX::SampleAssetRegistryLookupResult wrongKindLookup =
            registry.Lookup("walk", RVX::SampleAssetKind::Model);
        EXPECT_EQ(wrongKindLookup.code,
                  RVX::SampleAssetRegistryLookupCode::KindMismatch);
        ASSERT_NE(wrongKindLookup.entry, nullptr);
        EXPECT_EQ(wrongKindLookup.actualKind,
                  RVX::SampleAssetKind::Animation);

        const RVX::SampleAssetRegistryLookupResult unknownLookup =
            registry.Lookup("missing", RVX::SampleAssetKind::Animation);
        EXPECT_EQ(unknownLookup.code,
                  RVX::SampleAssetRegistryLookupCode::UnknownAssetId);
        EXPECT_EQ(unknownLookup.entry, nullptr);
    }

    TEST(SampleAssetCatalogValidation,
         RejectsUnsortedSchemaV3ManifestAndContentDrift)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/model.gltf", "{}");
        fixture.Write("models/z.bin", "z");
        fixture.Write("license.txt", "test license");

        const std::vector<ManifestFile> unsortedFiles{
            {"models/z.bin", "dependency", "z"},
            {"models/model.gltf", "root", "{}"}};
        fixture.Write("catalog.json",
                      MakeCatalog(MakeV3Entry("model",
                                              "models/model.gltf",
                                              unsortedFiles),
                                  3));
        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("strictly increasing"), std::string::npos) << error;

        const std::vector<ManifestFile> files{
            {"models/model.gltf", "root", "{}"}};
        std::string fileDrift = MakeV3Entry("model", "models/model.gltf", files);
        const std::string fileDigest = HashContents("{}");
        const size_t fileDigestPosition = fileDrift.find(fileDigest);
        ASSERT_NE(fileDigestPosition, std::string::npos);
        fileDrift.replace(fileDigestPosition, fileDigest.size(),
                          "cccccccccccccccccccccccccccccccc"
                          "cccccccccccccccccccccccccccccccc");
        fixture.Write("catalog.json", MakeCatalog(fileDrift, 3));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("hash or byte count drift"), std::string::npos) << error;

        fixture.Write("catalog.json",
                      MakeCatalog(
                          MakeV3Entry(
                              "model",
                              "models/model.gltf",
                              files,
                              {},
                              "dddddddddddddddddddddddddddddddd"
                              "dddddddddddddddddddddddddddddddd"),
                          3));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("assetContentId"), std::string::npos) << error;
    }

    TEST(SampleAssetCatalogValidation,
         RejectsSchemaV3UnsafePathAndSymlinkEscape)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/model.gltf", "{}");
        fixture.Write("license.txt", "test license");
        const std::vector<ManifestFile> unsafeFiles{
            {"models/../model.gltf", "root", "{}"}};
        fixture.Write("catalog.json",
                      MakeCatalog(MakeV3Entry("model",
                                              "models/../model.gltf",
                                              unsafeFiles),
                                  3));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("Schema-v3"), std::string::npos) << error;

        const std::filesystem::path outside =
            fixture.Root().parent_path() /
            (fixture.Root().filename().string() + "_outside.bin");
        {
            std::ofstream stream(outside, std::ios::binary | std::ios::trunc);
            stream << "outside fixture";
        }
        std::error_code linkError;
        std::filesystem::create_symlink(outside,
                                        fixture.Root() / "models" / "escape.bin",
                                        linkError);
        if (linkError)
        {
            std::filesystem::remove(outside, linkError);
            return;
        }

        const std::vector<ManifestFile> symlinkFiles{
            {"models/escape.bin", "dependency", "outside fixture"},
            {"models/model.gltf", "root", "{}"}};
        fixture.Write("catalog.json",
                      MakeCatalog(MakeV3Entry("model",
                                              "models/model.gltf",
                                              symlinkFiles),
                                  3));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("escapes asset root"), std::string::npos) << error;
        std::filesystem::remove(outside, linkError);
    }

    TEST(SampleAssetCatalogValidation,
         RejectsRemoteAndUnlistedSchemaV3GltfDependencies)
    {
        TemporaryCatalog fixture;
        fixture.Write("license.txt", "test license");
        const std::string remoteGltf =
            "{\"buffers\":[{\"uri\":\"https://example.invalid/model.bin\"}]}";
        fixture.Write("models/model.gltf", remoteGltf);
        const std::vector<ManifestFile> remoteFiles{
            {"models/model.gltf", "root", remoteGltf}};
        fixture.Write("catalog.json",
                      MakeCatalog(MakeV3Entry("model",
                                              "models/model.gltf",
                                              remoteFiles),
                                  3));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("local relative"), std::string::npos) << error;

        const std::string unlistedGltf =
            "{\"buffers\":[{\"uri\":\"buffer.bin\"}]}";
        fixture.Write("models/model.gltf", unlistedGltf);
        fixture.Write("models/buffer.bin", "unlisted dependency");
        const std::vector<ManifestFile> unlistedFiles{
            {"models/model.gltf", "root", unlistedGltf}};
        fixture.Write("catalog.json",
                      MakeCatalog(MakeV3Entry("model",
                                              "models/model.gltf",
                                              unlistedFiles),
                                  3));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("not listed"), std::string::npos) << error;
    }

    TEST(SampleAssetCatalogValidation,
         RejectsIncompleteSchemaV3LicenseAndDerivationMetadata)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/model.gltf", "{}");
        fixture.Write("license.txt", "test license");
        const std::vector<ManifestFile> files{
            {"models/model.gltf", "root", "{}"}};
        const std::string validEntry =
            MakeV3Entry("model", "models/model.gltf", files);

        fixture.Write("catalog.json",
                      MakeCatalog(RemoveField(validEntry,
                                              ",\"coverage\":\"full-package\""),
                                  3));
        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("coverage"), std::string::npos) << error;

        fixture.Write("catalog.json",
                      MakeCatalog(MakeV3Entry("model",
                                              "models/model.gltf",
                                              files,
                                              "\"derivedFrom\":[\"fixture-source\"]"),
                                  3));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("derivedFrom and generation"), std::string::npos) << error;

        fixture.Write("catalog.json",
                      MakeCatalog(RemoveField(validEntry,
                                              "\"modificationNotice\":\"No modifications.\","),
                                  3));
        error.clear();
        EXPECT_FALSE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error));
        EXPECT_NE(error.find("modificationNotice"), std::string::npos) << error;
    }

    TEST(SampleAssetCatalogValidation,
         QualificationCookAdmissionVerifiesManifestAndMountedClosure)
    {
        CookAdmissionFixture fixture;
        fixture.WriteCatalog(fixture.MakeCookDeclaration());

        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.catalog.CatalogPath(),
                                 fixture.catalog.Root(),
                                 &error))
            << error;
        const RVX::SampleAssetEntry* entry = catalog.Find("model");
        ASSERT_NE(entry, nullptr);
        EXPECT_TRUE(entry->cook.declared);
        EXPECT_EQ(entry->cook.selector.type, RVX::Resource::CookAssetType::Model);
        EXPECT_EQ(entry->cook.manifestContentIdentity,
                  fixture.manifestIdentity);

        const RVX::SampleCookedAssetAdmissionReceipt receipt =
            catalog.RequireCookedAdmission("model");
        EXPECT_TRUE(receipt.IsAccepted()) << receipt.detail;
        EXPECT_EQ(receipt.code, RVX::SampleCookedAssetAdmissionCode::Accepted);
        EXPECT_TRUE(receipt.resourceReceipt.IsAccepted());
    }

    TEST(SampleAssetCatalogValidation,
         QualificationCookAdmissionFailsClosedForLegacyAndMissingDeclarations)
    {
        TemporaryCatalog fixture;
        fixture.Write("models/model.gltf", "legacy model");
        fixture.Write("license.txt", "test license");
        fixture.Write("catalog.json",
                      MakeCatalog(MakeEntry("legacy", "models/model.gltf")));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.CatalogPath(), fixture.Root(), &error))
            << error;
        const RVX::SampleCookedAssetAdmissionReceipt legacy =
            catalog.RequireCookedAdmission("legacy");
        EXPECT_FALSE(legacy.IsAccepted());
        EXPECT_EQ(legacy.code,
                  RVX::SampleCookedAssetAdmissionCode::CookDataMissing);

        const RVX::SampleCookedAssetAdmissionReceipt unknown =
            catalog.RequireCookedAdmission("missing");
        EXPECT_FALSE(unknown.IsAccepted());
        EXPECT_EQ(unknown.code,
                  RVX::SampleCookedAssetAdmissionCode::AssetNotFound);

        CookAdmissionFixture v3Fixture;
        const std::vector<ManifestFile> v3Files{
            {"models/model.gltf",
             "runtime-root",
             "{\"asset\":{\"version\":\"2.0\"}}"}};
        v3Fixture.catalog.Write("catalog.json",
                                MakeCatalog(MakeV3Entry("v3-model",
                                                        "models/model.gltf",
                                                        v3Files),
                                            3));
        ASSERT_TRUE(catalog.Load(v3Fixture.catalog.CatalogPath(),
                                 v3Fixture.catalog.Root(),
                                 &error))
            << error;
        const RVX::SampleCookedAssetAdmissionReceipt v3Missing =
            catalog.RequireCookedAdmission("v3-model");
        EXPECT_EQ(v3Missing.code,
                  RVX::SampleCookedAssetAdmissionCode::CookDataMissing);
    }

    TEST(SampleAssetCatalogValidation,
         QualificationCookAdmissionRejectsEscapedAndMismatchedDeclarations)
    {
        CookAdmissionFixture fixture;
        fixture.WriteCatalog(fixture.MakeCookDeclaration("../outside.rvxmanifest"));

        RVX::SampleAssetCatalog catalog;
        std::string error;
        EXPECT_FALSE(catalog.Load(fixture.catalog.CatalogPath(),
                                  fixture.catalog.Root(),
                                  &error));
        EXPECT_NE(error.find("cook"), std::string::npos) << error;

        const std::string staleRecipe(64, 'a');
        std::string mismatchedDeclaration = fixture.MakeCookDeclaration();
        const size_t recipePosition =
            mismatchedDeclaration.find(fixture.recipeHash);
        ASSERT_NE(recipePosition, std::string::npos);
        mismatchedDeclaration.replace(recipePosition,
                                      fixture.recipeHash.size(),
                                      staleRecipe);
        fixture.WriteCatalog(mismatchedDeclaration);
        error.clear();
        ASSERT_TRUE(catalog.Load(fixture.catalog.CatalogPath(),
                                 fixture.catalog.Root(),
                                 &error))
            << error;
        const RVX::SampleCookedAssetAdmissionReceipt mismatch =
            catalog.RequireCookedAdmission("model");
        EXPECT_FALSE(mismatch.IsAccepted());
        EXPECT_EQ(mismatch.code,
                  RVX::SampleCookedAssetAdmissionCode::ResourceAdmissionFailed);
        EXPECT_EQ(mismatch.resourceReceipt.code,
                  RVX::Resource::CookManifestAdmissionCode::RecipeMismatch);
    }

    TEST(SampleAssetCatalogValidation,
         QualificationCookAdmissionDetectsCookedArtifactTamperingAfterLoad)
    {
        CookAdmissionFixture fixture;
        fixture.WriteCatalog(fixture.MakeCookDeclaration());

        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(fixture.catalog.CatalogPath(),
                                 fixture.catalog.Root(),
                                 &error))
            << error;
        fixture.catalog.Write("cooked/models/model.rva", "tampered cooked bytes");

        const RVX::SampleCookedAssetAdmissionReceipt receipt =
            catalog.RequireCookedAdmission("model");
        EXPECT_FALSE(receipt.IsAccepted());
        EXPECT_EQ(receipt.code,
                  RVX::SampleCookedAssetAdmissionCode::ResourceAdmissionFailed);
        EXPECT_EQ(receipt.resourceReceipt.code,
                  RVX::Resource::CookManifestAdmissionCode::MountedContentMismatch);
    }

#if defined(RVX_SOURCE_DIR)
    TEST(SampleAssetCatalogValidation, LoadsVendoredProductionCatalogOffline)
    {
        const std::filesystem::path assetRoot =
            std::filesystem::path(RVX_SOURCE_DIR) /
            "Samples" / "RenderVerseSamples" / "Assets";
        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(assetRoot / "catalog.json", assetRoot, &error))
            << error;
        const auto requireAsset = [&catalog](const char* id,
                                             RVX::SampleAssetKind kind,
                                             uint32_t fileCount)
        {
            const RVX::SampleAssetEntry* entry = catalog.Find(id);
            ASSERT_NE(entry, nullptr) << id;
            EXPECT_EQ(entry->kind, kind) << id;
            EXPECT_TRUE(entry->assetContentId.IsValid()) << id;
            EXPECT_EQ(entry->assetContentId.fileCount, fileCount) << id;
            EXPECT_TRUE(entry->contentIdentity.IsValid()) << id;
            EXPECT_TRUE(std::filesystem::is_regular_file(entry->resolvedPath)) << id;
            EXPECT_TRUE(std::filesystem::is_regular_file(entry->license.resolvedFile)) << id;
        };

        requireAsset("water-bottle", RVX::SampleAssetKind::Model, 1u);
        requireAsset("corset", RVX::SampleAssetKind::Model, 1u);
        requireAsset("casual-female", RVX::SampleAssetKind::Model, 5u);
        requireAsset("crytek-sponza", RVX::SampleAssetKind::Model, 26u);
        requireAsset("kloofendal-48d-partly-cloudy-pure-sky",
                     RVX::SampleAssetKind::Environment,
                     1u);
    }

    TEST(SampleAssetCatalogValidation,
         PinsProductionCasualFemaleCatalogAndSkeletalSourceContract)
    {
        EXPECT_EQ(RVX::RVX_SAMPLE_ASSET_CATALOG_SCHEMA_VERSION, 3u);
        const std::filesystem::path assetRoot =
            std::filesystem::path(RVX_SOURCE_DIR) /
            "Samples" / "RenderVerseSamples" / "Assets";
        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(assetRoot / "catalog.json", assetRoot, &error))
            << error;

        const RVX::SampleAssetEntry* entry = catalog.Find("casual-female");
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->kind, RVX::SampleAssetKind::Model);
        EXPECT_EQ(entry->path.generic_string(),
                  "models/casual-female/Casual_Female.gltf");
        EXPECT_EQ(entry->license.spdxId, "CC0-1.0");
        EXPECT_EQ(entry->license.file.generic_string(),
                  "LICENSE.quaternius-cc0.txt");
        EXPECT_TRUE(std::filesystem::is_regular_file(entry->license.resolvedFile));
        EXPECT_TRUE(entry->redistributable);

        constexpr std::string_view SourceUriId =
            "1E79ks2jbMt5iIrI8Ag9lRgA0VRnfU4pk";
        EXPECT_NE(entry->source.uri.find(SourceUriId), std::string::npos);

        const RVX::Resource::ResourceContentIdentity& sourceIdentity =
            entry->contentIdentity;
        EXPECT_TRUE(sourceIdentity.IsValid());
        EXPECT_EQ(sourceIdentity.schemaVersion,
                  RVX::Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION);
        EXPECT_EQ(sourceIdentity.domain,
                  RVX::Resource::ResourceContentIdentityDomain::Source);
        EXPECT_EQ(sourceIdentity.scope,
                  RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact);
        EXPECT_EQ(sourceIdentity.algorithm,
                  RVX::Resource::ResourceContentHashAlgorithm::SHA256);
        EXPECT_EQ(sourceIdentity.digest,
                  "87327963ab0f37d004c7340d130808727f3d67b724c3d41aad01fd63c7d6fc8a");
        EXPECT_EQ(sourceIdentity.byteCount, 2058883u);
        EXPECT_EQ(sourceIdentity.fileCount, 1u);

        EXPECT_TRUE(entry->assetContentId.IsValid());
        EXPECT_EQ(entry->assetContentId.schemaVersion,
                  RVX::RVX_SAMPLE_ASSET_CONTENT_ID_SCHEMA_VERSION);
        EXPECT_EQ(entry->assetContentId.algorithm, "sha256");
        EXPECT_EQ(entry->assetContentId.digest,
                  "42dec4e0c8ed81f540f666943f7823e500f53d22c9eccc7a9750cc9c1324dee5");
        EXPECT_EQ(entry->assetContentId.byteCount, 3595794u);
        EXPECT_EQ(entry->assetContentId.fileCount, 5u);

        ASSERT_EQ(entry->files.size(), 5u);
        for (const RVX::SampleAssetFile& declaredFile : entry->files)
        {
            EXPECT_TRUE(std::filesystem::is_regular_file(declaredFile.resolvedPath));
            EXPECT_EQ(std::filesystem::file_size(declaredFile.resolvedPath),
                      declaredFile.byteCount);
            EXPECT_EQ(HashFile(declaredFile.resolvedPath), declaredFile.sha256);
        }
        const auto runtimeRoot = std::find_if(
            entry->files.begin(),
            entry->files.end(),
            [](const RVX::SampleAssetFile& candidate)
            {
                return candidate.purpose == "runtime-root";
            });
        ASSERT_NE(runtimeRoot, entry->files.end());
        const RVX::SampleAssetFile& file = *runtimeRoot;
        EXPECT_EQ(file.path.generic_string(), entry->path.generic_string());
        EXPECT_EQ(file.resolvedPath, entry->resolvedPath);
        EXPECT_EQ(file.purpose, "runtime-root");
        EXPECT_EQ(file.byteCount, 2058883u);
        EXPECT_EQ(file.sha256, sourceIdentity.digest);
        EXPECT_EQ(HashFile(file.resolvedPath), sourceIdentity.digest);

        ASSERT_TRUE(entry->cook.declared);
        EXPECT_EQ(entry->cook.selector.sourcePath,
                  "models/casual-female/Casual_Female.gltf");
        EXPECT_EQ(entry->cook.selector.outputPath,
                  "models/casual-female/Casual_Female.rva");
        EXPECT_EQ(entry->cook.selector.type,
                  RVX::Resource::CookAssetType::Model);
        EXPECT_EQ(entry->cook.cookedContentIdentity.digest,
                  "c91215f2751eb272b6750de1a03829d2599577a13842fa76dc488e621a94ce89");
        EXPECT_EQ(entry->cook.cookedContentIdentity.byteCount, 1534789u);
        EXPECT_EQ(entry->cook.cookedContentIdentity.fileCount, 3u);
        EXPECT_EQ(entry->cook.manifestContentIdentity.digest,
                  "87e1fb3666fce0a75740894ea6124e8cb27e7fa88c4142fe9debe40670132096");
        EXPECT_EQ(entry->cook.cookSettingsHash,
                  "d47a3f3b166ac705cfd06128506f127d776f43ad59eaeaa5cb4d52c1946829fa");
        EXPECT_EQ(entry->cook.recipeHash,
                  "815ad489b05e8595afec66fa1dadfc39cb1dea900cea4e18bad7e5092f8592c5");
        const RVX::SampleCookedAssetAdmissionReceipt admission =
            catalog.RequireCookedAdmission("casual-female");
        EXPECT_TRUE(admission.IsAccepted()) << admission.detail;

        const std::string gltfContents = ReadFileContents(entry->resolvedPath);
        ASSERT_EQ(gltfContents.size(), sourceIdentity.byteCount);

#if defined(RVX_SAMPLE_ASSET_CATALOG_HAS_NLOHMANN_JSON)
        const nlohmann::json gltf = nlohmann::json::parse(gltfContents);
        ASSERT_TRUE(gltf.contains("skins"));
        ASSERT_TRUE(gltf.contains("animations"));
        ASSERT_EQ(gltf.at("skins").size(), 1u);
        ASSERT_EQ(gltf.at("skins").at(0).at("joints").size(), 23u);
        ASSERT_EQ(gltf.at("animations").size(), 17u);

        const auto hasAnimation = [&gltf](std::string_view name)
        {
            for (const nlohmann::json& animation : gltf.at("animations"))
            {
                if (animation.value("name", std::string{}) == name)
                    return true;
            }
            return false;
        };
        EXPECT_TRUE(hasAnimation("Idle"));
        EXPECT_TRUE(hasAnimation("Walk"));
        EXPECT_TRUE(hasAnimation("Run"));
#else
        struct ScopedLog final
        {
            ScopedLog() { RVX::Log::Initialize(); }
            ~ScopedLog() { RVX::Log::Shutdown(); }
        };
        [[maybe_unused]] ScopedLog scopedLog;

        RVX::Resource::GLTFImporter importer;
        const RVX::Resource::GLTFImportResult imported =
            importer.Import(entry->resolvedPath.string());
        ASSERT_TRUE(imported.success) << imported.errorMessage;
        // GLTFImporter fails closed for more than one skin, so a successful
        // skinned import proves the source contains exactly one skin.
        EXPECT_TRUE(imported.hasSkins);
        ASSERT_TRUE(imported.skeleton);
        EXPECT_EQ(imported.skeleton->GetBoneCount(), 23u);
        ASSERT_EQ(imported.animationClips.size(), 17u);
        EXPECT_TRUE(imported.animationClips.contains("Idle"));
        EXPECT_TRUE(imported.animationClips.contains("Walk"));
        EXPECT_TRUE(imported.animationClips.contains("Run"));
#endif
    }

    TEST(SampleAssetCatalogValidation,
         PinsProductionCasualFemaleWalkRootMotionAnimationPackage)
    {
        const std::filesystem::path assetRoot =
            std::filesystem::path(RVX_SOURCE_DIR) /
            "Samples" / "RenderVerseSamples" / "Assets";
        RVX::SampleAssetCatalog catalog;
        std::string error;
        ASSERT_TRUE(catalog.Load(assetRoot / "catalog.json", assetRoot, &error))
            << error;

        const RVX::SampleAssetEntry* entry =
            catalog.Find("casual-female-walk-root-motion");
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->kind, RVX::SampleAssetKind::Animation);
        EXPECT_EQ(entry->path.generic_string(),
                  "cooked/casual-female-root-motion/"
                  "casual-female-walk-root-motion.rvxanim");
        EXPECT_EQ(entry->license.spdxId, "CC0-1.0");
        EXPECT_EQ(entry->license.file.generic_string(),
                  "LICENSE.quaternius-cc0.txt");
        EXPECT_EQ(entry->license.coverage, "complete-declared-package");
        EXPECT_TRUE(std::filesystem::is_regular_file(entry->license.resolvedFile));
        EXPECT_TRUE(entry->redistributable);

        EXPECT_EQ(entry->source.name,
                  "Casual Female Walk Root Motion (deterministic derivative)");
        EXPECT_EQ(entry->source.uri,
                  "https://drive.google.com/file/d/1E79ks2jbMt5iIrI8Ag9lRgA0VRnfU4pk/view");
        EXPECT_EQ(entry->source.author,
                  "Quaternius; deterministic derivative by RenderVerseX contributors");
        EXPECT_EQ(entry->source.version, "rvx-animation-root-motion-derive-v1");
        EXPECT_EQ(entry->attribution,
                  "Derived from Quaternius Casual Female Walk, CC0 1.0 Universal.");
        EXPECT_EQ(entry->modificationNotice,
                  "The Walk clip root track is deterministically replaced with +Z "
                  "translation at 1.0 metre/second; all non-root animation data is "
                  "retained.");

        ASSERT_EQ(entry->derivedFrom.size(), 1u);
        EXPECT_EQ(entry->derivedFrom[0], "casual-female");
        EXPECT_EQ(entry->generation.recipe,
                  "rvx-animation-root-motion-derive-v1");
        EXPECT_EQ(entry->generation.toolVersion, "1.0.0");
        EXPECT_EQ(entry->generation.recipeHash,
                  "3f12412b0a93123d4727c06ab91a87c5e86d7c1055b1db511de0f62e9eab21e3");

        const auto expectIdentity =
            [](const RVX::Resource::ResourceContentIdentity& identity,
               RVX::Resource::ResourceContentIdentityDomain domain,
               RVX::Resource::ResourceContentIdentityScope scope,
               const char* digest,
               uint64_t byteCount,
               uint32_t fileCount)
        {
            EXPECT_TRUE(identity.IsValid());
            EXPECT_EQ(identity.schemaVersion,
                      RVX::Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION);
            EXPECT_EQ(identity.domain, domain);
            EXPECT_EQ(identity.scope, scope);
            EXPECT_EQ(identity.algorithm,
                      RVX::Resource::ResourceContentHashAlgorithm::SHA256);
            EXPECT_EQ(identity.digest, digest);
            EXPECT_EQ(identity.byteCount, byteCount);
            EXPECT_EQ(identity.fileCount, fileCount);
        };

        expectIdentity(
            entry->contentIdentity,
            RVX::Resource::ResourceContentIdentityDomain::CookedArtifact,
            RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact,
            "8a04a46e8507391e11df7b2102e5ba3260aecea14d822b33c2d5871c9909e1ca",
            54462u,
            1u);

        EXPECT_TRUE(entry->assetContentId.IsValid());
        EXPECT_EQ(entry->assetContentId.schemaVersion,
                  RVX::RVX_SAMPLE_ASSET_CONTENT_ID_SCHEMA_VERSION);
        EXPECT_EQ(entry->assetContentId.algorithm, "sha256");
        EXPECT_EQ(entry->assetContentId.digest,
                  "3e32fb4c332021590f3841ddb7fe858e09c8240246bfbf16e67bb8b795413360");
        EXPECT_EQ(entry->assetContentId.byteCount, 56156u);
        EXPECT_EQ(entry->assetContentId.fileCount, 2u);

        ASSERT_EQ(entry->files.size(), 2u);
        const RVX::SampleAssetFile& manifestFile = entry->files[0];
        EXPECT_EQ(manifestFile.path.generic_string(),
                  "cooked/casual-female-root-motion/CookManifest.rvxmanifest");
        EXPECT_EQ(manifestFile.purpose, "cook-manifest");
        EXPECT_EQ(manifestFile.byteCount, 1694u);
        EXPECT_EQ(manifestFile.sha256,
                  "00f59bc67067c1503f148daa77da2093e151cf3910aaed186b27bca93fd93c17");
        EXPECT_TRUE(std::filesystem::is_regular_file(manifestFile.resolvedPath));
        EXPECT_EQ(std::filesystem::file_size(manifestFile.resolvedPath),
                  manifestFile.byteCount);
        EXPECT_EQ(HashFile(manifestFile.resolvedPath), manifestFile.sha256);

        const RVX::SampleAssetFile& artifactFile = entry->files[1];
        EXPECT_EQ(artifactFile.path.generic_string(),
                  "cooked/casual-female-root-motion/"
                  "casual-female-walk-root-motion.rvxanim");
        EXPECT_EQ(artifactFile.purpose, "runtime-root");
        EXPECT_EQ(artifactFile.byteCount, 54462u);
        EXPECT_EQ(artifactFile.sha256,
                  "8a04a46e8507391e11df7b2102e5ba3260aecea14d822b33c2d5871c9909e1ca");
        EXPECT_TRUE(std::filesystem::is_regular_file(artifactFile.resolvedPath));
        EXPECT_EQ(std::filesystem::file_size(artifactFile.resolvedPath),
                  artifactFile.byteCount);
        EXPECT_EQ(HashFile(artifactFile.resolvedPath), artifactFile.sha256);

        ASSERT_TRUE(entry->cook.declared);
        EXPECT_EQ(entry->cook.manifestPath.generic_string(),
                  "cooked/casual-female-root-motion/CookManifest.rvxmanifest");
        EXPECT_EQ(entry->cook.cookedRoot.generic_string(),
                  "cooked/casual-female-root-motion");
        EXPECT_EQ(entry->cook.selector.sourcePath,
                  "cooked/casual-female/models/casual-female/"
                  "Casual_Female.rvdeps/animation.rvxanim");
        EXPECT_EQ(entry->cook.selector.outputPath,
                  "casual-female-walk-root-motion.rvxanim");
        EXPECT_EQ(entry->cook.selector.type,
                  RVX::Resource::CookAssetType::Animation);
        expectIdentity(
            entry->cook.sourceContentIdentity,
            RVX::Resource::ResourceContentIdentityDomain::Source,
            RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact,
            "36f4824d1d82aeaae42ce33208b3c774622e09d4547290c89ace8560eeb3ad11",
            927584u,
            1u);
        expectIdentity(
            entry->cook.cookedContentIdentity,
            RVX::Resource::ResourceContentIdentityDomain::CookedArtifact,
            RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact,
            "b820610acdfb55f6f182d753ddeabdc0d73d635816d0b14d93b8ae899b16acbb",
            54462u,
            1u);
        expectIdentity(
            entry->cook.manifestContentIdentity,
            RVX::Resource::ResourceContentIdentityDomain::CookManifest,
            RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact,
            "00f59bc67067c1503f148daa77da2093e151cf3910aaed186b27bca93fd93c17",
            1694u,
            1u);
        EXPECT_EQ(entry->cook.cookSettingsHash,
                  "0618f669053ab678745f85251fe13a8db9966f8196a30f955ea1585bc2c81963");
        EXPECT_EQ(entry->cook.recipeHash,
                  "2984a9d350b89ae24050cbcc3087c4714f66aafca654ea8cef9b7bc5a8dcd709");
        EXPECT_EQ(entry->cook.toolName, "RVXCook");
        EXPECT_EQ(entry->cook.toolVersion, "2.0.0");

        const RVX::SampleCookedAssetAdmissionReceipt admission =
            catalog.RequireCookedAdmission("casual-female-walk-root-motion");
        EXPECT_TRUE(admission.IsAccepted()) << admission.detail;
    }
#endif
} // namespace
