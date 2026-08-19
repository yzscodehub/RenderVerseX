#include "Core/Hash/SHA256.h"
#include "Resource/CookManifest.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace RVX;
using namespace RVX::Resource;
namespace fs = std::filesystem;

namespace
{
    std::string HashText(const std::string_view text)
    {
        Hash::SHA256Hasher hasher;
        hasher.Update(text);
        return hasher.FinalizeHex();
    }

    void WriteFile(const fs::path& path, const std::string_view contents)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        ASSERT_FALSE(error);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(output.good());
    }

    CookFileIdentity MakeIdentity(const std::string& path, const std::string_view contents)
    {
        CookFileIdentity identity;
        identity.relativePath = path;
        identity.byteCount = contents.size();
        identity.sha256 = HashText(contents);
        return identity;
    }

    std::string BuildRecipeHash(const CookManifestEntry& entry)
    {
        std::string recipe = "recipeSchema=RVX_COOK_RECIPE_V1\n";
        recipe += "toolName=RVXCook\n";
        recipe += "toolVersion=2.0.0\n";
        recipe += "importer=" + entry.importerName + '\n';
        recipe += "assetType=" + std::string(GetCookAssetTypeName(entry.type)) + '\n';
        recipe += "output.path=" + entry.outputPath + '\n';
        recipe += "source.path=" + entry.sourceContent.relativePath + '\n';
        recipe += "source.byteCount=" + std::to_string(entry.sourceContent.byteCount) + '\n';
        recipe += "source.sha256=" + entry.sourceContent.sha256 + '\n';
        if (entry.sourceDependencyClosureRecorded)
        {
            recipe += "sourceDependencyCount=" +
                      std::to_string(entry.sourceDependencies.size()) + '\n';
            for (const CookFileIdentity& dependency : entry.sourceDependencies)
            {
                recipe += "sourceDependency.path=" + dependency.relativePath + '\n';
                recipe += "sourceDependency.byteCount=" + std::to_string(dependency.byteCount) + '\n';
                recipe += "sourceDependency.sha256=" + dependency.sha256 + '\n';
            }
        }
        recipe += "cookSettingsHash=" + entry.cookSettingsHash + '\n';
        recipe += "dependencyCount=" + std::to_string(entry.dependencies.size()) + '\n';
        for (const CookFileIdentity& dependency : entry.dependencies)
        {
            recipe += "dependency.path=" + dependency.relativePath + '\n';
            recipe += "dependency.byteCount=" + std::to_string(dependency.byteCount) + '\n';
            recipe += "dependency.sha256=" + dependency.sha256 + '\n';
        }
        return HashText(recipe);
    }

    ResourceContentIdentity MakeSourceContentIdentity(const CookFileIdentity& file)
    {
        ResourceContentIdentity identity;
        identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = ResourceContentIdentityDomain::Source;
        identity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = ResourceContentHashAlgorithm::SHA256;
        identity.digest = file.sha256;
        identity.byteCount = file.byteCount;
        identity.fileCount = 1;
        return identity;
    }

    class CookManifestValidationFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            m_root = fs::temp_directory_path() / ("rvx-cook-manifest-" + std::to_string(stamp));
            m_sourceRoot = m_root / "source";
            m_cookedRoot = m_root / "cooked";

            WriteFile(m_sourceRoot / "models/model.gltf", "source-model-v1");
            WriteFile(m_cookedRoot / "deps/material.rva", "dependency-v1");
            WriteFile(m_cookedRoot / "models/model.rva", "artifact-model-v1");
            WriteFile(m_cookedRoot / "textures/model.rva", "artifact-texture-v1");
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(m_root, error);
        }

        CookManifest MakeManifest() const
        {
            CookManifest manifest;
            manifest.schema = CookManifest::SchemaName;
            manifest.version = RVX_COOK_MANIFEST_SCHEMA_VERSION;
            manifest.toolName = CookManifest::DefaultToolName;
            manifest.toolVersion = CookManifest::DefaultToolVersion;
            manifest.observedSourceRoot = "machine/source";
            manifest.observedOutputRoot = "machine/cooked";
            manifest.recursive = true;

            CookManifestEntry entry;
            entry.sourcePath = "models/model.gltf";
            entry.outputPath = "models/model.rva";
            entry.type = CookAssetType::Model;
            entry.success = true;
            entry.importerName = "GLTFImporter";
            entry.sourceContent = MakeIdentity(entry.sourcePath, "source-model-v1");
            entry.canonicalCookSettings = "settingsSchema=RVX_COOK_SETTINGS_V1\nassetType=Model\noptions=none\n";
            entry.cookSettingsHash = HashText(entry.canonicalCookSettings);
            entry.dependencies.push_back(MakeIdentity("deps/material.rva", "dependency-v1"));
            entry.artifacts.push_back(MakeIdentity("models/model.rva", "artifact-model-v1"));
            entry.artifacts.push_back(MakeIdentity("textures/model.rva", "artifact-texture-v1"));
            entry.recipeHash = BuildRecipeHash(entry);
            manifest.entries.push_back(std::move(entry));
            manifest.declaredMetrics.entryCount = 1;
            manifest.declaredMetrics.successCount = 1;
            manifest.declaredMetrics.failureCount = 0;
            return manifest;
        }

        std::string SaveText(const CookManifest& manifest) const
        {
            const fs::path path = m_root / "CookManifest.rvxmanifest";
            std::string error;
            EXPECT_TRUE(SaveCookManifest(path, manifest, error)) << error;
            std::ifstream input(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        }

        fs::path m_root;
        fs::path m_sourceRoot;
        fs::path m_cookedRoot;
    };

    std::string ReplaceOne(std::string value, const std::string_view from, const std::string_view to)
    {
        const size_t position = value.find(from);
        EXPECT_NE(position, std::string::npos);
        if (position != std::string::npos) value.replace(position, from.size(), to);
        return value;
    }
} // namespace

TEST(ResourceContentIdentityValidation, CookedAndManifestDomainsPreserveExistingIdentityRules)
{
    ResourceContentIdentity identity;
    identity.schemaVersion = RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
    identity.domain = ResourceContentIdentityDomain::CookedArtifact;
    identity.scope = ResourceContentIdentityScope::SelfContainedArtifact;
    identity.algorithm = ResourceContentHashAlgorithm::SHA256;
    identity.digest = std::string(64, 'a');
    identity.fileCount = 1;
    EXPECT_TRUE(identity.IsValid());
    EXPECT_STREQ(GetResourceContentIdentityDomainName(identity.domain), "cooked-artifact");

    identity.domain = ResourceContentIdentityDomain::CookManifest;
    EXPECT_TRUE(identity.IsValid());
    EXPECT_STREQ(GetResourceContentIdentityDomainName(identity.domain), "cook-manifest");
}

TEST_F(CookManifestValidationFixture, RoundTripCanonicalizesRootsAndPreservesRawManifestIdentity)
{
    const CookManifest source = MakeManifest();
    const std::string text = SaveText(source);
    EXPECT_NE(text.find("sourceRoot=.\n"), std::string::npos);
    EXPECT_NE(text.find("outputRoot=.\n"), std::string::npos);

    CookManifest parsed;
    std::string error;
    ASSERT_TRUE(ParseCookManifest(text, parsed, error)) << error;
    EXPECT_EQ(parsed.observedSourceRoot, ".");
    EXPECT_EQ(parsed.observedOutputRoot, ".");
    EXPECT_TRUE(parsed.manifestContentIdentity.IsValid());
    EXPECT_EQ(parsed.observedMetrics.entryCount, 1u);
    EXPECT_EQ(parsed.observedMetrics.dependencyCount, 1u);
    EXPECT_EQ(parsed.observedMetrics.artifactCount, 2u);

    ResourceContentIdentity firstClosure;
    ASSERT_TRUE(ComputeCookedContentIdentity(parsed, firstClosure, error)) << error;
    const std::string roundTrip = SaveText(parsed);
    CookManifest reparsed;
    ASSERT_TRUE(ParseCookManifest(roundTrip, reparsed, error)) << error;
    ResourceContentIdentity secondClosure;
    ASSERT_TRUE(ComputeCookedContentIdentity(reparsed, secondClosure, error)) << error;
    EXPECT_EQ(firstClosure, secondClosure);
    EXPECT_EQ(parsed.manifestContentIdentity, reparsed.manifestContentIdentity);
}

TEST_F(CookManifestValidationFixture, CurrentV2WriterRootsAreObservationsAndDoNotChangeClosureIdentity)
{
    const std::string canonical = SaveText(MakeManifest());
    const std::string writerCompatible = ReplaceOne(
        ReplaceOne(canonical, "sourceRoot=.\n", "sourceRoot=C:\\\\agent\\\\source\n"),
        "outputRoot=.\n", "outputRoot=D:\\\\agent\\\\cooked\n");
    CookManifest fromCanonical;
    CookManifest fromWriter;
    std::string error;
    ASSERT_TRUE(ParseCookManifest(canonical, fromCanonical, error)) << error;
    ASSERT_TRUE(ParseCookManifest(writerCompatible, fromWriter, error)) << error;
    EXPECT_EQ(fromWriter.observedSourceRoot, "C:\\agent\\source");
    EXPECT_EQ(fromWriter.observedOutputRoot, "D:\\agent\\cooked");
    ResourceContentIdentity canonicalClosure;
    ResourceContentIdentity writerClosure;
    ASSERT_TRUE(ComputeCookedContentIdentity(fromCanonical, canonicalClosure, error)) << error;
    ASSERT_TRUE(ComputeCookedContentIdentity(fromWriter, writerClosure, error)) << error;
    EXPECT_EQ(canonicalClosure, writerClosure);
    EXPECT_NE(fromCanonical.manifestContentIdentity, fromWriter.manifestContentIdentity);
}

TEST_F(CookManifestValidationFixture, ParserRejectsDuplicateUnknownMissingEscapedAndUnorderedFields)
{
    const std::string text = SaveText(MakeManifest());
    CookManifest parsed;
    std::string error;

    EXPECT_FALSE(ParseCookManifest(ReplaceOne(text, "toolName=RVXCook\n", "toolName=RVXCook\ntoolName=RVXCook\n"), parsed, error));
    EXPECT_FALSE(ParseCookManifest(ReplaceOne(text, "RVX_COOK_MANIFEST_END\n", "unknown=value\nRVX_COOK_MANIFEST_END\n"), parsed, error));
    EXPECT_FALSE(ParseCookManifest(ReplaceOne(text, "toolVersion=2.0.0\n", ""), parsed, error));
    EXPECT_FALSE(ParseCookManifest(ReplaceOne(text, "toolName=RVXCook\n", "toolName=RVX\\q\n"), parsed, error));
    EXPECT_FALSE(ParseCookManifest(ReplaceOne(text, "models/model.gltf", "../model.gltf"), parsed, error));
    EXPECT_FALSE(ParseCookManifest(ReplaceOne(text, "successCount=1\n", "successCount=0\n"), parsed, error));
    EXPECT_FALSE(ParseCookManifest(ReplaceOne(text, "success=1\n", "success=0\n"), parsed, error));

    CookManifest unordered = MakeManifest();
    unordered.entries.front().dependencies.push_back(
        MakeIdentity("aaa/early.rva", "late"));
    unordered.entries.front().recipeHash = BuildRecipeHash(unordered.entries.front());
    EXPECT_FALSE(SaveCookManifest(m_root / "unordered.rvxmanifest", unordered, error));
}

TEST_F(CookManifestValidationFixture, AdmissionVerifiesMountedContentAndEveryExpectation)
{
    const std::string text = SaveText(MakeManifest());
    CookManifest manifest;
    std::string error;
    ASSERT_TRUE(ParseCookManifest(text, manifest, error)) << error;
    ResourceContentIdentity cooked;
    ASSERT_TRUE(ComputeCookedContentIdentity(manifest, cooked, error)) << error;

    CookManifestExpectation expected;
    expected.selector.outputPath = "models/model.rva";
    expected.selector.type = CookAssetType::Model;
    expected.expectedSourceContentIdentity = MakeSourceContentIdentity(manifest.entries.front().sourceContent);
    expected.expectedCookedContentIdentity = cooked;
    expected.expectedManifestContentIdentity = manifest.manifestContentIdentity;
    expected.expectedCookSettingsHash = manifest.entries.front().cookSettingsHash;
    expected.expectedRecipeHash = manifest.entries.front().recipeHash;
    EXPECT_TRUE(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, expected).IsAccepted());

    CookManifestExpectation wrongSelector = expected;
    wrongSelector.selector.outputPath = "missing.rva";
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, wrongSelector).code,
              CookManifestAdmissionCode::SelectorNotFound);

    CookManifestExpectation wrongTool = expected;
    wrongTool.requiredToolVersion = "2.0.1";
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, wrongTool).code,
              CookManifestAdmissionCode::ToolMismatch);

    CookManifestExpectation wrongSource = expected;
    wrongSource.expectedSourceContentIdentity.digest[0] = 'b';
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, wrongSource).code,
              CookManifestAdmissionCode::SourceIdentityMismatch);

    CookManifestExpectation wrongCooked = expected;
    wrongCooked.expectedCookedContentIdentity.digest[0] = 'b';
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, wrongCooked).code,
              CookManifestAdmissionCode::CookedIdentityMismatch);

    CookManifestExpectation wrongManifest = expected;
    wrongManifest.expectedManifestContentIdentity.digest[0] = 'b';
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, wrongManifest).code,
              CookManifestAdmissionCode::ManifestIdentityMismatch);

    CookManifestExpectation wrongSettings = expected;
    wrongSettings.expectedCookSettingsHash[0] = 'b';
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, wrongSettings).code,
              CookManifestAdmissionCode::CookSettingsMismatch);

    CookManifestExpectation wrongRecipe = expected;
    wrongRecipe.expectedRecipeHash[0] = 'b';
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, wrongRecipe).code,
              CookManifestAdmissionCode::RecipeMismatch);

    fs::remove(m_cookedRoot / "models/model.rva");
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, expected).code,
              CookManifestAdmissionCode::MountedContentMismatch);
    WriteFile(m_cookedRoot / "models/model.rva", "tampered-artifact");
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, expected).code,
              CookManifestAdmissionCode::MountedContentMismatch);
}

TEST_F(CookManifestValidationFixture, AdmissionFailsClosedWhenMountedSymlinkEscapesTheCookedRoot)
{
    const fs::path outsideRoot = m_root / "outside";
    WriteFile(outsideRoot / "escape.rva", "outside-product");
    std::error_code linkError;
    fs::create_directory_symlink(outsideRoot, m_cookedRoot / "zzz", linkError);
    if (linkError)
    {
        GTEST_SKIP() << "The current test host does not permit directory symlink creation: " << linkError.message();
    }

    CookManifest manifest = MakeManifest();
    CookManifestEntry& entry = manifest.entries.front();
    entry.artifacts.push_back(MakeIdentity("zzz/escape.rva", "outside-product"));
    entry.recipeHash = BuildRecipeHash(entry);
    std::string error;
    ASSERT_TRUE(manifest.IsSemanticallyValid(error)) << error;
    CookManifestExpectation expected;
    expected.selector.outputPath = entry.outputPath;
    EXPECT_EQ(VerifyCookedAssetAdmission(manifest, m_sourceRoot, m_cookedRoot, expected).code,
              CookManifestAdmissionCode::MountedContentMismatch);
}

TEST_F(CookManifestValidationFixture,
       SourceDependencyClosureIsExactAndMountedSourceTamperingFailsClosed)
{
    WriteFile(m_sourceRoot / "models/Model.bin", "buffer-v1");
    WriteFile(m_sourceRoot / "models/Textures/albedo.png", "image-v1");

    CookManifest manifest = MakeManifest();
    CookManifestEntry& entry = manifest.entries.front();
    entry.sourceDependencyClosureRecorded = true;
    entry.sourceDependencies = {
        MakeIdentity("models/Model.bin", "buffer-v1"),
        MakeIdentity("models/Textures/albedo.png", "image-v1"),
    };
    entry.recipeHash = BuildRecipeHash(entry);

    const std::string text = SaveText(manifest);
    CookManifest parsed;
    std::string error;
    ASSERT_TRUE(ParseCookManifest(text, parsed, error)) << error;
    ASSERT_EQ(parsed.entries.front().sourceDependencies.size(), 2u);
    EXPECT_TRUE(parsed.entries.front().sourceDependencyClosureRecorded);

    CookManifestExpectation selector;
    selector.selector.outputPath = "models/model.rva";
    const CookManifestAdmissionReceipt observed =
        VerifyCookedAssetAdmission(parsed, m_sourceRoot, m_cookedRoot, selector);
    ASSERT_TRUE(observed.IsAccepted()) << observed.detail;
    EXPECT_EQ(observed.observedSourceContentIdentity.scope,
              ResourceContentIdentityScope::DependencyClosure);
    EXPECT_EQ(observed.observedSourceContentIdentity.fileCount, 3u);

    selector.expectedSourceContentIdentity = observed.observedSourceContentIdentity;
    EXPECT_TRUE(VerifyCookedAssetAdmission(parsed, m_sourceRoot, m_cookedRoot, selector).IsAccepted());

    WriteFile(m_sourceRoot / "models/Model.bin", "buffer-v2");
    EXPECT_EQ(VerifyCookedAssetAdmission(parsed, m_sourceRoot, m_cookedRoot, selector).code,
              CookManifestAdmissionCode::MountedContentMismatch);

    CookManifest escapeManifest = manifest;
    escapeManifest.entries.front().sourceDependencies.front().relativePath = "../escape.bin";
    escapeManifest.entries.front().recipeHash = BuildRecipeHash(escapeManifest.entries.front());
    EXPECT_FALSE(escapeManifest.IsSemanticallyValid(error));
}
