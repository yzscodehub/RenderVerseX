#include "Samples/SampleAnimationLoader.h"

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
        identity.digest =
            "15a36e86f826434396b55cc49f9a8f6b941fb08f1c81f6dedf064688f077a84f";
        identity.byteCount = 29;
        identity.fileCount = 1;
        return identity;
    }

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
            ResourceSceneAdapters::EcsEnvironmentLoadDesc,
            std::string&) override
        {
            return {};
        }

        bool CancelEnvironment(ResourceSceneAdapters::EcsEnvironmentLoadRef) override
        {
            return false;
        }

        std::optional<ResourceSceneAdapters::EcsEnvironmentLoadStatus>
        GetEnvironmentStatus(
            ResourceSceneAdapters::EcsEnvironmentLoadRef) const override
        {
            return std::nullopt;
        }

        AnimationSceneAdapters::EcsAnimationAssetLoadRef RequestAnimation(
            AnimationSceneAdapters::EcsAnimationAssetLoadDesc desc,
            std::string& outError) override
        {
            if (!requestSucceeds)
            {
                outError = "Fixture animation request failure.";
                return {};
            }
            outError.clear();
            requests.push_back(std::move(desc));
            const auto handle =
                AnimationSceneAdapters::EcsAnimationAssetLoadHandle::Create(
                    static_cast<uint32>(requests.size() - 1u), 1u);
            const AnimationSceneAdapters::EcsAnimationAssetLoadRef request{
                requests.back().expectedSceneRuntimeId, handle};
            statuses.emplace_back(
                request, AnimationSceneAdapters::EcsAnimationAssetLoadStatus{});
            statuses.back().second.sceneRuntimeId = request.sceneRuntimeId;
            return request;
        }

        bool CancelAnimation(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request) override
        {
            cancelled.push_back(request);
            return cancelResult;
        }

        std::optional<AnimationSceneAdapters::EcsAnimationAssetLoadStatus>
        GetAnimationStatus(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request) const override
        {
            for (const auto& [candidate, status] : statuses)
            {
                if (candidate == request)
                {
                    return status;
                }
            }
            return std::nullopt;
        }

        AnimationSceneAdapters::EcsAnimationBindingPreparationResult
        PrepareCompatibleAnimationBinding(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request,
            SceneECS::SceneEntityRef target,
            uint32 clipOrdinal) override
        {
            bindingRequests.push_back({request, target, clipOrdinal});
            AnimationSceneAdapters::EcsAnimationBindingPreparationResult result;
            result.code = bindingCode;
            return result;
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

        void Publish(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request,
            AnimationSceneAdapters::EcsAnimationAssetLoadStatus status)
        {
            for (auto& [candidate, stored] : statuses)
            {
                if (candidate == request)
                {
                    stored = std::move(status);
                    return;
                }
            }
            ADD_FAILURE() << "Unknown animation request in test fixture.";
        }

        struct BindingRequest
        {
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request;
            SceneECS::SceneEntityRef target;
            uint32 clipOrdinal = 0;
        };

        std::vector<AnimationSceneAdapters::EcsAnimationAssetLoadDesc> requests;
        std::vector<std::pair<
            AnimationSceneAdapters::EcsAnimationAssetLoadRef,
            AnimationSceneAdapters::EcsAnimationAssetLoadStatus>> statuses;
        std::vector<AnimationSceneAdapters::EcsAnimationAssetLoadRef> cancelled;
        std::vector<BindingRequest> bindingRequests;
        bool requestSucceeds = true;
        bool cancelResult = true;
        AnimationSceneAdapters::EcsAnimationBindingPreparationCode bindingCode =
            AnimationSceneAdapters::EcsAnimationBindingPreparationCode::Prepared;
    };

    SampleAssetRegistry MakeRegistry(
        const std::filesystem::path& path,
        const Resource::ResourceContentIdentity& identity)
    {
        SampleAssetRegistryEntry animation;
        animation.id = "walk";
        animation.kind = SampleAssetKind::Animation;
        animation.resolvedPath = path;
        animation.contentIdentity = identity;

        SampleAssetRegistryEntry model;
        model.id = "model";
        model.kind = SampleAssetKind::Model;
        model.resolvedPath = "model.glb";
        return SampleAssetRegistry({std::move(animation), std::move(model)});
    }
} // namespace

TEST(EcsSampleAnimationLoaderValidation,
     CatalogRequestCarriesExactSceneAndContentIdentityWithoutResourceHandles)
{
    RecordingRuntimeServices services;
    const ECS::SceneRuntimeId sceneRuntimeId(201);
    const Resource::ResourceContentIdentity identity = MakeIdentity();
    SampleAnimationLoader loader(
        services, sceneRuntimeId, MakeRegistry("walk.rvanim", identity));

    LoadedSampleAnimation animation;
    std::string error;
    ASSERT_TRUE(loader.RequestByAssetId("walk", animation, error)) << error;
    ASSERT_EQ(services.requests.size(), 1u);
    EXPECT_EQ(services.requests.front().path, "walk.rvanim");
    EXPECT_EQ(services.requests.front().expectedSceneRuntimeId, sceneRuntimeId);
    EXPECT_EQ(services.requests.front().resourceOptions.expectedContentIdentity,
              identity);
    EXPECT_EQ(animation.request.sceneRuntimeId, sceneRuntimeId);
    EXPECT_EQ(animation.sourcePath, std::filesystem::path("walk.rvanim"));

    const auto retainedRequest = animation.request;
    EXPECT_FALSE(loader.RequestByAssetId("walk", animation, error));
    EXPECT_EQ(animation.request, retainedRequest);

    LoadedSampleAnimation rejected;
    SampleAssetRegistryLookupResult lookup;
    EXPECT_FALSE(loader.RequestByAssetId(
        "missing", rejected, error, &lookup));
    EXPECT_EQ(lookup.code, SampleAssetRegistryLookupCode::UnknownAssetId);
    EXPECT_FALSE(loader.RequestByAssetId("model", rejected, error, &lookup));
    EXPECT_EQ(lookup.code, SampleAssetRegistryLookupCode::KindMismatch);
    EXPECT_EQ(services.requests.size(), 1u);
}

TEST(EcsSampleAnimationLoaderValidation,
     ReadyStatusAndCancellationRemainValueOnlyAndFailureRetainsEvidence)
{
    RecordingRuntimeServices services;
    const ECS::SceneRuntimeId sceneRuntimeId(202);
    const Resource::ResourceContentIdentity identity = MakeIdentity();
    SampleAnimationLoader loader(
        services, sceneRuntimeId, MakeRegistry("proof.rvanim", identity));

    LoadedSampleAnimation animation;
    std::string error;
    ASSERT_TRUE(loader.RequestByAssetId("walk", animation, error)) << error;

    AnimationSceneAdapters::EcsAnimationAssetLoadStatus ready;
    ready.state = AnimationSceneAdapters::EcsAnimationAssetLoadState::Ready;
    ready.resourceLoadState = Resource::ResourceLoadState::Ready;
    ready.sceneRuntimeId = sceneRuntimeId;
    ready.animationAssetValue = 77;
    ready.boneCount = 31;
    ready.contentVerification = {
        .status = Resource::ResourceContentVerificationStatus::Verified,
        .expected = identity,
        .observed = identity};
    ready.clips.push_back({.ordinal = 0,
                           .name = "Walk",
                           .durationUs = 1'250'000,
                           .hasRootMotion = true,
                           .rootMotionBoneName = "Root"});
    services.Publish(animation.request, ready);

    const auto status = loader.UpdateReadiness(animation);
    EXPECT_TRUE(animation.IsReady());
    EXPECT_EQ(status.boneCount, 31u);
    ASSERT_EQ(status.clips.size(), 1u);
    EXPECT_EQ(status.clips.front().name, "Walk");
    const auto receipt = loader.GetContentVerificationReceipt("proof.rvanim");
    ASSERT_TRUE(receipt.has_value());
    EXPECT_TRUE(receipt->IsVerified());

    services.cancelResult = false;
    const auto retainedRequest = animation.request;
    EXPECT_FALSE(loader.Cancel(animation));
    EXPECT_EQ(animation.request, retainedRequest);

    services.cancelResult = true;
    EXPECT_TRUE(loader.Cancel(animation));
    EXPECT_FALSE(animation.request.IsValid());
    ASSERT_EQ(services.cancelled.size(), 2u);
    EXPECT_EQ(services.cancelled.back(), retainedRequest);
}

TEST(EcsSampleAnimationLoaderValidation,
     BindingForwardsExactSceneTargetAndRejectsForeignTargetLocally)
{
    RecordingRuntimeServices services;
    const ECS::SceneRuntimeId sceneRuntimeId(203);
    SampleAnimationLoader loader(
        services, sceneRuntimeId, MakeRegistry("bind.rvanim", MakeIdentity()));

    LoadedSampleAnimation animation;
    std::string error;
    ASSERT_TRUE(loader.RequestByAssetId("walk", animation, error)) << error;

    const SceneECS::SceneEntityRef target{
        .sceneRuntimeId = sceneRuntimeId,
        .entity = ECS::EntityHandle::Create(9u, 4u)};
    const auto prepared = loader.PrepareCompatibleBinding(animation, target, 2u);
    EXPECT_TRUE(prepared.IsPrepared());
    ASSERT_EQ(services.bindingRequests.size(), 1u);
    EXPECT_EQ(services.bindingRequests.front().request, animation.request);
    EXPECT_EQ(services.bindingRequests.front().target, target);
    EXPECT_EQ(services.bindingRequests.front().clipOrdinal, 2u);

    const SceneECS::SceneEntityRef foreign{
        .sceneRuntimeId = ECS::SceneRuntimeId(204),
        .entity = target.entity};
    const auto rejected =
        loader.PrepareCompatibleBinding(animation, foreign, 0u);
    EXPECT_EQ(rejected.code,
              AnimationSceneAdapters::EcsAnimationBindingPreparationCode::
                  InvalidTargetEntity);
    EXPECT_EQ(services.bindingRequests.size(), 1u);

    const SceneECS::SceneEntityRef invalid{
        .sceneRuntimeId = sceneRuntimeId,
        .entity = ECS::EntityHandle::Invalid()};
    const auto invalidRejected =
        loader.PrepareCompatibleBinding(animation, invalid, 0u);
    EXPECT_EQ(invalidRejected.code,
              AnimationSceneAdapters::EcsAnimationBindingPreparationCode::
                  InvalidTargetEntity);
    EXPECT_EQ(services.bindingRequests.size(), 1u);
}

TEST(EcsSampleAnimationLoaderValidation,
     InvalidSceneIdentityNeverReachesRuntimeServices)
{
    RecordingRuntimeServices services;
    SampleAnimationLoader loader(
        services, ECS::SceneRuntimeId{},
        MakeRegistry("invalid.rvanim", MakeIdentity()));

    LoadedSampleAnimation animation;
    std::string error;
    EXPECT_FALSE(loader.RequestByAssetId("walk", animation, error));
    EXPECT_TRUE(services.requests.empty());
}

TEST(EcsSampleAnimationLoaderValidation,
     InvalidCatalogIdentityAndRuntimeFailurePreserveOutputEvidence)
{
    RecordingRuntimeServices services;
    const ECS::SceneRuntimeId sceneRuntimeId(205);
    SampleAssetRegistryEntry unverified;
    unverified.id = "unverified";
    unverified.kind = SampleAssetKind::Animation;
    unverified.resolvedPath = "unverified.rvanim";
    SampleAnimationLoader unverifiedLoader(
        services, sceneRuntimeId, SampleAssetRegistry({unverified}));

    LoadedSampleAnimation output;
    output.sourcePath = "previous.rvanim";
    std::string error;
    EXPECT_FALSE(unverifiedLoader.RequestByAssetId("unverified", output, error));
    EXPECT_EQ(output.sourcePath, std::filesystem::path("previous.rvanim"));
    EXPECT_FALSE(output.request.IsValid());
    EXPECT_TRUE(services.requests.empty());

    SampleAnimationLoader loader(
        services, sceneRuntimeId, MakeRegistry("failure.rvanim", MakeIdentity()));
    services.requestSucceeds = false;
    EXPECT_FALSE(loader.RequestByAssetId("walk", output, error));
    EXPECT_EQ(output.sourcePath, std::filesystem::path("previous.rvanim"));
    EXPECT_FALSE(output.request.IsValid());
    EXPECT_TRUE(services.requests.empty());
}
