#include "Samples/SampleEnvironmentLoader.h"
#include "Samples/SampleHostComposition.h"

#include "AssetImport/EnvironmentLoader.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
    using namespace RVX;

    Resource::ResourceContentIdentity MakeIdentity()
    {
        Resource::ResourceContentIdentity identity;
        identity.schemaVersion = Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = Resource::ResourceContentIdentityDomain::Source;
        identity.scope = Resource::ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = Resource::ResourceContentHashAlgorithm::SHA256;
        identity.digest = "0bef2e59d7a45a6a3918bef0833263967f5d403c45725b5bc0c303dc48e8c3a7";
        identity.byteCount = 17;
        identity.fileCount = 1;
        return identity;
    }

    Resource::ResourceContentIdentity MakeCookedIdentity()
    {
        Resource::ResourceContentIdentity identity = MakeIdentity();
        identity.domain =
            Resource::ResourceContentIdentityDomain::CookedArtifact;
        return identity;
    }

    class TestSampleHostComposition final : public ISampleHostComposition
    {
    public:
        [[nodiscard]] bool RequiresCookedOnly() const noexcept override
        {
            return false;
        }

        bool CreateEngineRuntimeDependencies(
            const SampleRuntimeCompositionEvidence&,
            EngineRuntimeDependencies&,
            std::string& outError) const override
        {
            outError = "Not exercised by SampleEnvironmentLoader validation.";
            return false;
        }

        bool CreateResourceManagerConfig(
            const SampleRuntimeCompositionEvidence&,
            Resource::ResourceManagerConfig&,
            std::string& outError) const override
        {
            outError = "Not exercised by SampleEnvironmentLoader validation.";
            return false;
        }

        bool PrepareSourceEnvironment(
            const SampleSourceEnvironmentPreparationRequest& request,
            uint64& outCanonicalImportOptionsHash,
            Resource::ResourceLoadPreparationStateRef& outState,
            Resource::ResourceLoadError& outError) const override
        {
            Resource::EnvironmentLoadOptions options;
            if (request.quality == "low")
            {
                options.quality = Resource::HDRIBLQualityProfile::Low;
            }
            else if (request.quality == "high")
            {
                options.quality = Resource::HDRIBLQualityProfile::High;
            }
            else if (request.quality == "default")
            {
                options.quality = Resource::HDRIBLQualityProfile::Default;
            }
            else
            {
                outCanonicalImportOptionsHash = 0;
                outState.reset();
                outError = {Resource::ResourceLoadErrorCode::InvalidRequest,
                            "Environment quality must be one of: default, low, high."};
                return false;
            }
            if (request.smoke)
            {
                options.quality = Resource::HDRIBLQualityProfile::Validation;
            }
            options.exposure = request.exposure;
            options.applyGamma = false;
            outState = Resource::EnvironmentLoader::CreatePreparationState(
                options, outCanonicalImportOptionsHash, outError);
            return outState != nullptr;
        }
    };

    class RecordingRuntimeServices final : public IWorldEcsRuntimeServices
    {
    public:
        ResourceSceneAdapters::EcsModelAssetLoadRef RequestModel(
            ResourceSceneAdapters::EcsModelAssetLoadDesc,
            std::string&) override
        {
            return {};
        }

        bool CancelModel(ResourceSceneAdapters::EcsModelAssetLoadRef) override
        {
            return false;
        }

        std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus> GetModelStatus(
            ResourceSceneAdapters::EcsModelAssetLoadRef) const override
        {
            return std::nullopt;
        }

        ResourceSceneAdapters::EcsEnvironmentLoadRef RequestEnvironment(
            ResourceSceneAdapters::EcsEnvironmentLoadDesc desc,
            std::string& outError) override
        {
            outError.clear();
            requests.push_back(std::move(desc));
            const auto handle = ResourceSceneAdapters::EcsEnvironmentLoadHandle::Create(
                static_cast<uint32>(requests.size() - 1u), 1u);
            ResourceSceneAdapters::EcsEnvironmentLoadStatus status;
            status.sceneRuntimeId = requests.back().expectedSceneRuntimeId;
            statuses.emplace_back(handle, std::move(status));
            return {.sceneRuntimeId = requests.back().expectedSceneRuntimeId, .handle = handle};
        }

        bool CancelEnvironment(
            ResourceSceneAdapters::EcsEnvironmentLoadRef request) override
        {
            if (!request.IsValid() || !cancelResult)
            {
                return false;
            }
            for (const auto& [candidate, status] : statuses)
            {
                if (candidate == request.handle && status.sceneRuntimeId == request.sceneRuntimeId)
                {
                    cancelled.push_back(request);
                    return true;
                }
            }
            return false;
        }

        std::optional<ResourceSceneAdapters::EcsEnvironmentLoadStatus>
        GetEnvironmentStatus(
            ResourceSceneAdapters::EcsEnvironmentLoadRef request) const override
        {
            for (const auto& [candidate, status] : statuses)
            {
                if (request.IsValid() && candidate == request.handle &&
                    status.sceneRuntimeId == request.sceneRuntimeId)
                {
                    return status;
                }
            }
            return std::nullopt;
        }

        AnimationSceneAdapters::EcsAnimationAssetLoadRef RequestAnimation(
            AnimationSceneAdapters::EcsAnimationAssetLoadDesc,
            std::string&) override
        {
            return {};
        }

        bool CancelAnimation(AnimationSceneAdapters::EcsAnimationAssetLoadRef) override
        {
            return false;
        }

        std::optional<AnimationSceneAdapters::EcsAnimationAssetLoadStatus>
        GetAnimationStatus(AnimationSceneAdapters::EcsAnimationAssetLoadRef) const override
        {
            return std::nullopt;
        }

        AnimationSceneAdapters::EcsAnimationBindingPreparationResult
        PrepareCompatibleAnimationBinding(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef,
            SceneECS::SceneEntityRef,
            uint32) override
        {
            return {};
        }

        WorldEcsAnimationPhysicsBindingResult BindAnimationRootMotionPhysics(
            SceneECS::SceneEntityRef) override
        {
            return {};
        }

        WorldEcsRuntimeServicesDiagnostics GetRuntimeDiagnostics() const override
        {
            return {};
        }

        void Publish(ResourceSceneAdapters::EcsEnvironmentLoadRef request,
                     ResourceSceneAdapters::EcsEnvironmentLoadStatus status)
        {
            for (auto& [candidate, stored] : statuses)
            {
                if (candidate == request.handle)
                {
                    stored = std::move(status);
                    return;
                }
            }
            ADD_FAILURE() << "Unknown environment handle in test fixture.";
        }

        std::vector<ResourceSceneAdapters::EcsEnvironmentLoadDesc> requests;
        std::vector<std::pair<ResourceSceneAdapters::EcsEnvironmentLoadHandle,
                              ResourceSceneAdapters::EcsEnvironmentLoadStatus>> statuses;
        std::vector<ResourceSceneAdapters::EcsEnvironmentLoadRef> cancelled;
        bool cancelResult = true;
    };

    SampleAssetRegistry MakeRegistry(const std::filesystem::path& path,
                                     const Resource::ResourceContentIdentity& identity)
    {
        SampleAssetRegistryEntry environment;
        environment.id = "studio";
        environment.kind = SampleAssetKind::Environment;
        environment.resolvedPath = path;
        environment.contentIdentity = identity;
        return SampleAssetRegistry({std::move(environment)});
    }
} // namespace

TEST(EcsSampleEnvironmentLoaderValidation,
     CatalogRequestsCarryExactSceneAndDistinctLowSmokeHighPreparationProfiles)
{
    RecordingRuntimeServices services;
    TestSampleHostComposition composition;
    const ECS::SceneRuntimeId sceneRuntimeId(104);
    const Resource::ResourceContentIdentity identity = MakeIdentity();
    SampleEnvironmentLoader loader(
        services, sceneRuntimeId, MakeRegistry("studio.hdr", identity), composition);

    LoadedSampleEnvironment low;
    LoadedSampleEnvironment high;
    LoadedSampleEnvironment smoke;
    std::string error;

    SampleEnvironmentLoadOptions lowOptions;
    lowOptions.quality = "low";
    lowOptions.exposure = 1.25f;
    ASSERT_TRUE(loader.RequestByAssetId("studio", lowOptions, low, error)) << error;

    SampleEnvironmentLoadOptions highOptions;
    highOptions.quality = "high";
    highOptions.exposure = 2.0f;
    ASSERT_TRUE(loader.Request("presentation.exr", identity, highOptions, high, error)) << error;

    SampleEnvironmentLoadOptions smokeOptions;
    smokeOptions.quality = "high";
    smokeOptions.smoke = true;
    smokeOptions.exposure = 1.5f;
    ASSERT_TRUE(loader.Request("smoke.hdr", identity, smokeOptions, smoke, error)) << error;

    ASSERT_EQ(services.requests.size(), 3u);
    const auto& lowDesc = services.requests[0];
    const auto& highDesc = services.requests[1];
    const auto& smokeDesc = services.requests[2];
    EXPECT_EQ(lowDesc.expectedSceneRuntimeId, sceneRuntimeId);
    EXPECT_EQ(highDesc.expectedSceneRuntimeId, sceneRuntimeId);
    EXPECT_EQ(smokeDesc.expectedSceneRuntimeId, sceneRuntimeId);
    const auto lowState = std::dynamic_pointer_cast<
        const Resource::EnvironmentPreparationState>(lowDesc.preparationState);
    const auto highState = std::dynamic_pointer_cast<
        const Resource::EnvironmentPreparationState>(highDesc.preparationState);
    const auto smokeState = std::dynamic_pointer_cast<
        const Resource::EnvironmentPreparationState>(smokeDesc.preparationState);
    ASSERT_NE(lowState, nullptr);
    ASSERT_NE(highState, nullptr);
    ASSERT_NE(smokeState, nullptr);
    EXPECT_EQ(lowState->sourceOptions.quality, Resource::HDRIBLQualityProfile::Low);
    EXPECT_EQ(highState->sourceOptions.quality, Resource::HDRIBLQualityProfile::High);
    EXPECT_EQ(smokeState->sourceOptions.quality,
              Resource::HDRIBLQualityProfile::Validation);
    EXPECT_FLOAT_EQ(lowState->sourceOptions.exposure, 1.25f);
    EXPECT_FLOAT_EQ(highState->sourceOptions.exposure, 2.0f);
    EXPECT_FLOAT_EQ(smokeState->sourceOptions.exposure, 1.5f);
    EXPECT_EQ(lowDesc.resourceOptions.expectedContentIdentity, identity);
    EXPECT_EQ(highDesc.resourceOptions.expectedContentIdentity, identity);
    EXPECT_EQ(smokeDesc.resourceOptions.expectedContentIdentity, identity);
    EXPECT_NE(lowState->sourceOptions.quality, highState->sourceOptions.quality);
    EXPECT_NE(smokeState->sourceOptions.quality, highState->sourceOptions.quality);
}

TEST(EcsSampleEnvironmentLoaderValidation,
     PackageOnlyCookedEnvironmentUsesMountedRuntimePathWithoutSourcePreparation)
{
    RecordingRuntimeServices services;
    TestSampleHostComposition composition;
    const ECS::SceneRuntimeId sceneRuntimeId(108);
    const Resource::ResourceContentIdentity identity = MakeCookedIdentity();
    SampleAssetRegistryEntry entry;
    entry.id = "studio-cooked";
    entry.kind = SampleAssetKind::Environment;
    entry.resolvedPath = "cooked/studio-cooked/environment.rva";
    entry.resourcePath =
        "cooked://cooked/studio-cooked/environment.rva";
    entry.requiresCookedResourcePath = true;
    entry.contentIdentity = identity;
    entry.packageOnly = true;
    SampleEnvironmentLoader loader(
        services, sceneRuntimeId, SampleAssetRegistry({entry}), composition);

    LoadedSampleEnvironment environment;
    std::string error;
    ASSERT_TRUE(loader.RequestByAssetId(
        "studio-cooked", {}, environment, error)) << error;

    ASSERT_EQ(services.requests.size(), 1u);
    const auto& request = services.requests.front();
    EXPECT_EQ(request.path,
              "cooked://cooked/studio-cooked/environment.rva");
    EXPECT_EQ(request.expectedSceneRuntimeId, sceneRuntimeId);
    EXPECT_EQ(request.resourceOptions.importOptionsHash, 0u);
    EXPECT_EQ(request.resourceOptions.expectedContentIdentity, identity);
    EXPECT_EQ(request.preparationState, nullptr);
}

TEST(EcsSampleEnvironmentLoaderValidation,
     PackageOnlyEnvironmentRejectsNonCookedArtifactBeforeRuntimeServices)
{
    RecordingRuntimeServices services;
    TestSampleHostComposition composition;
    const ECS::SceneRuntimeId sceneRuntimeId(109);
    SampleAssetRegistryEntry entry;
    entry.id = "invalid-package";
    entry.kind = SampleAssetKind::Environment;
    entry.resolvedPath = "cooked/invalid-package/environment.hdr";
    entry.resourcePath =
        "cooked://cooked/invalid-package/environment.hdr";
    entry.requiresCookedResourcePath = true;
    entry.contentIdentity = MakeIdentity();
    entry.packageOnly = true;
    SampleEnvironmentLoader loader(
        services, sceneRuntimeId, SampleAssetRegistry({entry}), composition);

    LoadedSampleEnvironment output;
    std::string error;
    EXPECT_FALSE(loader.RequestByAssetId(
        "invalid-package", {}, output, error));
    EXPECT_NE(error.find("CookedArtifact .rva"), std::string::npos);
    EXPECT_TRUE(services.requests.empty());
}

TEST(EcsSampleEnvironmentLoaderValidation,
     PublishedValueStatusCarriesVerificationAndIblMetadataWithoutResourceHandles)
{
    RecordingRuntimeServices services;
    TestSampleHostComposition composition;
    const ECS::SceneRuntimeId sceneRuntimeId(105);
    const Resource::ResourceContentIdentity identity = MakeIdentity();
    SampleEnvironmentLoader loader(
        services, sceneRuntimeId, MakeRegistry("proof.hdr", identity), composition);

    LoadedSampleEnvironment environment;
    std::string error;
    ASSERT_TRUE(loader.RequestByAssetId(
        "studio", {}, environment, error)) << error;

    ResourceSceneAdapters::EcsEnvironmentLoadStatus published;
    published.state = ResourceSceneAdapters::EcsEnvironmentLoadState::FullyResident;
    published.sceneRuntimeId = sceneRuntimeId;
    published.canonicalImportOptionsHash = Resource::EnvironmentLoader::ComputeImportOptionsHash(
        {.quality = Resource::HDRIBLQualityProfile::Default, .exposure = 1.75f});
    published.contentVerificationReceipt = Resource::ResourceContentVerificationReceipt{
        .status = Resource::ResourceContentVerificationStatus::Verified,
        .expected = identity,
        .observed = identity};
    published.environmentResolution = 64;
    published.irradianceResolution = 16;
    published.prefilteredResolution = 64;
    published.prefilteredMipLevels = 6;
    published.brdfLUTResolution = 64;
    published.exposure = 1.75f;
    services.Publish(environment.request, published);

    const auto status = loader.UpdateReadiness(environment);
    EXPECT_EQ(status.state, ResourceSceneAdapters::EcsEnvironmentLoadState::FullyResident);
    ASSERT_TRUE(environment.contentVerificationReceipt.has_value());
    EXPECT_TRUE(environment.contentVerificationReceipt->IsVerified());
    EXPECT_EQ(environment.environmentResolution, 64u);
    EXPECT_EQ(environment.irradianceResolution, 16u);
    EXPECT_EQ(environment.prefilteredResolution, 64u);
    EXPECT_EQ(environment.prefilteredMipLevels, 6u);
    EXPECT_EQ(environment.brdfLUTResolution, 64u);
    EXPECT_FLOAT_EQ(environment.exposure, 1.75f);
    EXPECT_TRUE(environment.IsCPUReady());
    EXPECT_TRUE(environment.IsValid());

    const auto receipt = loader.GetContentVerificationReceipt("proof.hdr");
    ASSERT_TRUE(receipt.has_value());
    EXPECT_EQ(receipt->status, environment.contentVerificationReceipt->status);
    EXPECT_EQ(receipt->expected, environment.contentVerificationReceipt->expected);
    EXPECT_EQ(receipt->observed, environment.contentVerificationReceipt->observed);

    const auto request = environment.request;
    EXPECT_TRUE(loader.Cancel(environment));
    EXPECT_FALSE(environment.request.IsValid());
    ASSERT_EQ(services.cancelled.size(), 1u);
    EXPECT_EQ(services.cancelled.front(), request);
}

TEST(EcsSampleEnvironmentLoaderValidation,
     InvalidCatalogAndSceneIdentityRequestsDoNotReachRuntimeServices)
{
    RecordingRuntimeServices services;
    TestSampleHostComposition composition;
    const Resource::ResourceContentIdentity identity = MakeIdentity();
    SampleEnvironmentLoader loader(
        services, ECS::SceneRuntimeId{}, MakeRegistry("studio.hdr", identity), composition);

    LoadedSampleEnvironment output;
    std::string error;
    SampleAssetRegistryLookupResult lookup;
    EXPECT_FALSE(loader.RequestByAssetId("missing", {}, output, error, &lookup));
    EXPECT_EQ(lookup.code, SampleAssetRegistryLookupCode::UnknownAssetId);
    EXPECT_TRUE(services.requests.empty());

    EXPECT_FALSE(loader.Request("studio.hdr", identity, {}, output, error));
    EXPECT_TRUE(services.requests.empty());
}

TEST(EcsSampleEnvironmentLoaderValidation,
     RequestRefsRequireContentIdentityAndPreserveLiveEvidenceOnFailure)
{
    RecordingRuntimeServices services;
    TestSampleHostComposition composition;
    const ECS::SceneRuntimeId sceneRuntimeId(106);
    const Resource::ResourceContentIdentity identity = MakeIdentity();
    SampleEnvironmentLoader loader(
        services, sceneRuntimeId, MakeRegistry("identity.hdr", identity), composition);
    LoadedSampleEnvironment environment;
    std::string error;
    ASSERT_TRUE(loader.RequestByAssetId("studio", {}, environment, error)) << error;
    const auto originalRequest = environment.request;

    EXPECT_FALSE(loader.RequestByAssetId("studio", {}, environment, error));
    EXPECT_EQ(environment.request, originalRequest);
    EXPECT_EQ(services.requests.size(), 1u);

    Resource::ResourceContentIdentity invalidIdentity;
    LoadedSampleEnvironment invalidOutput;
    EXPECT_FALSE(loader.Request("unverified.hdr", invalidIdentity, {}, invalidOutput, error));
    EXPECT_TRUE(services.requests.size() == 1u);

    SampleEnvironmentLoadOptions unknownQuality;
    unknownQuality.quality = "ultra";
    unknownQuality.smoke = true;
    EXPECT_FALSE(loader.Request("unknown-quality.hdr", identity, unknownQuality, invalidOutput, error));
    EXPECT_TRUE(services.requests.size() == 1u);

    ResourceSceneAdapters::EcsEnvironmentLoadStatus foreignStatus;
    foreignStatus.sceneRuntimeId = ECS::SceneRuntimeId(9999);
    services.Publish(originalRequest, foreignStatus);
    const auto updated = loader.UpdateReadiness(environment);
    EXPECT_EQ(updated.state, ResourceSceneAdapters::EcsEnvironmentLoadState::Failed);
    EXPECT_EQ(environment.request, originalRequest);

    services.cancelResult = false;
    EXPECT_FALSE(loader.Cancel(environment));
    EXPECT_EQ(environment.request, originalRequest);
}

TEST(EcsSampleEnvironmentLoaderValidation,
     CatalogEntryWithoutContentIdentityIsRejectedBeforeRuntimeService)
{
    RecordingRuntimeServices services;
    TestSampleHostComposition composition;
    const ECS::SceneRuntimeId sceneRuntimeId(107);
    SampleAssetRegistryEntry entry;
    entry.id = "unverified";
    entry.kind = SampleAssetKind::Environment;
    entry.resolvedPath = "unverified.hdr";
    SampleEnvironmentLoader loader(
        services, sceneRuntimeId, SampleAssetRegistry({entry}), composition);

    LoadedSampleEnvironment output;
    std::string error;
    EXPECT_FALSE(loader.RequestByAssetId("unverified", {}, output, error));
    EXPECT_FALSE(output.request.IsValid());
    EXPECT_TRUE(services.requests.empty());
}
