#include "Scenes/WaterSurfaceSample.h"

#include "Resource/RuntimeResourcePolicy.h"
#include "Samples/SampleContext.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace
{
    constexpr std::string_view WaterFixtureId =
        "water-surface-reference-fixture-v1";

    TEST(WaterSurfaceSampleValidation, DeclaresOneFixedCookedPrimaryFixture)
    {
        const RVX::WaterSurfaceSample sample;
        const RVX::SampleInfo& info = sample.GetInfo();

        EXPECT_EQ(info.id, "water-surface");
        EXPECT_EQ(info.defaultAssetId, WaterFixtureId);
        EXPECT_EQ(info.assetPolicy, RVX::SampleAssetPolicy::Fixed);
        EXPECT_EQ(info.environmentPolicy, RVX::SampleEnvironmentPolicy::None);
        EXPECT_EQ(info.defaultRenderPath, RVX::SampleRenderPath::Direct);
        EXPECT_TRUE(info.supportsRenderPathSelection);
        EXPECT_TRUE(info.deterministicQualification);
        EXPECT_EQ(info.visualContractId, "water-surface-v1");
        EXPECT_TRUE(info.additionalDefaultAssetIds.empty());
        ASSERT_EQ(info.requiredCookedAssetIds.size(), 1u);
        EXPECT_EQ(info.requiredCookedAssetIds.front(), WaterFixtureId);
    }

    TEST(WaterSurfaceSampleValidation,
         DeclaresOnlyRendererExecutionGroundedWaterCapabilities)
    {
        const RVX::WaterSurfaceSample sample;
        const RVX::SampleAssessmentContract contract =
            sample.GetAssessmentContract();

        ASSERT_EQ(contract.code.GetValue(), "SAMPLE.WATER_SURFACE");
        ASSERT_EQ(contract.revision, "1");
        ASSERT_EQ(contract.capabilities.size(), 4u);
        for (const RVX::AssessmentCapability& capability : contract.capabilities)
        {
            EXPECT_TRUE(capability.gating);
            EXPECT_TRUE(capability.code.IsValid());
            EXPECT_FALSE(capability.blockingReason.empty());
        }

        const auto hasCapability = [&contract](const std::string_view code)
        {
            return std::any_of(contract.capabilities.begin(),
                               contract.capabilities.end(),
                               [code](const RVX::AssessmentCapability& capability)
                               {
                                   return capability.code.GetValue() == code;
                               });
        };
        EXPECT_TRUE(hasCapability("RENDER.CAPABILITY.GERSTNER_WATER_SURFACE"));
        EXPECT_TRUE(hasCapability("RENDER.CAPABILITY.WATER_DEPTH_THICKNESS"));
        EXPECT_TRUE(hasCapability("RENDER.CAPABILITY.WATER_BEER_LAMBERT"));
        EXPECT_TRUE(hasCapability("RENDER.CAPABILITY.WATER_REACTIVE_MASK"));
    }

    TEST(WaterSurfaceSampleValidation,
         RuntimeOnlyRegistryIssuesOnlyMountedRelativeCookedRequests)
    {
        RVX::SampleAssetRegistryEntry entry;
        entry.id = std::string(WaterFixtureId);
        entry.kind = RVX::SampleAssetKind::Model;
        entry.resolvedPath =
            "E:/mounted-samples/cooked/water-surface-reference-fixture-v1/"
            "models/water-surface-reference-fixture-v1.rva";
        entry.resourcePath =
            "cooked://cooked/water-surface-reference-fixture-v1/"
            "models/water-surface-reference-fixture-v1.rva";
        entry.requiresCookedResourcePath = true;
        entry.packageOnly = true;

        const RVX::SampleAssetRegistry registry({entry});
        const RVX::SampleAssetRegistryLookupResult lookup = registry.Lookup(
            WaterFixtureId, RVX::SampleAssetKind::Model);
        ASSERT_TRUE(lookup.IsFound());

        std::string resourcePath;
        std::string error;
        ASSERT_TRUE(RVX::TryGetSampleAssetResourcePath(
            *lookup.entry, resourcePath, error)) << error;
        EXPECT_EQ(resourcePath, entry.resourcePath);
        EXPECT_NE(resourcePath.find("E:/mounted-samples"), 0u);

        RVX::Resource::ResourceRuntimePolicy policy =
            RVX::Resource::MakeResourceRuntimePolicyForAppMode(RVX::AppMode::Runtime);
        policy.cookedRoot = "E:/mounted-samples";
        const RVX::Resource::ResourcePathResolution accepted =
            RVX::Resource::ResolveRuntimeResourcePath(
                policy, policy.cookedRoot, resourcePath);
        EXPECT_TRUE(accepted.allowed);
        EXPECT_TRUE(accepted.cookedArtifactRead);
        EXPECT_EQ(accepted.logicalPath,
                  "cooked/water-surface-reference-fixture-v1/"
                  "models/water-surface-reference-fixture-v1.rva");

        entry.resourcePath = "cooked://../outside.rva";
        EXPECT_FALSE(RVX::TryGetSampleAssetResourcePath(entry, resourcePath, error));
        entry.resourcePath = "E:/mounted-samples/cooked/outside.rva";
        EXPECT_FALSE(RVX::TryGetSampleAssetResourcePath(entry, resourcePath, error));
        entry.resourcePath = "source://cooked/reference.rva";
        EXPECT_FALSE(RVX::TryGetSampleAssetResourcePath(entry, resourcePath, error));
    }
} // namespace
