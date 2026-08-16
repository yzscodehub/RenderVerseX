#include "Core/Log.h"
#include "Resource/Loader/EnvironmentLoader.h"
#include "Resource/PreparedResourceBundle.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/EnvironmentResource.h"
#include "Resource/Types/TextureResource.h"
#include "ResourceSceneAdapters/ECS/EcsEnvironmentLoadCoordinator.h"
#include "Scene/ECS/RetirementFragments.h"

#include <gtest/gtest.h>

#include <unordered_map>

namespace
{
    RVX::Resource::ResourceContentIdentity MakeObservedIdentity()
    {
        RVX::Resource::ResourceContentIdentity identity;
        identity.schemaVersion = RVX::Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
        identity.domain = RVX::Resource::ResourceContentIdentityDomain::Source;
        identity.scope = RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact;
        identity.algorithm = RVX::Resource::ResourceContentHashAlgorithm::SHA256;
        identity.digest = "4d66ebacb7e4a7a7ce7f8125169a47e8807ca3f3ba994571d004f7e9ee8cc5ef";
        identity.byteCount = 64;
        identity.fileCount = 1;
        return identity;
    }

    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { RVX::Log::Initialize(); }
        void TearDown() override { RVX::Log::Shutdown(); }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class ImmediateEnvironmentLoader final : public RVX::Resource::IResourceLoader
    {
    public:
        RVX::ResourceType GetResourceType() const override
        {
            return RVX::ResourceType::Environment;
        }

        std::vector<std::string> GetSupportedExtensions() const override { return {".hdr"}; }
        RVX::Resource::IResource* Load(const std::string&) override { return nullptr; }
        bool SupportsPreparedLoading() const override { return true; }

        bool ValidatePreparationState(
            RVX::uint64 requestedImportOptionsHash,
            RVX::Resource::ResourceLoadPreparationStateRef suppliedState,
            RVX::Resource::ResourceLoadPreparationStateRef& outState,
            RVX::uint64& outCanonicalImportOptionsHash,
            RVX::Resource::ResourceLoadError& outError) const override
        {
            const auto preparation = std::dynamic_pointer_cast<
                const RVX::Resource::EnvironmentPreparationState>(std::move(suppliedState));
            if (!preparation || !preparation->IsValid() ||
                requestedImportOptionsHash != preparation->importOptionsHash)
            {
                outState.reset();
                outCanonicalImportOptionsHash = 0;
                outError = {RVX::Resource::ResourceLoadErrorCode::InvalidRequest,
                            "Fixture rejected an inconsistent Environment preparation state."};
                return false;
            }
            outState = preparation;
            outCanonicalImportOptionsHash = preparation->importOptionsHash;
            outError = {};
            return true;
        }

        bool Prepare(const RVX::Resource::ResourceLoadPreparationContext& context,
                     RVX::Resource::PreparedResourceBundle& bundle,
                     RVX::Resource::ResourceLoadError& error) override
        {
            const auto preparation = std::dynamic_pointer_cast<
                const RVX::Resource::EnvironmentPreparationState>(context.loaderState);
            if (!preparation || !preparation->IsValid())
            {
                error = {RVX::Resource::ResourceLoadErrorCode::InvalidRequest,
                         "Fixture requires immutable Environment preparation state."};
                return false;
            }
            const auto makeTexture = [&context](const char* suffix, bool cubemap)
            {
                auto* texture = new RVX::Resource::TextureResource();
                texture->SetId(RVX::GenerateResourceId(context.resourceIdentityPath + suffix));
                RVX::Resource::TextureMetadata metadata;
                metadata.width = 1;
                metadata.height = 1;
                metadata.arrayLayers = cubemap ? 6 : 1;
                metadata.isCubemap = cubemap;
                texture->SetData(std::vector<RVX::uint8>(cubemap ? 24 : 4, 255), metadata);
                return RVX::Resource::TextureHandle(texture);
            };

            RVX::Resource::EnvironmentResourceData data;
            data.sourcePath = context.resolvedPath;
            data.environment = makeTexture("#environment", true);
            data.irradiance = makeTexture("#irradiance", true);
            data.prefiltered = makeTexture("#prefiltered", true);
            data.brdfLUT = makeTexture("#brdf", false);
            data.environmentResolution = preparation->hdrOptions.cubemapResolution;
            data.irradianceResolution = preparation->hdrOptions.irradianceResolution;
            data.prefilteredResolution = preparation->hdrOptions.prefilteredResolution;
            data.prefilteredMipLevels = preparation->hdrOptions.prefilteredMipLevels;
            data.brdfLUTResolution = preparation->hdrOptions.brdfLUTResolution;
            data.intensity = preparation->hdrOptions.exposure;

            auto root = RVX::Resource::EnvironmentHandle(new RVX::Resource::EnvironmentResource());
            root->SetId(context.rootResourceId);
            root->SetPath(context.requestedPath);
            root->SetName("EcsEnvironmentCoordinatorFixture");
            if (!root->SetPreparedData(std::move(data)))
            {
                error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                         "Could not prepare Environment fixture."};
                return false;
            }
            const RVX::Resource::EnvironmentResourceData& prepared = root->GetData();
            return bundle.AddDependency(
                       RVX::Resource::ResourceHandle<RVX::Resource::IResource>(prepared.environment)) &&
                   bundle.AddDependency(
                       RVX::Resource::ResourceHandle<RVX::Resource::IResource>(prepared.irradiance)) &&
                   bundle.AddDependency(
                       RVX::Resource::ResourceHandle<RVX::Resource::IResource>(prepared.prefiltered)) &&
                   bundle.AddDependency(
                       RVX::Resource::ResourceHandle<RVX::Resource::IResource>(prepared.brdfLUT)) &&
                   bundle.SetRoot(RVX::Resource::ResourceHandle<RVX::Resource::IResource>(root)) &&
                   bundle.SetObservedContentIdentity(MakeObservedIdentity());
        }
    };

    class ResourceGateway final : public RVX::IRenderResourceGateway
    {
    public:
        RVX::RenderResourceReserveResult ReserveResource(
            RVX::AssetId assetId, RVX::RenderResourceKind kind) noexcept override
        {
            const auto existing = m_assets.find(assetId);
            if (existing != m_assets.end())
            {
                return {RVX::RenderResourceReserveCode::Existing,
                        existing->second,
                        QueryResourceStatus(existing->second)};
            }
            const RVX::RenderResourceHandle handle{m_nextSlot++, 1};
            const RVX::RenderResourceStatus status{
                RVX::RenderResourceStatusCode::Current,
                RVX::RenderResourcePublicState::Reserved,
                RVX::RenderResourceFailureCode::None};
            m_assets.emplace(assetId, handle);
            m_statuses.emplace(handle, status);
            (void)kind;
            return {RVX::RenderResourceReserveCode::Reserved, handle, status};
        }

        RVX::RenderUploadEnqueueResult TryEnqueueUpload(
            const RVX::ResourceUploadRequestRef& request) noexcept override
        {
            if (!request)
            {
                return {RVX::RenderUploadEnqueueCode::InvalidRequest};
            }
            const auto found = m_statuses.find(request->GetHandle());
            if (found == m_statuses.end())
            {
                return {RVX::RenderUploadEnqueueCode::StaleGeneration};
            }
            if (m_uploadsReady)
            {
                found->second.state = RVX::RenderResourcePublicState::GPUReady;
            }
            found->second.committedContentRevision = request->GetSourceRevision();
            return {RVX::RenderUploadEnqueueCode::Accepted};
        }

        RVX::RenderReleaseResult RequestRelease(RVX::RenderResourceHandle handle) noexcept override
        {
            const auto found = m_statuses.find(handle);
            if (found == m_statuses.end())
            {
                return {RVX::RenderReleaseCode::StaleGeneration};
            }
            found->second.state = RVX::RenderResourcePublicState::Released;
            return {RVX::RenderReleaseCode::Accepted};
        }

        [[nodiscard]] RVX::RenderResourceStatus QueryResourceStatus(
            RVX::RenderResourceHandle handle) const noexcept override
        {
            const auto found = m_statuses.find(handle);
            return found != m_statuses.end() ?
                       found->second :
                       RVX::RenderResourceStatus{
                           RVX::RenderResourceStatusCode::StaleGeneration,
                           RVX::RenderResourcePublicState::Released,
                           RVX::RenderResourceFailureCode::None};
        }

        void MakeUploadsReady() noexcept
        {
            m_uploadsReady = true;
            for (auto& [handle, status] : m_statuses)
            {
                (void)handle;
                if (status.state == RVX::RenderResourcePublicState::Reserved)
                {
                    status.state = RVX::RenderResourcePublicState::GPUReady;
                }
            }
        }

    private:
        RVX::uint32 m_nextSlot = 1;
        bool m_uploadsReady = false;
        std::unordered_map<RVX::AssetId, RVX::RenderResourceHandle, RVX::AssetIdHash> m_assets;
        std::unordered_map<RVX::RenderResourceHandle,
                           RVX::RenderResourceStatus,
                           RVX::RenderResourceHandleHash>
            m_statuses;
    };

    class ResourceSubsystemScope final
    {
    public:
        ResourceSubsystemScope()
        {
            m_resources.SetRenderResourceGateway(&m_gateway);
            RVX::Resource::ResourceManagerConfig config;
            config.asyncThreadCount = 0;
            m_resources.Initialize(config);
            m_resources.RegisterLoader(
                RVX::ResourceType::Environment,
                std::make_unique<ImmediateEnvironmentLoader>());
        }

        ~ResourceSubsystemScope() { m_resources.Deinitialize(); }

        void PublishEnvironment() { m_resources.Tick(0.0f); }

        ResourceGateway m_gateway;
        RVX::Resource::ResourceSubsystem m_resources;
    };

    class PresentedRetirementProof final
        : public RVX::ResourceSceneAdapters::IEcsSceneAssetRetirementProofGateway
    {
    public:
        explicit PresentedRetirementProof(bool presented = true)
            : m_presented(presented)
        {
        }

        void SetPresented(bool presented) noexcept { m_presented = presented; }
        void SetAdmissionAccepted(bool accepted) noexcept { m_admissionAccepted = accepted; }

        [[nodiscard]] RVX::ResourceSceneAdapters::EcsSceneAssetRetirementBeginReceipt
        BeginRetirement(
            const RVX::ResourceSceneAdapters::EcsSceneAssetRetirementRequest& request) override
        {
            ++beginCalls;
            if (!m_admissionAccepted)
            {
                return {.code = RVX::ResourceSceneAdapters::EcsSceneAssetRetirementBeginCode::FailedRetained,
                        .request = request,
                        .diagnostic = "Fixture rejected admission."};
            }
            m_request = request;
            return {.code = RVX::ResourceSceneAdapters::EcsSceneAssetRetirementBeginCode::Accepted,
                    .token = {.value = 1, .generation = 1},
                    .request = request};
        }

        [[nodiscard]] RVX::ResourceSceneAdapters::EcsSceneAssetRetirementProof
        QueryRetirementProof(
            RVX::ResourceSceneAdapters::EcsSceneAssetRetirementToken token) const override
        {
            return {.state = m_presented ?
                             RVX::ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Presented :
                             RVX::ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Pending,
                    .request = token.IsValid() ? m_request :
                        RVX::ResourceSceneAdapters::EcsSceneAssetRetirementRequest{},
                    .appliedRenderSceneRevision = m_presented ? 7u : 0u,
                    .presentedFrameSequence = m_presented ? 11u : 0u};
        }

        [[nodiscard]] bool AcknowledgeRetirementProof(
            RVX::ResourceSceneAdapters::EcsSceneAssetRetirementToken token) override
        {
            ++acknowledgeCalls;
            return token.IsValid();
        }

        RVX::uint32 beginCalls = 0;
        RVX::uint32 acknowledgeCalls = 0;

    private:
        bool m_presented = true;
        bool m_admissionAccepted = true;
        RVX::ResourceSceneAdapters::EcsSceneAssetRetirementRequest m_request;
    };

    [[nodiscard]] RVX::ResourceSceneAdapters::EcsEnvironmentPresentationReceipt
    MakeReceipt(const RVX::ResourceSceneAdapters::EcsEnvironmentLoadStatus& status)
    {
        return {
            .sceneRuntimeId = status.sceneRuntimeId,
            .skyboxEntity = status.skyboxEntity,
            .skyboxWriteVersion = status.skyboxWriteVersion,
            .frozenSourceSnapshotRevision = 5,
            .renderSceneRevision = 7,
            .carryingPresentedFrameSequence = 11,
        };
    }

    void AdvanceToPresentation(
        RVX::ResourceSceneAdapters::EcsEnvironmentLoadCoordinator& coordinator,
        ResourceSubsystemScope& resources,
        RVX::ResourceSceneAdapters::EcsEnvironmentLoadHandle handle)
    {
        resources.PublishEnvironment();
        ASSERT_TRUE(coordinator.Update()); // Ready -> GPU readiness wait.
        ASSERT_TRUE(coordinator.Update()); // Four reserved GPU uploads block adoption.
        ASSERT_EQ(coordinator.GetStatus(handle)->state,
                  RVX::ResourceSceneAdapters::EcsEnvironmentLoadState::PendingGpuReadiness);
        resources.m_gateway.MakeUploadsReady();
        ASSERT_TRUE(coordinator.Update()); // GPU-ready exact IBL set -> PendingAdopt.
        ASSERT_EQ(coordinator.GetStatus(handle)->state,
                  RVX::ResourceSceneAdapters::EcsEnvironmentLoadState::PendingAdopt);
        ASSERT_TRUE(coordinator.Update()); // Atomic dedicated Skybox adoption.
        ASSERT_EQ(coordinator.GetStatus(handle)->state,
                  RVX::ResourceSceneAdapters::EcsEnvironmentLoadState::PendingPresentation);
    }

    RVX::ResourceSceneAdapters::EcsEnvironmentLoadDesc MakeEnvironmentDesc(
        std::string path,
        RVX::ECS::SceneRuntimeId sceneRuntimeId)
    {
        RVX::ResourceSceneAdapters::EcsEnvironmentLoadDesc desc;
        desc.path = std::move(path);
        desc.expectedSceneRuntimeId = sceneRuntimeId;
        return desc;
    }
} // namespace

TEST(EcsEnvironmentLoadCoordinatorValidation, EnvironmentAdoptionIsAtomicAndSpecialized)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    RVX::ResourceSceneAdapters::EcsEnvironmentAdoptionBatch batch;
    batch.sourceEnvironmentAssetId = {.value = 100};
    batch.skybox.mode = RVX::SceneECS::SkyboxMode::Cubemap;
    batch.skybox.environmentAssetId = {.value = 101};
    batch.skybox.irradianceAssetId = {.value = 102};
    batch.skybox.prefilteredEnvironmentAssetId = {.value = 103};
    batch.skybox.brdfLutAssetId = {.value = 104};

    const auto rejected = RVX::ResourceSceneAdapters::AdoptEcsEnvironmentBatch(
        runtime,
        batch,
        runtime.GetSceneRuntimeId(),
        {.fault = RVX::ResourceSceneAdapters::EcsEnvironmentAdoptionFault::RejectAfterRecording});
    EXPECT_FALSE(rejected.IsApplied());
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 0u);

    const auto receipt = RVX::ResourceSceneAdapters::AdoptEcsEnvironmentBatch(
        runtime, batch, runtime.GetSceneRuntimeId());
    ASSERT_TRUE(receipt.IsApplied());
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 1u);
    EXPECT_NE(runtime.GetRegistry().TryGet<RVX::SceneECS::LocalTransform>(receipt.skyboxEntity),
              nullptr);
    EXPECT_NE(runtime.GetRegistry().TryGet<RVX::SceneECS::EntityLifecycleState>(
                  receipt.skyboxEntity),
              nullptr);
    EXPECT_NE(runtime.GetRegistry().TryGet<RVX::SceneECS::Skybox>(receipt.skyboxEntity), nullptr);
    EXPECT_NE(runtime.GetRegistry().TryGet<RVX::ResourceSceneAdapters::EnvironmentInstanceFragment>(
                  receipt.skyboxEntity),
              nullptr);
    EXPECT_TRUE(runtime.GetRegistry().HasTag<RVX::SceneECS::SpecializedRenderRetirement>(
        receipt.skyboxEntity));

    const auto duplicate = RVX::ResourceSceneAdapters::AdoptEcsEnvironmentBatch(
        runtime, batch, runtime.GetSceneRuntimeId());
    EXPECT_FALSE(duplicate.IsApplied());
    EXPECT_EQ(duplicate.error,
              RVX::ResourceSceneAdapters::EcsEnvironmentAdoptionError::ExistingLiveSkybox);
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 1u);
}

TEST(EcsEnvironmentLoadCoordinatorValidation,
     FourGpuReadyTexturesGateAtomicSkyboxAdoptionAndExactPresentation)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsEnvironmentLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestEnvironment(
        MakeEnvironmentDesc("four-textures.hdr", runtime.GetSceneRuntimeId()), error);
    ASSERT_TRUE(handle.IsValid()) << error;
    AdvanceToPresentation(coordinator, resources, handle);

    const auto waiting = coordinator.GetStatus(handle);
    ASSERT_TRUE(waiting.has_value());
    ASSERT_TRUE(waiting->skyboxEntity.IsValid());
    const auto* skybox = runtime.GetRegistry().TryGet<RVX::SceneECS::Skybox>(waiting->skyboxEntity);
    ASSERT_NE(skybox, nullptr);
    EXPECT_EQ(skybox->mode, RVX::SceneECS::SkyboxMode::Cubemap);
    EXPECT_TRUE(skybox->environmentAssetId.IsValid());
    EXPECT_TRUE(skybox->irradianceAssetId.IsValid());
    EXPECT_TRUE(skybox->prefilteredEnvironmentAssetId.IsValid());
    EXPECT_TRUE(skybox->brdfLutAssetId.IsValid());
    EXPECT_TRUE(runtime.GetRegistry().HasTag<RVX::SceneECS::SpecializedRenderRetirement>(
        waiting->skyboxEntity));

    auto malformed = MakeReceipt(*waiting);
    ++malformed.skyboxWriteVersion;
    EXPECT_FALSE(coordinator.ConfirmPresentation(handle, malformed));
    EXPECT_TRUE(coordinator.ConfirmPresentation(handle, MakeReceipt(*waiting)));
    EXPECT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsEnvironmentLoadState::FullyResident);

    EXPECT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.Update()); // Render proof -> Resource closure request.
    resources.PublishEnvironment();
    ASSERT_TRUE(coordinator.Update()); // Resource closure -> ECS recycle.
    EXPECT_FALSE(coordinator.IsValid(handle));
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(waiting->skyboxEntity));
    EXPECT_EQ(proof.beginCalls, 1u);
    EXPECT_EQ(proof.acknowledgeCalls, 1u);
}

TEST(EcsEnvironmentLoadCoordinatorValidation,
     CancelBeforeAdoptionLeavesNoEcsStateAndShutdownDrainsAfterProof)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof(false);
    RVX::ResourceSceneAdapters::EcsEnvironmentLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto beforeAdoption = coordinator.RequestEnvironment(
        MakeEnvironmentDesc("cancel-before.hdr", runtime.GetSceneRuntimeId()), error);
    ASSERT_TRUE(beforeAdoption.IsValid()) << error;
    EXPECT_TRUE(coordinator.Cancel(beforeAdoption));
    EXPECT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(beforeAdoption));
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 0u);

    const auto active = coordinator.RequestEnvironment(
        MakeEnvironmentDesc("shutdown-drain.hdr", runtime.GetSceneRuntimeId()), error);
    ASSERT_TRUE(active.IsValid()) << error;
    AdvanceToPresentation(coordinator, resources, active);
    EXPECT_TRUE(coordinator.ConfirmPresentation(active, MakeReceipt(*coordinator.GetStatus(active))));
    EXPECT_FALSE(coordinator.PrepareForHostShutdown());
    ASSERT_EQ(coordinator.GetStatus(active)->state,
              RVX::ResourceSceneAdapters::EcsEnvironmentLoadState::AwaitingRenderProof);

    proof.SetPresented(true);
    ASSERT_TRUE(coordinator.Update());
    resources.PublishEnvironment();
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(active));
}

TEST(EcsEnvironmentLoadCoordinatorValidation, RejectedRenderAdmissionRetainsTheAdoptedSkybox)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    proof.SetAdmissionAccepted(false);
    RVX::ResourceSceneAdapters::EcsEnvironmentLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestEnvironment(
        MakeEnvironmentDesc("retained.hdr", runtime.GetSceneRuntimeId()), error);
    ASSERT_TRUE(handle.IsValid()) << error;
    AdvanceToPresentation(coordinator, resources, handle);
    const auto status = coordinator.GetStatus(handle);
    ASSERT_TRUE(status.has_value());
    EXPECT_FALSE(coordinator.Cancel(handle));
    EXPECT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsEnvironmentLoadState::FailedRetained);
    EXPECT_TRUE(runtime.GetRegistry().IsAlive(status->skyboxEntity));

    proof.SetAdmissionAccepted(true);
    EXPECT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.Update());
    resources.PublishEnvironment();
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsEnvironmentLoadCoordinatorValidation,
     ImmutableProfilesProduceDistinctCanonicalAssetsAndPublishValueOnlyMetadata)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsEnvironmentLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    const RVX::Resource::ResourceContentIdentity expectedIdentity = MakeObservedIdentity();
    RVX::ResourceSceneAdapters::EcsEnvironmentLoadDesc desc =
        MakeEnvironmentDesc("profile-low.hdr", runtime.GetSceneRuntimeId());
    desc.environmentOptions.quality = RVX::Resource::HDRIBLQualityProfile::Low;
    desc.environmentOptions.exposure = 1.25f;
    desc.resourceOptions.expectedContentIdentity = expectedIdentity;
    std::string error;
    const auto low = coordinator.RequestEnvironment(std::move(desc), error);
    ASSERT_TRUE(low.IsValid()) << error;

    const auto lowRequested = coordinator.GetStatus(low);
    ASSERT_TRUE(lowRequested.has_value());
    EXPECT_EQ(lowRequested->canonicalImportOptionsHash,
              RVX::Resource::EnvironmentLoader::ComputeImportOptionsHash(
                  lowRequested->environmentOptions));
    EXPECT_EQ(lowRequested->request.assetKey.importOptionsHash,
              lowRequested->canonicalImportOptionsHash);
    const RVX::Resource::EnvironmentLoadOptions smokeOptions{
        .quality = RVX::Resource::HDRIBLQualityProfile::Validation,
        .exposure = 1.5f,
    };
    const RVX::Resource::EnvironmentLoadOptions highOptions{
        .quality = RVX::Resource::HDRIBLQualityProfile::High,
        .exposure = 2.0f,
    };
    const RVX::uint64 smokeHash =
        RVX::Resource::EnvironmentLoader::ComputeImportOptionsHash(smokeOptions);
    const RVX::uint64 highHash =
        RVX::Resource::EnvironmentLoader::ComputeImportOptionsHash(highOptions);
    EXPECT_NE(lowRequested->canonicalImportOptionsHash, smokeHash);
    EXPECT_NE(lowRequested->canonicalImportOptionsHash, highHash);
    EXPECT_NE(smokeHash, highHash);

    AdvanceToPresentation(coordinator, resources, low);
    const auto published = coordinator.GetStatus(low);
    ASSERT_TRUE(published.has_value());
    ASSERT_TRUE(published->contentVerificationReceipt.has_value());
    EXPECT_TRUE(published->contentVerificationReceipt->IsVerified());
    EXPECT_EQ(published->contentVerificationReceipt->expected, expectedIdentity);
    EXPECT_EQ(published->contentVerificationReceipt->observed, expectedIdentity);
    const RVX::Resource::HDRLoadOptions expected =
        RVX::Resource::ResolveHDRIBLQualityProfile(
            RVX::Resource::HDRIBLQualityProfile::Low, 1.25f, false);
    EXPECT_EQ(published->environmentResolution, expected.cubemapResolution);
    EXPECT_EQ(published->irradianceResolution, expected.irradianceResolution);
    EXPECT_EQ(published->prefilteredResolution, expected.prefilteredResolution);
    EXPECT_EQ(published->prefilteredMipLevels, expected.prefilteredMipLevels);
    EXPECT_EQ(published->brdfLUTResolution, expected.brdfLUTResolution);
    EXPECT_FLOAT_EQ(published->exposure, 1.25f);

    EXPECT_TRUE(coordinator.ConfirmPresentation(low, MakeReceipt(*published)));
    EXPECT_TRUE(coordinator.Cancel(low));
    EXPECT_TRUE(coordinator.Update());
    resources.PublishEnvironment();
    EXPECT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(low));
}
