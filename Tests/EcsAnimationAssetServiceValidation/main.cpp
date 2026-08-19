#include "AnimationSceneAdapters/ECS/EcsAnimationAssetService.h"

#include "Core/Log.h"
#include "Engine/Engine.h"
#include "Resource/PreparedResourceBundle.h"
#include "Resource/ResourceContentIdentity.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/AnimationResource.h"
#include "Resource/Types/ModelResource.h"
#include "WorldEcsRuntimeComposition.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

namespace
{
    using namespace RVX;

    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { Log::Initialize(); }
        void TearDown() override { Log::Shutdown(); }
    };

    [[maybe_unused]] const auto* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class ResourceSubsystemScope final
    {
    public:
        ResourceSubsystemScope()
        {
            Resource::ResourceManagerConfig config;
            config.asyncThreadCount = 1;
            config.runtimePolicy.mode = Resource::ResourceRuntimeMode::Editor;
            config.runtimePolicy.allowSourceAssetReads = true;
            m_resources.Initialize(config);
        }

        ~ResourceSubsystemScope()
        {
            m_resources.Deinitialize();
        }

        Resource::ResourceSubsystem& Get() { return m_resources; }

    private:
        Resource::ResourceSubsystem m_resources;
    };

    class RejectingEcsRenderTransport final : public IEcsRenderFrameTransport
    {
    public:
        EcsRenderFrameTransportReceipt TryPublish(EcsRenderFrameTransportCandidate candidate) noexcept override
        {
            return {
                .disposition = EcsRenderFrameTransportDisposition::NotAccepted,
                .candidateIdentity = candidate.candidateIdentity,
                .sourceSceneRuntimeId = candidate.sourceSceneRuntimeId,
                .sourceSnapshotRevision = candidate.sourceSnapshotRevision,
                .targetRenderSceneRevision = candidate.targetRenderSceneRevision,
                .frameSequence = candidate.frameSequence,
            };
        }
    };

    Animation::Skeleton::Ptr MakeSkeleton(float childBindTranslationX = 0.0f)
    {
        auto skeleton = Animation::Skeleton::Create();
        Animation::Bone root("root", -1);
        root.localBindPose = Animation::TransformSample::Identity();
        skeleton->AddBone(root);
        Animation::Bone child("child", 0);
        child.localBindPose = Animation::TransformSample::Identity();
        child.localBindPose.translation.x = childBindTranslationX;
        skeleton->AddBone(child);
        skeleton->BuildHierarchy();
        skeleton->ComputeInverseBindPoses();
        return skeleton;
    }

    Animation::AnimationClip::Ptr MakeClip(
        std::string name,
        const Animation::Skeleton::ConstPtr& skeleton,
        Animation::TimeUs duration,
        bool rootMotion)
    {
        auto clip = Animation::AnimationClip::Create(name);
        clip->description = name + " test clip";
        clip->duration = duration;
        clip->defaultWrapMode = Animation::WrapMode::Loop;
        clip->defaultSpeed = 1.0f;
        clip->metadata.sourceFps = 30;
        clip->metadata.sourceStartFrame = 0;
        clip->metadata.sourceEndFrame = 30;
        clip->metadata.sourceFormat = Animation::AnimationSourceFormat::Custom;
        clip->metadata.sourceFile = "service-test";
        clip->skeleton = skeleton;
        clip->hasRootMotion = rootMotion;
        clip->rootMotionBoneName = rootMotion ? "root" : "";

        Animation::TransformTrack track;
        track.targetName = "root";
        track.targetType = Animation::TrackTargetType::Bone;
        track.mode = Animation::TransformMode::TRS;
        track.translationKeyframes.emplace_back(0, Vec3(0.0f));
        track.translationKeyframes.emplace_back(duration, Vec3(0.0f, 0.0f, 1.0f));
        track.rotationKeyframes.emplace_back(0, Quat(1.0f, 0.0f, 0.0f, 0.0f));
        track.rotationKeyframes.emplace_back(duration, Quat(1.0f, 0.0f, 0.0f, 0.0f));
        track.scaleKeyframes.emplace_back(0, Vec3(1.0f));
        track.scaleKeyframes.emplace_back(duration, Vec3(1.0f));
        clip->transformTracks.push_back(std::move(track));
        return clip;
    }

    Resource::ResourceContentIdentity MakeObservedIdentity()
    {
        Resource::ResourceContentIdentity identity;
        identity.schemaVersion = Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = Resource::ResourceContentIdentityDomain::Source;
        identity.scope = Resource::ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = Resource::ResourceContentHashAlgorithm::SHA256;
        identity.digest = "f009e85dae4e41d6d1cc9ba534a5948090c986993523280fd7e513df87c40300";
        identity.byteCount = 32;
        identity.fileCount = 1;
        return identity;
    }

    bool SetAnimationPayload(Resource::AnimationResource& resource,
                             float childBindTranslationX = 0.0f)
    {
        const Animation::Skeleton::Ptr skeleton = MakeSkeleton(childBindTranslationX);
        Resource::AnimationResource::AnimationClipMap clips;
        clips.emplace("Idle", MakeClip("Idle", skeleton, 250'000, false));
        clips.emplace("Walk", MakeClip("Walk", skeleton, 500'000, true));
        return resource.SetData(skeleton, std::move(clips));
    }

    class AnimationTestLoader final : public Resource::IResourceLoader
    {
    public:
        Resource::ResourceType GetResourceType() const override
        {
            return Resource::ResourceType::Animation;
        }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".animsvc"};
        }

        Resource::IResource* Load(const std::string&) override { return nullptr; }

        bool Prepare(const Resource::ResourceLoadPreparationContext& context,
                     Resource::PreparedResourceBundle& outBundle,
                     Resource::ResourceLoadError& outError) override
        {
            ++prepareCount;
            auto* resource = new Resource::AnimationResource();
            resource->SetId(context.rootResourceId);
            resource->SetPath(context.requestedPath);
            resource->SetName("AnimationService");
            const float childBindTranslationX =
                context.requestedPath == "target-bind-mismatch.animsvc" ? 1.0f : 0.0f;
            if (!SetAnimationPayload(*resource, childBindTranslationX) ||
                !outBundle.SetRoot(Resource::ResourceHandle<Resource::IResource>(resource)) ||
                !outBundle.SetObservedContentIdentity(MakeObservedIdentity()))
            {
                outError = {Resource::ResourceLoadErrorCode::LoaderFailure,
                            "Test animation loader could not prepare an immutable resource."};
                return false;
            }
            return true;
        }

        bool SupportsPreparedLoading() const override { return true; }

        uint32 prepareCount = 0;
    };

    class ModelWithAnimationLoader final : public Resource::IResourceLoader
    {
    public:
        Resource::ResourceType GetResourceType() const override
        {
            return Resource::ResourceType::Model;
        }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".modelsvc"};
        }

        Resource::IResource* Load(const std::string&) override { return nullptr; }

        bool Prepare(const Resource::ResourceLoadPreparationContext& context,
                     Resource::PreparedResourceBundle& outBundle,
                     Resource::ResourceLoadError& outError) override
        {
            auto* animation = new Resource::AnimationResource();
            animation->SetId(GenerateResourceId(context.resourceIdentityPath + "#animation"));
            animation->SetPath(context.requestedPath + "#animation");
            animation->SetName("ContainedAnimation");
            if (!SetAnimationPayload(*animation))
            {
                delete animation;
                outError = {Resource::ResourceLoadErrorCode::LoaderFailure,
                            "Test model loader could not create its animation dependency."};
                return false;
            }

            auto* model = new Resource::ModelResource();
            model->SetId(context.rootResourceId);
            model->SetPath(context.requestedPath);
            model->SetName("ModelWithAnimation");
            model->SetAnimationResource(Resource::AnimationHandle(animation));
            if (!outBundle.AddDependency(Resource::ResourceHandle<Resource::IResource>(animation)) ||
                !outBundle.SetRoot(Resource::ResourceHandle<Resource::IResource>(model)))
            {
                outError = {Resource::ResourceLoadErrorCode::LoaderFailure,
                            "Test model loader could not stage the model and animation dependency."};
                return false;
            }
            return true;
        }

        bool SupportsPreparedLoading() const override { return true; }
    };

    template<typename Predicate>
    bool PumpUntil(Resource::ResourceSubsystem& resources, Predicate&& predicate)
    {
        for (uint32 attempt = 0; attempt < 200; ++attempt)
        {
            resources.Tick(0.0f);
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    AnimationSceneAdapters::EcsAnimationSkeletonTopology MakeTopology(bool compatible,
                                                                        bool matchingBindPose = true)
    {
        AnimationSceneAdapters::EcsAnimationSkeletonTopology topology;
        topology.bones.push_back({
            .name = "root",
            .parentIndex = -1,
            .localBindPose = Animation::TransformSample::Identity(),
            .inverseBindPose = Mat4(1.0f),
        });
        topology.bones.push_back({
            .name = "child",
            .parentIndex = compatible ? 0 : -1,
            .localBindPose = Animation::TransformSample::Identity(),
            .inverseBindPose = Mat4(1.0f),
        });
        if (!matchingBindPose)
        {
            topology.bones[1].localBindPose.translation.x = 1.0f;
        }
        return topology;
    }
} // namespace

TEST(EcsAnimationAssetServiceValidation,
     PublishesExactAsyncMetadataAndCancelsOnlyTheOwningSceneRequest)
{
    ResourceSubsystemScope resourceScope;
    Resource::ResourceSubsystem& resources = resourceScope.Get();
    auto animationLoader = std::make_unique<AnimationTestLoader>();
    AnimationTestLoader* const loader = animationLoader.get();
    resources.RegisterLoader(Resource::ResourceType::Animation, std::move(animationLoader));

    AnimationSceneAdapters::EcsAnimationAssetService service(resources);
    const ECS::SceneRuntimeId firstScene(101);
    const ECS::SceneRuntimeId secondScene(102);
    std::string error;

    const auto first = service.Request({.path = "shared.animsvc", .expectedSceneRuntimeId = firstScene},
                                       error);
    ASSERT_TRUE(first.IsValid()) << error;
    const auto second = service.Request({.path = "shared.animsvc", .expectedSceneRuntimeId = secondScene},
                                        error);
    ASSERT_TRUE(second.IsValid()) << error;
    ASSERT_NE(first.handle, second.handle);
    ASSERT_TRUE(service.Cancel(first));

    ASSERT_TRUE(PumpUntil(resources, [&]
    {
        static_cast<void>(service.Update(secondScene));
        const auto status = service.GetStatus(second);
        return status.has_value() && status->state ==
                                      AnimationSceneAdapters::EcsAnimationAssetLoadState::Ready;
    }));

    const auto cancelled = service.GetStatus(first);
    const auto ready = service.GetStatus(second);
    ASSERT_TRUE(cancelled.has_value());
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(cancelled->state, AnimationSceneAdapters::EcsAnimationAssetLoadState::Cancelled);
    EXPECT_EQ(ready->sceneRuntimeId, secondScene);
    EXPECT_EQ(ready->request.state, Resource::ResourceLoadState::Ready);
    EXPECT_EQ(ready->assetKey, ready->request.assetKey);
    EXPECT_TRUE(ready->animationAssetValue != 0);
    EXPECT_EQ(ready->boneCount, 2u);
    ASSERT_EQ(ready->clips.size(), 2u);
    EXPECT_EQ(ready->clips[0].ordinal, 0u);
    EXPECT_EQ(ready->clips[0].name, "Idle");
    EXPECT_EQ(ready->clips[1].ordinal, 1u);
    EXPECT_EQ(ready->clips[1].name, "Walk");
    EXPECT_EQ(ready->clips[1].durationUs, 500'000);
    EXPECT_TRUE(ready->clips[1].hasRootMotion);
    EXPECT_EQ(ready->clips[1].rootMotionBoneName, "root");
    EXPECT_EQ(ready->contentVerification.status,
              Resource::ResourceContentVerificationStatus::Observed);
    EXPECT_EQ(loader->prepareCount, 1u);

    const auto resolver = service.CreateResolver();
    AnimationSceneAdapters::ResourceAnimationEcsEvaluator evaluator(service.CreateResolver());
    EXPECT_FALSE(resolver(0));
    EXPECT_FALSE(resolver(ready->animationAssetValue + 1000));
    EXPECT_TRUE(resolver(ready->animationAssetValue));
    EXPECT_EQ(loader->prepareCount, 1u);

    // Draining the first World scene frees only its generation-safe entry. The Engine-shared
    // service and evaluator remain usable by the second World.
    EXPECT_TRUE(service.PrepareForSceneShutdown(firstScene));
    EXPECT_FALSE(service.GetStatus(first));
    EXPECT_EQ(service.GetSceneDiagnosticsSnapshot(firstScene).requestCount, 0u);
    EXPECT_EQ(service.GetDiagnosticsSnapshot().requestCount, 1u);
    EXPECT_FALSE(service.IsShutdown());
    EXPECT_FALSE(evaluator.IsShutdown());
    EXPECT_TRUE(resolver(ready->animationAssetValue));

    const SceneECS::AnimationSkeletonBinding target{
        .sourceModelAssetValue = 77,
        .sourceSkinIndex = 3,
        .boneCount = 2,
    };
    const auto compatible = service.PrepareCompatibleAnimationBinding(
        second, target, MakeTopology(true), 1);
    ASSERT_TRUE(compatible.IsPrepared()) << compatible.diagnostic;
    EXPECT_EQ(compatible.binding.animationAssetValue, ready->animationAssetValue);
    EXPECT_EQ(compatible.binding.animationClipOrdinal, 1u);
    EXPECT_EQ(compatible.binding.sourceModelAssetValue, 77u);
    EXPECT_EQ(compatible.binding.sourceSkinIndex, 3);

    const auto mismatch = service.PrepareCompatibleAnimationBinding(
        second, target, MakeTopology(false), 1);
    EXPECT_EQ(mismatch.code,
              AnimationSceneAdapters::EcsAnimationBindingPreparationCode::SkeletonTopologyMismatch);

    const auto bindMismatch = service.PrepareCompatibleAnimationBinding(
        second, target, MakeTopology(true, false), 1);
    EXPECT_EQ(bindMismatch.code,
              AnimationSceneAdapters::EcsAnimationBindingPreparationCode::SkeletonBindPoseMismatch);

    const SceneECS::AnimationSkeletonBinding resourceBackedTarget{
        .animationAssetValue = ready->animationAssetValue,
        .sourceModelAssetValue = 88,
        .sourceSkinIndex = 4,
        .boneCount = 2,
    };
    const auto resourceBackedCompatible = service.PrepareCompatibleAnimationBinding(
        second, resourceBackedTarget, 1);
    ASSERT_TRUE(resourceBackedCompatible.IsPrepared()) << resourceBackedCompatible.diagnostic;

    auto mismatchedTargetRequest =
        resources.RequestAsync<Resource::AnimationResource>("target-bind-mismatch.animsvc");
    ASSERT_TRUE(mismatchedTargetRequest.IsValid());
    ASSERT_TRUE(PumpUntil(resources, [&]
    {
        return mismatchedTargetRequest.GetSnapshot().state == Resource::ResourceLoadState::Ready;
    }));
    const Resource::ResourceHandle<Resource::AnimationResource> mismatchedTarget =
        mismatchedTargetRequest.TryGet();
    ASSERT_TRUE(mismatchedTarget);
    const auto resourceBackedMismatch = service.PrepareCompatibleAnimationBinding(
        second,
        SceneECS::AnimationSkeletonBinding{
            .animationAssetValue = mismatchedTarget->GetId(),
            .boneCount = 2,
        },
        1);
    EXPECT_EQ(resourceBackedMismatch.code,
              AnimationSceneAdapters::EcsAnimationBindingPreparationCode::SkeletonBindPoseMismatch);

    const auto staleResourceBackedTarget = service.PrepareCompatibleAnimationBinding(
        second,
        SceneECS::AnimationSkeletonBinding{
            .animationAssetValue = mismatchedTarget->GetId() + 1000,
            .boneCount = 2,
        },
        1);
    EXPECT_EQ(staleResourceBackedTarget.code,
              AnimationSceneAdapters::EcsAnimationBindingPreparationCode::TargetResourceUnavailable);

    EXPECT_TRUE(service.PrepareForSceneShutdown(secondScene));
    EXPECT_EQ(service.GetSceneDiagnosticsSnapshot(secondScene).requestCount, 0u);
    EXPECT_FALSE(service.GetStatus(second));
    evaluator.Shutdown();
    service.Shutdown();
}

TEST(EcsAnimationAssetServiceValidation, ResolvesLoadedModelContainedAnimationAssetIdWithoutStartingIo)
{
    ResourceSubsystemScope resourceScope;
    Resource::ResourceSubsystem& resources = resourceScope.Get();
    resources.RegisterLoader(Resource::ResourceType::Model,
                             std::make_unique<ModelWithAnimationLoader>());
    AnimationSceneAdapters::EcsAnimationAssetService service(resources);

    auto modelRequest = resources.RequestAsync<Resource::ModelResource>("character.modelsvc");
    ASSERT_TRUE(modelRequest.IsValid());
    ASSERT_TRUE(PumpUntil(resources, [&] { return modelRequest.GetSnapshot().state ==
                                                   Resource::ResourceLoadState::Ready; }));
    const Resource::ResourceHandle<Resource::ModelResource> model = modelRequest.TryGet();
    ASSERT_TRUE(model);
    ASSERT_TRUE(model->GetAnimationResource());

    const auto resolver = service.CreateResolver();
    EXPECT_FALSE(resolver(model->GetId()));
    const auto contained = resolver(model->GetAnimationResource()->GetId());
    ASSERT_TRUE(contained);
    EXPECT_EQ(contained->GetId(), model->GetAnimationResource().GetId());
    EXPECT_EQ(contained->GetClips().begin()->first, "Idle");

    modelRequest.Cancel();
    service.Shutdown();
}

TEST(EcsAnimationAssetServiceValidation,
     EngineDestroysSharedAnimationOwnersBeforeResourceSubsystemShutdown)
{
    Engine engine;
    EngineConfig config;
    config.enableJobSystem = false;
    engine.SetConfig(config);
    ASSERT_NE(engine.AddSubsystem<Resource::ResourceSubsystem>(), nullptr);
    ASSERT_TRUE(engine.Initialize());

    engine.Shutdown();
    const EngineShutdownDiagnostics& diagnostics = engine.GetLastShutdownDiagnostics();
    EXPECT_TRUE(diagnostics.clean);
    EXPECT_TRUE(diagnostics.resourceSubsystemObserved);
    EXPECT_TRUE(diagnostics.resourceSubsystemClean);
}

TEST(EcsAnimationAssetServiceValidation,
     RejectsPendingDestroyTargetBeforeReadingOrReplacingItsBinding)
{
    ResourceSubsystemScope resources;
    auto service = std::make_shared<AnimationSceneAdapters::EcsAnimationAssetService>(resources.Get());
    auto evaluator = std::make_shared<AnimationSceneAdapters::ResourceAnimationEcsEvaluator>(
        service->CreateResolver());
    SceneECS::SceneEcsRuntime runtime;
    RejectingEcsRenderTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    WorldEcsRuntimeCompositionOptions options;
    options.animationAssetService = service;
    options.resourceAnimationEvaluator = evaluator;
    WorldEcsRuntimeComposition composition(runtime, pipeline, nullptr, std::move(options));
    ASSERT_TRUE(composition.Initialize());
    WorldEcsRuntimeCompositionTickRequest request;
    request.sceneTick.variableDeltaSeconds = 1.0 / 60.0;
    ASSERT_TRUE(composition.Tick(request).succeeded);

    const ECS::EntityHandle target = runtime.CreateEntity();
    ASSERT_TRUE(target.IsValid());
    ASSERT_TRUE(runtime.AddFragment<SceneECS::AnimationSkeletonBinding>(
        target, {.animationAssetValue = 0, .boneCount = 1}));
    const SceneECS::SceneEntityRef targetRef = runtime.GetEntityRef(target);
    ASSERT_TRUE(targetRef.IsValid());
    ASSERT_EQ(runtime.RequestDestroy(
                  target,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);
    // PendingDestroy entities remain Registry-alive; this asserts that the World service
    // additionally requires the Scene lifecycle phase to be exactly Alive.
    ASSERT_EQ(runtime.GetEntityRef(target), targetRef);

    const AnimationSceneAdapters::EcsAnimationBindingPreparationResult result =
        composition.PrepareCompatibleAnimationBinding(
            {.sceneRuntimeId = runtime.GetSceneRuntimeId(),
             .handle = AnimationSceneAdapters::EcsAnimationAssetLoadHandle::Create(0)},
            targetRef);
    EXPECT_EQ(result.code,
              AnimationSceneAdapters::EcsAnimationBindingPreparationCode::InvalidTargetEntity);

    // Advance the normal owner tick so the composition's cleanup processor observes the
    // fixture destroy request before the regular shutdown drain begins.
    ASSERT_TRUE(composition.Tick(request).succeeded);
    bool shutdownComplete = false;
    for (uint32 attempt = 0; attempt < 8 && !shutdownComplete; ++attempt)
    {
        shutdownComplete = composition.PrepareForShutdown(request);
    }
    EXPECT_TRUE(shutdownComplete) << composition.GetDiagnostics().lastDiagnostic;
    evaluator->Shutdown();
    service->Shutdown();
}
