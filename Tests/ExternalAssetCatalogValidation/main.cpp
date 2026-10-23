#include "Samples/SampleAssetCatalog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
    using RVX::SampleAssetCatalog;
    using RVX::SampleAssetEntry;
    using RVX::SampleAssetUsagePolicy;
    using RVX::Resource::ResourceContentIdentityDomain;
    using RVX::Resource::ResourceContentIdentityScope;

    const std::filesystem::path kAssetRoot =
        std::filesystem::path(RVX_EXTERNAL_TEST_ASSET_ROOT);

    void ExpectOptionalNightly(const SampleAssetEntry& entry)
    {
        EXPECT_EQ(entry.usagePolicy, SampleAssetUsagePolicy::OptionalNightly);
        EXPECT_FALSE(entry.qualificationEligible);
        EXPECT_EQ(entry.license.coverage, "complete-declared-package");
        EXPECT_FALSE(entry.source.version.empty());
        EXPECT_FALSE(entry.attribution.empty());
        EXPECT_FALSE(entry.modificationNotice.empty());
        EXPECT_TRUE(entry.assetContentId.IsValid());
        EXPECT_EQ(entry.assetContentId.fileCount, entry.files.size());
    }
} // namespace

TEST(ExternalAssetCatalogValidation, LoadsCompleteOptionalExternalPackages)
{
    SampleAssetCatalog catalog;
    std::string error;
    ASSERT_TRUE(catalog.Load(kAssetRoot / "catalog.json", kAssetRoot, &error))
        << error;

    const SampleAssetEntry* porsche =
        catalog.Find("free-1975-porsche-911-930-turbo");
    ASSERT_NE(porsche, nullptr);
    ExpectOptionalNightly(*porsche);
    EXPECT_EQ(porsche->path.generic_string(),
              "models/free_1975_porsche_911_930_turbo/scene.gltf");
    EXPECT_EQ(porsche->assetContentId.fileCount, 30u);
    EXPECT_EQ(porsche->contentIdentity.domain,
              ResourceContentIdentityDomain::Source);
    EXPECT_EQ(porsche->contentIdentity.scope,
              ResourceContentIdentityScope::DependencyClosure);
    EXPECT_EQ(porsche->contentIdentity.fileCount, 29u);
    EXPECT_EQ(porsche->files.front().path.generic_string(),
              "models/free_1975_porsche_911_930_turbo/license.txt");
    EXPECT_EQ(porsche->files.back().path.generic_string(),
              "models/free_1975_porsche_911_930_turbo/textures/plate_normal.png");

    for (const char* id : {"cowboy-town-hall-8k", "valley-of-desolation-4k"})
    {
        const SampleAssetEntry* environment = catalog.Find(id);
        ASSERT_NE(environment, nullptr) << id;
        ExpectOptionalNightly(*environment);
        EXPECT_EQ(environment->assetContentId.fileCount, 2u);
        EXPECT_EQ(environment->contentIdentity.domain,
                  ResourceContentIdentityDomain::Source);
        EXPECT_EQ(environment->contentIdentity.scope,
                  ResourceContentIdentityScope::SelfContainedArtifact);
        EXPECT_EQ(environment->contentIdentity.fileCount, 1u);
    }
}
