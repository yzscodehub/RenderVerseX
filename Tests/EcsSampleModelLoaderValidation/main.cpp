#include "Core/Log.h"
#include "Samples/SampleModelLoader.h"

#include <gtest/gtest.h>

#include <limits>
#include <map>
#include <utility>
#include <vector>

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

    Resource::ResourceContentIdentity MakeIdentity()
    {
        Resource::ResourceContentIdentity identity;
        identity.schemaVersion = Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = Resource::ResourceContentIdentityDomain::CookedArtifact;
        identity.scope = Resource::ResourceContentIdentityScope::DependencyClosure;
        identity.algorithm = Resource::ResourceContentHashAlgorithm::SHA256;
        identity.digest = "c20da18f79ec48a49e720d6fd6f23c1cf2f0a3b7f2ecaaed53477732d0500f4c";
        identity.byteCount = 321;
        identity.fileCount = 4;
        return identity;
    }

    class RecordingWorldServices final : public IWorldEcsRuntimeServices
    {
    public:
        [[nodiscard]] ResourceSceneAdapters::EcsModelAssetLoadRef RequestModel(
            ResourceSceneAdapters::EcsModelAssetLoadDesc desc,
            std::string& outError) override
        {
            outError.clear();
            m_requests.push_back(desc);
            const auto handle = m_handles.Allocate();
            ResourceSceneAdapters::EcsSceneAssetLoadStatus status;
            status.request.assetKey = Resource::MakeAssetKey(
                desc.path,
                Resource::ResourceType::Model,
                desc.resourceOptions.importOptionsHash,
                desc.resourceOptions.platformProfileHash,
                desc.resourceOptions.loaderSchemaVersion,
                desc.resourceOptions.expectedContentIdentity);
            status.request.options = desc.resourceOptions;
            status.assetKey = status.request.assetKey;
            status.sceneRuntimeId = desc.expectedSceneRuntimeId;
            m_statuses.emplace(handle, std::move(status));
            return {.sceneRuntimeId = desc.expectedSceneRuntimeId, .handle = handle};
        }

        [[nodiscard]] bool CancelModel(
            ResourceSceneAdapters::EcsModelAssetLoadRef request) override
        {
            const auto status = m_statuses.find(request.handle);
            if (!request.IsValid() || status == m_statuses.end() ||
                status->second.sceneRuntimeId != request.sceneRuntimeId || !m_cancelResult)
            {
                return false;
            }
            status->second.state = ResourceSceneAdapters::EcsSceneAssetLoadState::Retiring;
            return true;
        }

        [[nodiscard]] std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus>
        GetModelStatus(ResourceSceneAdapters::EcsModelAssetLoadRef request) const override
        {
            const auto status = m_statuses.find(request.handle);
            return request.IsValid() && status != m_statuses.end() &&
                           status->second.sceneRuntimeId == request.sceneRuntimeId
                       ? std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus>(
                             status->second)
                       : std::nullopt;
        }

        [[nodiscard]] ResourceSceneAdapters::EcsEnvironmentLoadRef RequestEnvironment(
            ResourceSceneAdapters::EcsEnvironmentLoadDesc,
            std::string& outError) override
        {
            outError = "Environment is outside this model-loader test service.";
            return {};
        }

        [[nodiscard]] bool CancelEnvironment(
            ResourceSceneAdapters::EcsEnvironmentLoadRef) override
        {
            return false;
        }

        [[nodiscard]] std::optional<ResourceSceneAdapters::EcsEnvironmentLoadStatus>
        GetEnvironmentStatus(ResourceSceneAdapters::EcsEnvironmentLoadRef) const override
        {
            return std::nullopt;
        }

        [[nodiscard]] AnimationSceneAdapters::EcsAnimationAssetLoadRef RequestAnimation(
            AnimationSceneAdapters::EcsAnimationAssetLoadDesc,
            std::string& outError) override
        {
            outError = "Animation is outside this model-loader test service.";
            return {};
        }

        [[nodiscard]] bool CancelAnimation(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef) override
        {
            return false;
        }

        [[nodiscard]] std::optional<AnimationSceneAdapters::EcsAnimationAssetLoadStatus>
        GetAnimationStatus(AnimationSceneAdapters::EcsAnimationAssetLoadRef) const override
        {
            return std::nullopt;
        }

        [[nodiscard]] AnimationSceneAdapters::EcsAnimationBindingPreparationResult
        PrepareCompatibleAnimationBinding(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef,
            SceneECS::SceneEntityRef,
            uint32) override
        {
            return {};
        }

        [[nodiscard]] WorldEcsAnimationPhysicsBindingResult
        BindAnimationRootMotionPhysics(SceneECS::SceneEntityRef) override
        {
            return {};
        }

        [[nodiscard]] WorldEcsRuntimeServicesDiagnostics GetRuntimeDiagnostics() const override
        {
            return {};
        }

        void SetStatus(ResourceSceneAdapters::EcsModelAssetLoadRef request,
                       ResourceSceneAdapters::EcsSceneAssetLoadStatus status)
        {
            ASSERT_TRUE(request.IsValid());
            m_statuses.insert_or_assign(request.handle, std::move(status));
        }

        void Retire(ResourceSceneAdapters::EcsModelAssetLoadRef request)
        {
            m_statuses.erase(request.handle);
            ASSERT_TRUE(m_handles.TryFree(request.handle));
        }

        void SetCancelResult(bool value) noexcept { m_cancelResult = value; }

        [[nodiscard]] const std::vector<ResourceSceneAdapters::EcsModelAssetLoadDesc>&
        GetRequests() const
        {
            return m_requests;
        }

    private:
        HandlePool<ResourceSceneAdapters::EcsSceneAssetLoadHandle> m_handles;
        std::map<ResourceSceneAdapters::EcsSceneAssetLoadHandle,
                 ResourceSceneAdapters::EcsSceneAssetLoadStatus> m_statuses;
        std::vector<ResourceSceneAdapters::EcsModelAssetLoadDesc> m_requests;
        bool m_cancelResult = true;
    };

    SceneECS::LocalTransform MakeTranslation(float32 x)
    {
        SceneECS::LocalTransform transform;
        transform.translation = {x, 0.0f, 0.0f};
        return transform;
    }

    LoadedSampleModel MakeBoundModel(const SceneECS::SceneEcsRuntime& scene,
                                     ECS::EntityHandle root,
                                     ECS::EntityHandle member)
    {
        LoadedSampleModel model;
        model.request = {.sceneRuntimeId = scene.GetSceneRuntimeId(),
                         .handle = ResourceSceneAdapters::EcsSceneAssetLoadHandle::Create(1, 1)};
        model.status.sceneRuntimeId = model.request.sceneRuntimeId;
        model.status.rootEntity = root;
        model.status.members = {root, member};
        model.status.modelMetadata.sourceModelAssetId = {.value = 7001};
        model.status.state = ResourceSceneAdapters::EcsSceneAssetLoadState::CPUReadyHidden;
        return model;
    }
} // namespace

TEST(EcsSampleModelLoaderValidation,
     CatalogIdentityStatusAdditionalInstanceAndCancellationStayValueOnly)
{
    SceneECS::SceneEcsRuntime scene;
    RecordingWorldServices services;
    const Resource::ResourceContentIdentity identity = MakeIdentity();
    SampleAssetRegistry registry({
        {.id = "model",
         .kind = SampleAssetKind::Model,
         .resolvedPath = "catalog-model.ecsmodel",
         .contentIdentity = identity},
        {.id = "environment",
         .kind = SampleAssetKind::Environment,
         .resolvedPath = "wrong-kind.hdr"},
    });
    SampleModelLoader models(services, scene, registry);

    LoadedSampleModel rejected;
    std::string error;
    EXPECT_FALSE(models.RequestByAssetId("missing", rejected, error));
    EXPECT_FALSE(models.RequestByAssetId("environment", rejected, error));
    EXPECT_TRUE(services.GetRequests().empty());

    LoadedSampleModel source;
    ASSERT_TRUE(models.RequestByAssetId("model", source, error)) << error;
    ASSERT_EQ(services.GetRequests().size(), 1u);
    const auto& firstRequest = services.GetRequests().front();
    EXPECT_EQ(firstRequest.expectedSceneRuntimeId, scene.GetSceneRuntimeId());
    EXPECT_EQ(firstRequest.resourceOptions.expectedContentIdentity, identity);

    auto sourceStatus = *services.GetModelStatus(source.request);
    sourceStatus.state = ResourceSceneAdapters::EcsSceneAssetLoadState::CPUReadyHidden;
    sourceStatus.modelMetadata = {
        .sourceModelAssetId = {.value = 101},
        .sourceNodeCount = 7,
        .meshAssetIds = {{.value = 201}, {.value = 202}},
        .materialAssetIds = {{.value = 301}},
        .streamingTextureAssetIds = {{.value = 401}, {.value = 402}},
        .animationAssetId = {.value = 501},
        .contentVerificationReceipt = {
            .status = Resource::ResourceContentVerificationStatus::Verified,
            .expected = identity,
            .observed = identity,
        },
    };
    services.SetStatus(source.request, sourceStatus);
    const auto refreshed = models.UpdateReadiness(source);
    EXPECT_TRUE(source.IsCPUReady());
    EXPECT_EQ(refreshed.modelMetadata.sourceModelAssetId.value, 101u);
    EXPECT_EQ(refreshed.modelMetadata.sourceNodeCount, 7u);
    EXPECT_EQ(refreshed.modelMetadata.meshAssetIds.size(), 2u);
    EXPECT_EQ(refreshed.modelMetadata.materialAssetIds.front().value, 301u);
    EXPECT_EQ(refreshed.modelMetadata.streamingTextureAssetIds.back().value, 402u);
    EXPECT_EQ(refreshed.modelMetadata.animationAssetId.value, 501u);
    const auto receipt = models.GetContentVerificationReceipt(source.sourcePath);
    ASSERT_TRUE(receipt.has_value());
    EXPECT_TRUE(receipt->IsVerified());

    LoadedSampleModel additional;
    ASSERT_TRUE(models.RequestAdditionalInstance(source, additional, error)) << error;
    ASSERT_EQ(services.GetRequests().size(), 2u);
    EXPECT_EQ(services.GetRequests()[1].path, sourceStatus.request.assetKey.canonicalPath);
    EXPECT_EQ(services.GetRequests()[1].resourceOptions.expectedContentIdentity, identity);

    SampleModelCancellation cancellation;
    ASSERT_TRUE(models.CancelForRetirement(additional, cancellation));
    EXPECT_TRUE(cancellation.IsValid());
    EXPECT_TRUE(models.QueryRetirementStatus(cancellation).has_value());
    services.Retire(cancellation.request);
    EXPECT_TRUE(models.IsRetirementComplete(cancellation));

    LoadedSampleModel retry;
    ASSERT_TRUE(models.RequestAdditionalInstance(source, retry, error)) << error;
    EXPECT_EQ(services.GetRequests().size(), 3u);
}

TEST(EcsSampleModelLoaderValidation,
     ImmediateLocalBoundsValidateMemberIdentityAndDoNotReadVisibilityOrResolvedTransforms)
{
    SceneECS::SceneEcsRuntime scene;
    SceneECS::RuntimeEntityDesc rootDesc;
    rootDesc.localTransform = MakeTranslation(10.0f);
    const ECS::EntityHandle root = scene.CreateEntity(rootDesc);
    SceneECS::RuntimeEntityDesc childDesc;
    childDesc.localTransform = MakeTranslation(1.0f);
    childDesc.bounds = {.center = Vec3(0.0f), .extents = Vec3(1.0f)};
    const ECS::EntityHandle child = scene.CreateEntity(childDesc);
    ASSERT_TRUE(root.IsValid());
    ASSERT_TRUE(child.IsValid());
    ASSERT_EQ(scene.Reparent(child, root), SceneECS::ReparentResult::Applied);
    ASSERT_TRUE(scene.AddFragment<SceneECS::Mesh>(
        child, {.meshAssetId = {.value = 41}, .submeshCount = 1}));
    ASSERT_TRUE(scene.AddFragment<SceneECS::Visibility>(child, {.visible = false}));

    LoadedSampleModel model = MakeBoundModel(scene, root, child);
    AABB bounds;
    ASSERT_TRUE(TryComputeSampleModelRenderableWorldBounds(model, scene, bounds));
    EXPECT_EQ(bounds.GetMin(), Vec3(10.0f, -1.0f, -1.0f));
    EXPECT_EQ(bounds.GetMax(), Vec3(12.0f, 1.0f, 1.0f));

    // No Scene tick: the calculation must still observe the ancestor's current local write.
    ASSERT_TRUE(scene.SetLocalTransform(root, MakeTranslation(20.0f)));
    ASSERT_TRUE(TryComputeSampleModelRenderableWorldBounds(model, scene, bounds));
    EXPECT_EQ(bounds.GetMin(), Vec3(20.0f, -1.0f, -1.0f));
    EXPECT_EQ(bounds.GetMax(), Vec3(22.0f, 1.0f, 1.0f));

    LoadedSampleModel stale = model;
    stale.status.members[1] = ECS::EntityHandle::Invalid();
    EXPECT_FALSE(TryComputeSampleModelRenderableWorldBounds(stale, scene, bounds));

    SceneECS::SceneEcsRuntime foreignScene;
    LoadedSampleModel foreign = model;
    foreign.request.sceneRuntimeId = foreignScene.GetSceneRuntimeId();
    foreign.status.sceneRuntimeId = foreign.request.sceneRuntimeId;
    EXPECT_FALSE(TryComputeSampleModelRenderableWorldBounds(foreign, scene, bounds));

    // Corrupt only this test fixture through the Registry to prove defensive
    // detection of a ParentRelation cycle that public Scene APIs reject.
    ECS::Registry& registry = const_cast<ECS::Registry&>(scene.GetRegistry());
    ASSERT_TRUE(registry.Add<SceneECS::ParentRelation>(root, {.parent = child}));
    EXPECT_FALSE(TryComputeSampleModelRenderableWorldBounds(model, scene, bounds));

    SceneECS::SceneEcsRuntime missingFragmentScene;
    SceneECS::RuntimeEntityDesc missingDesc;
    missingDesc.bounds = {.center = Vec3(0.0f), .extents = Vec3(1.0f)};
    const ECS::EntityHandle missing = missingFragmentScene.CreateEntity(missingDesc);
    ASSERT_TRUE(missingFragmentScene.AddFragment<SceneECS::Mesh>(
        missing, {.meshAssetId = {.value = 43}, .submeshCount = 1}));
    LoadedSampleModel missingModel =
        MakeBoundModel(missingFragmentScene, missing, missing);
    ECS::Registry& missingRegistry =
        const_cast<ECS::Registry&>(missingFragmentScene.GetRegistry());
    ASSERT_TRUE(missingRegistry.Remove<SceneECS::Bounds>(missing));
    EXPECT_FALSE(TryComputeSampleModelRenderableWorldBounds(
        missingModel, missingFragmentScene, bounds));

    SceneECS::SceneEcsRuntime nonfiniteScene;
    SceneECS::RuntimeEntityDesc nonfiniteDesc;
    nonfiniteDesc.localTransform = MakeTranslation(0.0f);
    nonfiniteDesc.bounds = {.center = Vec3(0.0f), .extents = Vec3(1.0f)};
    const ECS::EntityHandle nonfinite = nonfiniteScene.CreateEntity(nonfiniteDesc);
    ASSERT_TRUE(nonfiniteScene.AddFragment<SceneECS::Mesh>(
        nonfinite, {.meshAssetId = {.value = 42}, .submeshCount = 1}));
    LoadedSampleModel nonfiniteModel =
        MakeBoundModel(nonfiniteScene, nonfinite, nonfinite);
    SceneECS::LocalTransform invalid = MakeTranslation(0.0f);
    invalid.translation.x = std::numeric_limits<float32>::quiet_NaN();
    ASSERT_TRUE(nonfiniteScene.SetLocalTransform(nonfinite, invalid));
    EXPECT_FALSE(TryComputeSampleModelRenderableWorldBounds(
        nonfiniteModel, nonfiniteScene, bounds));
}

TEST(EcsSampleModelLoaderValidation,
     RequestRefsRejectLiveOutputReplacementForeignStatusAndFailedCancellation)
{
    SceneECS::SceneEcsRuntime sceneA;
    SceneECS::SceneEcsRuntime sceneB;
    RecordingWorldServices servicesA;
    RecordingWorldServices servicesB;
    const Resource::ResourceContentIdentity identity = MakeIdentity();
    SampleAssetRegistry registry({
        {.id = "model",
         .kind = SampleAssetKind::Model,
         .resolvedPath = "identity-model.ecsmodel",
         .contentIdentity = identity},
    });
    SampleModelLoader modelsA(servicesA, sceneA, registry);
    SampleModelLoader modelsB(servicesB, sceneB, registry);

    LoadedSampleModel modelA;
    LoadedSampleModel modelB;
    std::string error;
    ASSERT_TRUE(modelsA.RequestByAssetId("model", modelA, error)) << error;
    ASSERT_TRUE(modelsB.RequestByAssetId("model", modelB, error)) << error;
    ASSERT_EQ(modelA.request.handle, modelB.request.handle);
    EXPECT_NE(modelA.request.sceneRuntimeId, modelB.request.sceneRuntimeId);

    const auto originalRequest = modelA.request;
    EXPECT_FALSE(modelsA.RequestByAssetId("model", modelA, error));
    EXPECT_EQ(modelA.request, originalRequest);
    EXPECT_EQ(servicesA.GetRequests().size(), 1u);

    LoadedSampleModel foreign = modelA;
    foreign.request = modelB.request;
    const auto foreignStatus = modelsA.UpdateReadiness(foreign);
    EXPECT_EQ(foreignStatus.state, ResourceSceneAdapters::EcsSceneAssetLoadState::Failed);
    EXPECT_EQ(foreign.request, modelB.request);

    servicesA.SetCancelResult(false);
    EXPECT_FALSE(modelsA.Cancel(modelA));
    EXPECT_EQ(modelA.request, originalRequest);
    servicesA.SetCancelResult(true);
    EXPECT_TRUE(modelsA.Cancel(modelA));
    EXPECT_FALSE(modelA.request.IsValid());
}
