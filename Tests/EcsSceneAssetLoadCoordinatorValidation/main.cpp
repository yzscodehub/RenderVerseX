#include "Core/Log.h"
#include "Resource/PreparedResourceBundle.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/ModelResource.h"
#include "ResourceSceneAdapters/ECS/EcsSceneAssetLoadCoordinator.h"
#include "Scene/ECS/AnimationFragments.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace
{
    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { RVX::Log::Initialize(); }
        void TearDown() override { RVX::Log::Shutdown(); }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    /** @brief Enough Resource gateway surface for a model with no deferred texture sources. */
    class NoopRenderResourceGateway final : public RVX::IRenderResourceGateway
    {
    public:
        RVX::RenderResourceReserveResult ReserveResource(
            RVX::AssetId, RVX::RenderResourceKind) noexcept override
        {
            return {.code = RVX::RenderResourceReserveCode::InvalidAsset};
        }

        RVX::RenderUploadEnqueueResult TryEnqueueUpload(
            const RVX::ResourceUploadRequestRef&) noexcept override
        {
            return {.code = RVX::RenderUploadEnqueueCode::InvalidRequest};
        }

        RVX::RenderReleaseResult RequestRelease(RVX::RenderResourceHandle) noexcept override
        {
            return {.code = RVX::RenderReleaseCode::StaleGeneration};
        }

        [[nodiscard]] RVX::RenderResourceStatus QueryResourceStatus(
            RVX::RenderResourceHandle) const noexcept override
        {
            return {.code = RVX::RenderResourceStatusCode::StaleGeneration,
                    .state = RVX::RenderResourcePublicState::Released};
        }
    };

    class PreparedRootOnlyModelLoader final : public RVX::Resource::IResourceLoader
    {
    public:
        static bool RebindReceiptStreamingTexture(
            RVX::Resource::ResourceId expectedAssetId,
            RVX::Resource::ResourceId replacementAssetId)
        {
            if (s_receiptStreamingModel == nullptr)
            {
                return false;
            }
            const auto found = std::find_if(
                s_receiptStreamingTextures.begin(),
                s_receiptStreamingTextures.end(),
                [replacementAssetId](const auto& value)
                {
                    return value.first == replacementAssetId;
                });
            if (found == s_receiptStreamingTextures.end() || found->second == nullptr)
            {
                return false;
            }
            s_receiptStreamingModel->RebindTextureStreamingDependency(
                expectedAssetId,
                RVX::Resource::ResourceHandle<RVX::Resource::TextureResource>(
                    found->second));
            return true;
        }

        static bool ForceReceiptStreamingCompleteWithoutReplacingPlaceholder(
            RVX::Resource::ResourceId textureAssetId)
        {
            if (s_receiptStreamingModel == nullptr)
            {
                return false;
            }
            static_cast<void>(s_receiptStreamingModel->BeginTextureStreaming());
            s_receiptStreamingModel->MarkTextureDecodeComplete(1);
            s_receiptStreamingModel->MarkTexturePublicationComplete(textureAssetId);
            return s_receiptStreamingModel->GetTextureStreamingSnapshot().stage ==
                   RVX::Resource::ModelTextureStreamingStage::FullyResident;
        }

        static void ClearReceiptStreamingTestState()
        {
            s_receiptStreamingModel = nullptr;
            s_receiptStreamingTextures.clear();
        }

        RVX::ResourceType GetResourceType() const override
        {
            return RVX::ResourceType::Model;
        }

        std::vector<std::string> GetSupportedExtensions() const override { return {".ecsmodel"}; }
        RVX::Resource::IResource* Load(const std::string&) override { return nullptr; }
        bool SupportsPreparedLoading() const override { return true; }

        bool Prepare(const RVX::Resource::ResourceLoadPreparationContext& context,
                     RVX::Resource::PreparedResourceBundle& bundle,
                     RVX::Resource::ResourceLoadError& error) override
        {
            ClearReceiptStreamingTestState();
            auto* model = new RVX::Resource::ModelResource();
            model->SetId(context.rootResourceId);
            model->SetPath(context.requestedPath);
            model->SetName("EcsCoordinatorFixture");
            auto root = std::make_shared<RVX::Node>("Root");
            if (context.requestedPath == "batch-retirement.ecsmodel")
            {
                root->AddChild(std::make_shared<RVX::Node>("Child"));
            }
            if (context.requestedPath == "identity-mapping.ecsmodel")
            {
                root->AddChild(std::make_shared<RVX::Node>("Panel"));
                root->AddChild(std::make_shared<RVX::Node>("Panel"));
                root->AddChild(std::make_shared<RVX::Node>("\xE6\x9C\xBA\xE7\xBF\xBC"));
                root->AddChild(std::make_shared<RVX::Node>(""));
            }
            if (context.requestedPath == "gpu-pending.ecsmodel")
            {
                auto* mesh = new RVX::Resource::MeshResource();
                mesh->SetId(7001);
                mesh->SetMesh(RVX::MeshFactory::CreateTriangle());
                auto* material = new RVX::Resource::MaterialResource();
                material->SetId(7002);
                if (!material->SetMaterialData(std::make_shared<RVX::Material>("GpuPending")))
                {
                    error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                             "Could not stage the test MaterialResource value."};
                    return false;
                }
                model->AddMesh(RVX::Resource::ResourceHandle<RVX::Resource::MeshResource>(mesh));
                model->AddMaterial(
                    RVX::Resource::ResourceHandle<RVX::Resource::MaterialResource>(material));
                if (!bundle.AddDependency(
                        RVX::Resource::ResourceHandle<RVX::Resource::IResource>(mesh)) ||
                    !bundle.AddDependency(
                        RVX::Resource::ResourceHandle<RVX::Resource::IResource>(material)))
                {
                    error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                             "Could not stage the test Model dependencies."};
                    return false;
                }
                root->SetMeshIndex(0);
                root->SetMaterialIndices({0});
            }
            if (context.requestedPath == "material-inventory.ecsmodel")
            {
                const auto makeTexture = [](RVX::Resource::ResourceId id,
                                            RVX::uint32 width,
                                            RVX::uint32 height,
                                            RVX::uint32 mipLevels,
                                            RVX::Resource::TextureFormat format)
                {
                    auto* texture = new RVX::Resource::TextureResource();
                    texture->SetId(id);
                    texture->SetData({0x7fu},
                                     {.width = width,
                                      .height = height,
                                      .mipLevels = mipLevels,
                                      .format = format});
                    return RVX::Resource::ResourceHandle<RVX::Resource::TextureResource>(texture);
                };
                const auto albedo = makeTexture(
                    7201, 128, 64, 7, RVX::Resource::TextureFormat::BC7);
                const auto normal = makeTexture(
                    7202, 64, 32, 5, RVX::Resource::TextureFormat::BC5);
                normal->MarkDefaultFallback("fixture fallback");
                normal->MarkStreamingPlaceholder(true);

                auto* firstMaterial = new RVX::Resource::MaterialResource();
                firstMaterial->SetId(7101);
                auto firstValue = std::make_shared<RVX::Material>("Paint");
                firstValue->SetWorkflow(RVX::MaterialWorkflow::SpecularGlossiness);
                firstValue->SetAlphaMode(RVX::Material::AlphaMode::Blend);
                firstValue->SetBaseColor(0.125f, 0.25f, 0.5f, 0.75f);
                firstValue->SetMetallicFactor(0.7f);
                firstValue->SetRoughnessFactor(0.2f);
                if (!firstMaterial->SetMaterialData(std::move(firstValue)) ||
                    !firstMaterial->SetTexture("normal", normal) ||
                    !firstMaterial->SetTexture("albedo", albedo))
                {
                    error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                             "Could not prepare the first material inventory fixture."};
                    return false;
                }

                auto* secondMaterial = new RVX::Resource::MaterialResource();
                secondMaterial->SetId(7102);
                auto secondValue = std::make_shared<RVX::Material>("Paint");
                secondValue->SetWorkflow(RVX::MaterialWorkflow::Unlit);
                secondValue->SetAlphaMode(RVX::Material::AlphaMode::Mask);
                secondValue->SetBaseColor(0.9f, 0.8f, 0.7f, 1.0f);
                secondValue->SetMetallicFactor(0.0f);
                secondValue->SetRoughnessFactor(1.0f);
                if (!secondMaterial->SetMaterialData(std::move(secondValue)))
                {
                    error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                             "Could not prepare the second material inventory fixture."};
                    return false;
                }

                const RVX::Resource::ResourceHandle<RVX::Resource::MaterialResource> first(
                    firstMaterial);
                const RVX::Resource::ResourceHandle<RVX::Resource::MaterialResource> second(
                    secondMaterial);
                model->AddMaterial(first);
                model->AddMaterial(second);
                if (!bundle.AddDependency(
                        RVX::Resource::ResourceHandle<RVX::Resource::IResource>(albedo)) ||
                    !bundle.AddDependency(
                        RVX::Resource::ResourceHandle<RVX::Resource::IResource>(normal)) ||
                    !bundle.AddDependency(
                        RVX::Resource::ResourceHandle<RVX::Resource::IResource>(first)) ||
                    !bundle.AddDependency(
                        RVX::Resource::ResourceHandle<RVX::Resource::IResource>(second)))
                {
                    error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                             "Could not stage the material inventory dependencies."};
                    return false;
                }
            }
            if (context.requestedPath == "missing-texture-inventory.ecsmodel")
            {
                auto* material = new RVX::Resource::MaterialResource();
                material->SetId(7301);
                if (!material->SetMaterialData(std::make_shared<RVX::Material>("Broken")) ||
                    !material->SetTexture("albedo", {}))
                {
                    error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                             "Could not prepare the malformed material fixture."};
                    return false;
                }
                const RVX::Resource::ResourceHandle<RVX::Resource::MaterialResource> handle(material);
                model->AddMaterial(handle);
                if (!bundle.AddDependency(
                        RVX::Resource::ResourceHandle<RVX::Resource::IResource>(handle)))
                {
                    error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                             "Could not stage the malformed material fixture."};
                    return false;
                }
            }
            if (context.requestedPath == "streaming-receipt.ecsmodel")
            {
                auto* texture = new RVX::Resource::TextureResource();
                texture->SetId(7401);
                texture->SetData({0x3fu},
                                 {.width = 256,
                                  .height = 128,
                                  .mipLevels = 8,
                                  .format = RVX::Resource::TextureFormat::RGBA8});
                texture->MarkStreamingPlaceholder(true);
                const RVX::Resource::ResourceHandle<RVX::Resource::TextureResource> handle(
                    texture);
                model->SetTextureStreamingSources(
                    {{.texture = handle, .estimatedDecodedBytes = 64}});
                if (!bundle.AddDependency(
                        RVX::Resource::ResourceHandle<RVX::Resource::IResource>(handle)))
                {
                    error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                             "Could not stage the streaming texture fixture."};
                    return false;
                }
            }
            if (context.requestedPath == "fully-resident-texture-receipt.ecsmodel" ||
                context.requestedPath == "fully-resident-placeholder-receipt.ecsmodel" ||
                context.requestedPath == "fully-resident-mismatched-receipt.ecsmodel")
            {
                const auto makeTexture = [](RVX::Resource::ResourceId id,
                                            RVX::uint32 width,
                                            RVX::uint32 height,
                                            bool placeholder)
                {
                    auto* texture = new RVX::Resource::TextureResource();
                    texture->SetId(id);
                    texture->SetData({0x2au},
                                     {.width = width,
                                      .height = height,
                                      .mipLevels = 1,
                                      .format = RVX::Resource::TextureFormat::RGBA8});
                    texture->MarkStreamingPlaceholder(placeholder);
                    return texture;
                };

                if (context.requestedPath == "fully-resident-placeholder-receipt.ecsmodel")
                {
                    auto* placeholder = makeTexture(7701, 1, 1, true);
                    const RVX::Resource::ResourceHandle<RVX::Resource::TextureResource>
                        placeholderHandle(placeholder);
                    model->SetTextureStreamingSources(
                        {{.texture = placeholderHandle, .estimatedDecodedBytes = 1}});
                    if (!bundle.AddDependency(
                            RVX::Resource::ResourceHandle<RVX::Resource::IResource>(
                                placeholderHandle)))
                    {
                        error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                                 "Could not stage the placeholder receipt fixture."};
                        return false;
                    }
                    s_receiptStreamingModel = model;
                    s_receiptStreamingTextures.emplace_back(7701, placeholder);
                }
                else
                {
                    const RVX::Resource::ResourceId firstAssetId =
                        context.requestedPath == "fully-resident-mismatched-receipt.ecsmodel" ?
                            7801 :
                            7602;
                    auto* first = makeTexture(firstAssetId, 5, 5, false);
                    auto* second = makeTexture(
                        context.requestedPath == "fully-resident-mismatched-receipt.ecsmodel" ?
                            7802 :
                            7601,
                        5, 5, false);
                    const RVX::Resource::ResourceHandle<RVX::Resource::TextureResource>
                        firstHandle(first);
                    const RVX::Resource::ResourceHandle<RVX::Resource::TextureResource>
                        secondHandle(second);
                    model->SetTextureStreamingSources(
                        {{.texture = firstHandle, .estimatedDecodedBytes = 1},
                         {.texture = secondHandle, .estimatedDecodedBytes = 1}});
                    if (!bundle.AddDependency(
                            RVX::Resource::ResourceHandle<RVX::Resource::IResource>(
                                firstHandle)) ||
                        !bundle.AddDependency(
                            RVX::Resource::ResourceHandle<RVX::Resource::IResource>(
                                secondHandle)))
                    {
                        error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                                 "Could not stage the fully-resident receipt fixture."};
                        return false;
                    }
                    s_receiptStreamingModel = model;
                    s_receiptStreamingTextures.emplace_back(firstAssetId, first);
                    s_receiptStreamingTextures.emplace_back(
                        secondHandle.GetId(), second);
                }
            }
            model->SetRootNode(std::move(root));
            if (!bundle.SetRoot(RVX::Resource::ResourceHandle<RVX::Resource::IResource>(model)))
            {
                error = {RVX::Resource::ResourceLoadErrorCode::LoaderFailure,
                         "Could not stage the test ModelResource."};
                return false;
            }
            return true;
        }

    private:
        inline static RVX::Resource::ModelResource* s_receiptStreamingModel = nullptr;
        inline static std::vector<std::pair<
            RVX::Resource::ResourceId,
            RVX::Resource::TextureResource*>> s_receiptStreamingTextures;
    };

    class ResourceSubsystemScope final
    {
    public:
        explicit ResourceSubsystemScope(RVX::uint32 asyncThreadCount = 0)
        {
            RVX::Resource::ResourceManagerConfig config;
            config.asyncThreadCount = asyncThreadCount;
            m_resources.Initialize(config);
            m_resources.RegisterLoader(
                RVX::ResourceType::Model,
                std::make_unique<PreparedRootOnlyModelLoader>());
        }

        ~ResourceSubsystemScope() { m_resources.Deinitialize(); }

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

        void SetPresented(bool presented) { m_presented = presented; }

        [[nodiscard]] RVX::ResourceSceneAdapters::EcsSceneAssetRetirementBeginReceipt
        BeginRetirement(
            const RVX::ResourceSceneAdapters::EcsSceneAssetRetirementRequest& request) override
        {
            m_request = request;
            return {
                .code = RVX::ResourceSceneAdapters::EcsSceneAssetRetirementBeginCode::Accepted,
                .token = {.value = 1, .generation = 1},
                .request = request,
            };
        }

        [[nodiscard]] RVX::ResourceSceneAdapters::EcsSceneAssetRetirementProof
        QueryRetirementProof(
            RVX::ResourceSceneAdapters::EcsSceneAssetRetirementToken token) const override
        {
            return {
                .state = m_presented ?
                             RVX::ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Presented :
                             RVX::ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Pending,
                .request = token.IsValid() ? m_request :
                    RVX::ResourceSceneAdapters::EcsSceneAssetRetirementRequest{},
                .appliedRenderSceneRevision = m_presented ? 1u : 0u,
                .presentedFrameSequence = m_presented ? 1u : 0u,
            };
        }

        [[nodiscard]] bool AcknowledgeRetirementProof(
            RVX::ResourceSceneAdapters::EcsSceneAssetRetirementToken token) override
        {
            return token.IsValid();
        }

    private:
        RVX::ResourceSceneAdapters::EcsSceneAssetRetirementRequest m_request;
        bool m_presented = true;
    };

    class RejectingRetirementProof final
        : public RVX::ResourceSceneAdapters::IEcsSceneAssetRetirementProofGateway
    {
    public:
        explicit RejectingRetirementProof(bool deviceLost)
            : m_deviceLost(deviceLost)
        {
        }

        void AcceptSubsequentRetirement() { m_acceptSubsequentRetirement = true; }

        [[nodiscard]] RVX::ResourceSceneAdapters::EcsSceneAssetRetirementBeginReceipt
        BeginRetirement(
            const RVX::ResourceSceneAdapters::EcsSceneAssetRetirementRequest& request) override
        {
            if (m_acceptSubsequentRetirement)
            {
                m_request = request;
                return {
                    .code = RVX::ResourceSceneAdapters::EcsSceneAssetRetirementBeginCode::Accepted,
                    .token = {.value = 2, .generation = 1},
                    .request = request,
                };
            }
            return {
                .code = m_deviceLost ?
                            RVX::ResourceSceneAdapters::EcsSceneAssetRetirementBeginCode::DeviceLost :
                            RVX::ResourceSceneAdapters::EcsSceneAssetRetirementBeginCode::FailedRetained,
                .request = request,
                .diagnostic = "Test admission rejection.",
            };
        }

        [[nodiscard]] RVX::ResourceSceneAdapters::EcsSceneAssetRetirementProof
        QueryRetirementProof(RVX::ResourceSceneAdapters::EcsSceneAssetRetirementToken) const override
        {
            return {
                .state = m_acceptSubsequentRetirement ?
                             RVX::ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Presented :
                             RVX::ResourceSceneAdapters::EcsSceneAssetRetirementProofState::Pending,
                .request = m_request,
                .appliedRenderSceneRevision = m_acceptSubsequentRetirement ? 1u : 0u,
                .presentedFrameSequence = m_acceptSubsequentRetirement ? 1u : 0u,
            };
        }

        [[nodiscard]] bool AcknowledgeRetirementProof(
            RVX::ResourceSceneAdapters::EcsSceneAssetRetirementToken token) override
        {
            return token.IsValid();
        }

    private:
        bool m_deviceLost = false;
        bool m_acceptSubsequentRetirement = false;
        RVX::ResourceSceneAdapters::EcsSceneAssetRetirementRequest m_request;
    };

    void PublishModel(RVX::Resource::ResourceSubsystem& resources)
    {
        resources.Tick(0.0f);
    }

    RVX::ResourceSceneAdapters::EcsSceneAssetMinimumResidentPresentationReceipt
    MakeMinimumResidentPresentationReceipt(
        const RVX::ResourceSceneAdapters::EcsSceneAssetLoadStatus& status)
    {
        return {
            .sceneRuntimeId = status.sceneRuntimeId,
            .rootEntity = status.rootEntity,
            .renderableVisibilityVersions = status.renderableVisibilityVersions,
            .frozenSourceSnapshotRevision = 11,
            .renderSceneRevision = 17,
            .carryingPresentedFrameSequence = 23,
        };
    }
} // namespace

TEST(EcsSceneAssetLoadCoordinatorValidation, WorkerCannotMutateAndCancellationBeforeAdoptionLeavesNoEcsState)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "worker.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    bool workerUpdate = true;
    bool workerIsValid = true;
    bool workerHasStatus = true;
    bool workerHasRequestId = true;
    std::thread worker([&]()
    {
        workerUpdate = coordinator.Update();
        workerIsValid = coordinator.IsValid(handle);
        workerHasStatus = coordinator.GetStatus(handle).has_value();
        workerHasRequestId = coordinator.GetResourceRequestId(handle).IsValid();
    });
    worker.join();
    EXPECT_FALSE(workerUpdate);
    EXPECT_FALSE(workerIsValid);
    EXPECT_FALSE(workerHasStatus);
    EXPECT_FALSE(workerHasRequestId);
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 0u);

    EXPECT_TRUE(coordinator.Cancel(handle));
    EXPECT_TRUE(coordinator.Cancel(handle));
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 0u);
    EXPECT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     HostShutdownReportsFalseWhileExactRetirementProofIsStillPending)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof(false);
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "shutdown-pending.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    ASSERT_TRUE(coordinator.Update());
    ASSERT_TRUE(coordinator.Update());
    ASSERT_TRUE(coordinator.Cancel(handle));
    EXPECT_FALSE(coordinator.PrepareForHostShutdown());
    ASSERT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::AwaitingRenderProof);

    proof.SetPresented(true);
    ASSERT_TRUE(coordinator.Update());
    resources.m_resources.Tick(0.0f);
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     AwaitingPublishRemainsNonAdoptableUntilOwnerPublicationCommits)
{
    ResourceSubsystemScope resources(1);
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "awaiting-publish.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;

    bool observedAwaitingPublish = false;
    for (RVX::uint32 attempt = 0; attempt < 100; ++attempt)
    {
        ASSERT_TRUE(coordinator.Update());
        const auto status = coordinator.GetStatus(handle);
        ASSERT_TRUE(status.has_value());
        if (status->request.state == RVX::Resource::ResourceLoadState::AwaitingPublish)
        {
            observedAwaitingPublish = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(observedAwaitingPublish);

    for (RVX::uint32 repeat = 0; repeat < 3; ++repeat)
    {
        ASSERT_TRUE(coordinator.Update());
        const auto status = coordinator.GetStatus(handle);
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(status->request.state, RVX::Resource::ResourceLoadState::AwaitingPublish);
        EXPECT_EQ(status->state, RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::PreparedCPU);
        EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 0u);
        EXPECT_FALSE(resources.m_resources.GetManager().AcquireAssetResidencyLease(status->assetKey).IsValid());
    }

    PublishModel(resources.m_resources);
    ASSERT_TRUE(coordinator.Update()); // Ready -> PendingAdopt; only Ready builds the batch.
    ASSERT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::PendingAdopt);
    ASSERT_TRUE(coordinator.Update()); // PendingAdopt -> hidden atomic ECS adoption.
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 1u);

    ASSERT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.Update());
    resources.m_resources.Tick(0.0f);
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     PublishedModelMetadataCapturesPbrValuesOrderedSlotsAndTextureResidency)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "material-inventory.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    ASSERT_TRUE(coordinator.Update());

    const auto status = coordinator.GetStatus(handle);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(status->state, RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::PendingAdopt);
    const auto& metadata = status->modelMetadata;
    ASSERT_EQ(metadata.materialAssetIds.size(), 2u);
    EXPECT_EQ(metadata.materialAssetIds[0].value, 7101u);
    EXPECT_EQ(metadata.materialAssetIds[1].value, 7102u);
    ASSERT_EQ(metadata.materials.size(), 2u);
    EXPECT_EQ(metadata.materials[0].sourceName, "Paint");
    EXPECT_EQ(metadata.materials[1].sourceName, "Paint");
    EXPECT_EQ(metadata.materials[0].workflow,
              RVX::Resource::MaterialWorkflowMode::SpecularGlossiness);
    EXPECT_EQ(metadata.materials[0].alphaMode, RVX::Resource::MaterialAlphaMode::Blend);
    EXPECT_FLOAT_EQ(metadata.materials[0].baseColor.x, 0.125f);
    EXPECT_FLOAT_EQ(metadata.materials[0].baseColor.y, 0.25f);
    EXPECT_FLOAT_EQ(metadata.materials[0].baseColor.z, 0.5f);
    EXPECT_FLOAT_EQ(metadata.materials[0].baseColor.w, 0.75f);
    EXPECT_FLOAT_EQ(metadata.materials[0].metallicFactor, 0.7f);
    EXPECT_FLOAT_EQ(metadata.materials[0].roughnessFactor, 0.2f);
    EXPECT_EQ(metadata.materials[1].workflow, RVX::Resource::MaterialWorkflowMode::Unlit);
    EXPECT_EQ(metadata.materials[1].alphaMode, RVX::Resource::MaterialAlphaMode::Mask);
    ASSERT_EQ(metadata.materials[0].textureSlots.size(), 2u);
    EXPECT_EQ(metadata.materials[0].textureSlots[0].slot, "albedo");
    EXPECT_EQ(metadata.materials[0].textureSlots[0].textureAssetId.value, 7201u);
    EXPECT_EQ(metadata.materials[0].textureSlots[1].slot, "normal");
    EXPECT_EQ(metadata.materials[0].textureSlots[1].textureAssetId.value, 7202u);
    ASSERT_EQ(metadata.textures.size(), 2u);
    EXPECT_EQ(metadata.textures[0].textureAssetId.value, 7201u);
    EXPECT_EQ(metadata.textures[0].width, 128u);
    EXPECT_EQ(metadata.textures[0].height, 64u);
    EXPECT_EQ(metadata.textures[0].mipLevels, 7u);
    EXPECT_EQ(metadata.textures[0].format, RVX::Resource::TextureFormat::BC7);
    EXPECT_FALSE(metadata.textures[0].isDefaultFallback);
    EXPECT_FALSE(metadata.textures[0].isStreamingPlaceholder);
    EXPECT_EQ(metadata.textures[1].textureAssetId.value, 7202u);
    EXPECT_EQ(metadata.textures[1].width, 64u);
    EXPECT_EQ(metadata.textures[1].height, 32u);
    EXPECT_EQ(metadata.textures[1].mipLevels, 5u);
    EXPECT_EQ(metadata.textures[1].format, RVX::Resource::TextureFormat::BC5);
    EXPECT_TRUE(metadata.textures[1].isDefaultFallback);
    EXPECT_TRUE(metadata.textures[1].isStreamingPlaceholder);
    EXPECT_EQ(metadata.textures[1].fallbackReason, "fixture fallback");

    ASSERT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     MissingOrUnloadedMetadataDependencyFailsClosedWithoutPartialInventory)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "missing-texture-inventory.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    EXPECT_FALSE(coordinator.Update());

    const auto status = coordinator.GetStatus(handle);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->state, RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::Failed);
    EXPECT_FALSE(status->modelMetadata.HasPublishedSource());
    EXPECT_TRUE(status->modelMetadata.materials.empty());
    EXPECT_TRUE(status->modelMetadata.textures.empty());
    EXPECT_FALSE(status->textureStreamingStartReceipt.has_value());

    ASSERT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     TextureStreamingReceiptRequiresPresentationAndEchoesExactAuthorizedValues)
{
    ResourceSubsystemScope resources;
    NoopRenderResourceGateway resourceGateway;
    resources.m_resources.SetRenderResourceGateway(&resourceGateway);
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "streaming-receipt.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    ASSERT_TRUE(coordinator.Update()); // Ready -> PendingAdopt
    ASSERT_TRUE(coordinator.Update()); // PendingAdopt -> CPUReadyHidden
    ASSERT_TRUE(coordinator.Update()); // CPUReadyHidden -> presentation gate
    const auto pending = coordinator.GetStatus(handle);
    ASSERT_TRUE(pending.has_value());
    ASSERT_EQ(pending->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::MinimumResidentPendingPresentation);
    EXPECT_FALSE(pending->textureStreamingStartReceipt.has_value());
    EXPECT_FALSE(pending->fullyResidentTextureReceipt.has_value());
    const auto authorization = MakeMinimumResidentPresentationReceipt(*pending);
    ASSERT_TRUE(coordinator.ConfirmMinimumResidentPresentation(handle, authorization));
    EXPECT_FALSE(coordinator.GetStatus(handle)->textureStreamingStartReceipt.has_value());

    const RVX::uint64 expectedStartFrame = runtime.GetDiagnosticsSnapshot().frameSequence;
    ASSERT_TRUE(coordinator.Update()); // MinimumResident -> begin deferred texture streaming
    const auto streaming = coordinator.GetStatus(handle);
    ASSERT_TRUE(streaming.has_value());
    ASSERT_EQ(streaming->state, RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::Streaming);
    ASSERT_TRUE(streaming->textureStreamingStartReceipt.has_value());
    const auto& receipt = *streaming->textureStreamingStartReceipt;
    EXPECT_TRUE(receipt.IsAuthorized());
    EXPECT_EQ(receipt.authorizedFrozenSourceSnapshotRevision,
              authorization.frozenSourceSnapshotRevision);
    EXPECT_EQ(receipt.authorizedRenderSceneRevision, authorization.renderSceneRevision);
    EXPECT_EQ(receipt.authorizedPresentedFrameSequence,
              authorization.carryingPresentedFrameSequence);
    EXPECT_EQ(receipt.startSceneFrameSequence, expectedStartFrame);
    ASSERT_EQ(receipt.textureAssetIds.size(), 1u);
    EXPECT_EQ(receipt.textureAssetIds.front().value, 7401u);

    ASSERT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.GetStatus(handle).has_value());
    EXPECT_FALSE(coordinator.GetStatus(handle)->textureStreamingStartReceipt.has_value());
    for (RVX::uint32 attempt = 0; coordinator.IsValid(handle) && attempt < 20; ++attempt)
    {
        ASSERT_TRUE(coordinator.Update());
        resources.m_resources.Tick(0.0f);
    }
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     FullyResidentTextureReceiptPublishesStableCurrentValuesOnlyAfterExactCompletion)
{
    ResourceSubsystemScope resources;
    NoopRenderResourceGateway resourceGateway;
    resources.m_resources.SetRenderResourceGateway(&resourceGateway);
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestModel(
        {.path = "fully-resident-texture-receipt.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    ASSERT_TRUE(coordinator.Update()); // Ready -> PendingAdopt
    ASSERT_TRUE(coordinator.Update()); // PendingAdopt -> CPUReadyHidden
    ASSERT_TRUE(coordinator.Update()); // CPUReadyHidden -> presentation gate
    const auto pending = coordinator.GetStatus(handle);
    ASSERT_TRUE(pending.has_value());
    EXPECT_FALSE(pending->fullyResidentTextureReceipt.has_value());
    ASSERT_TRUE(coordinator.ConfirmMinimumResidentPresentation(
        handle, MakeMinimumResidentPresentationReceipt(*pending)));

    // The exact source order is intentionally 7602 then 7601. Rebinding the
    // existing, already non-placeholder values makes the no-decode fixture
    // fully resident without altering that source identity inventory.
    ASSERT_TRUE(PreparedRootOnlyModelLoader::RebindReceiptStreamingTexture(7602, 7602));
    ASSERT_TRUE(PreparedRootOnlyModelLoader::RebindReceiptStreamingTexture(7601, 7601));
    ASSERT_TRUE(coordinator.Update()); // MinimumResident -> Streaming
    ASSERT_TRUE(coordinator.Update()); // Streaming -> FullyResident + value receipt

    const auto fullyResident = coordinator.GetStatus(handle);
    ASSERT_TRUE(fullyResident.has_value());
    ASSERT_EQ(fullyResident->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::FullyResident);
    ASSERT_TRUE(fullyResident->fullyResidentTextureReceipt.has_value());
    const auto& receipt = *fullyResident->fullyResidentTextureReceipt;
    EXPECT_EQ(receipt.sourceModelAssetId,
              fullyResident->modelMetadata.sourceModelAssetId);
    ASSERT_EQ(receipt.textures.size(), 2u);
    EXPECT_EQ(receipt.textures[0].textureAssetId.value, 7602u);
    EXPECT_EQ(receipt.textures[1].textureAssetId.value, 7601u);
    for (const auto& texture : receipt.textures)
    {
        EXPECT_EQ(texture.width, 5u);
        EXPECT_EQ(texture.height, 5u);
        EXPECT_EQ(texture.mipLevels, 1u);
        EXPECT_EQ(texture.format, RVX::Resource::TextureFormat::RGBA8);
        EXPECT_FALSE(texture.isDefaultFallback);
        EXPECT_FALSE(texture.isStreamingPlaceholder);
    }

    ASSERT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.Update());
    resources.m_resources.Tick(0.0f);
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
    PreparedRootOnlyModelLoader::ClearReceiptStreamingTestState();
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     FullyResidentTextureReceiptFailsClosedForPlaceholderOrSourceIdentityMismatch)
{
    for (const std::string_view path : {
             "fully-resident-placeholder-receipt.ecsmodel",
             "fully-resident-mismatched-receipt.ecsmodel"})
    {
        ResourceSubsystemScope resources;
        NoopRenderResourceGateway resourceGateway;
        resources.m_resources.SetRenderResourceGateway(&resourceGateway);
        RVX::SceneECS::SceneEcsRuntime runtime;
        PresentedRetirementProof proof;
        RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
            runtime, resources.m_resources, &proof);

        std::string error;
        const auto handle = coordinator.RequestModel({.path = std::string(path)}, error);
        ASSERT_TRUE(handle.IsValid()) << error;
        PublishModel(resources.m_resources);
        ASSERT_TRUE(coordinator.Update()); // Ready -> PendingAdopt
        ASSERT_TRUE(coordinator.Update()); // PendingAdopt -> CPUReadyHidden
        ASSERT_TRUE(coordinator.Update()); // CPUReadyHidden -> presentation gate
        const auto pending = coordinator.GetStatus(handle);
        ASSERT_TRUE(pending.has_value());
        ASSERT_TRUE(coordinator.ConfirmMinimumResidentPresentation(
            handle, MakeMinimumResidentPresentationReceipt(*pending)));

        if (path == "fully-resident-placeholder-receipt.ecsmodel")
        {
            // This deliberately models a broken streamer that reports full
            // residency without replacing its 1x1 placeholder value.
            ASSERT_TRUE(
                PreparedRootOnlyModelLoader::ForceReceiptStreamingCompleteWithoutReplacingPlaceholder(
                    7701));
        }
        else
        {
            // This is a loaded foreign value, not an invalid handle. The
            // coordinator must still reject it because it changes the source
            // inventory AssetId/order captured at CPU-ready time.
            ASSERT_TRUE(PreparedRootOnlyModelLoader::RebindReceiptStreamingTexture(7801, 7802));
            ASSERT_TRUE(PreparedRootOnlyModelLoader::RebindReceiptStreamingTexture(7802, 7802));
        }

        ASSERT_TRUE(coordinator.Update()); // MinimumResident -> Streaming
        EXPECT_FALSE(coordinator.Update()); // receipt construction fails closed
        const auto rejected = coordinator.GetStatus(handle);
        ASSERT_TRUE(rejected.has_value());
        EXPECT_NE(rejected->state,
                  RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::FullyResident);
        EXPECT_FALSE(rejected->fullyResidentTextureReceipt.has_value());
        EXPECT_NE(rejected->diagnostic.find("exact value-only receipt"), std::string::npos);

        ASSERT_TRUE(coordinator.Update());
        resources.m_resources.Tick(0.0f);
        ASSERT_TRUE(coordinator.Update());
        EXPECT_FALSE(coordinator.IsValid(handle));
        PreparedRootOnlyModelLoader::ClearReceiptStreamingTestState();
    }
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     InvalidBatchDestroyMemberCannotMarkAnyOtherMemberPendingDestroy)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle valid = runtime.CreateEntity();
    ASSERT_TRUE(valid.IsValid());
    const RVX::SceneECS::CleanupDomainMask allDomains =
        RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::All);
    const std::array destroySet{valid, RVX::ECS::EntityHandle::Invalid()};
    EXPECT_EQ(runtime.RequestDestroyBatch(destroySet, allDomains),
              RVX::SceneECS::DestroyRequestResult::InvalidEntity);
    const RVX::SceneECS::EntityLifecycleState* lifecycle =
        runtime.GetRegistry().TryGet<RVX::SceneECS::EntityLifecycleState>(valid);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->phase, RVX::SceneECS::EntityLifecyclePhase::Alive);
    RVX::SceneECS::CleanupRecordCursor cleanupCursor;
    EXPECT_TRUE(runtime.ReadCleanupRecords(cleanupCursor).records.empty());
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     StreamingStartFailureBeginsRetirementAndRecyclesAfterExactRenderProof)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "streaming-failure.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    ASSERT_TRUE(coordinator.Update()); // Ready -> PendingAdopt
    ASSERT_TRUE(coordinator.Update()); // PendingAdopt -> hidden ECS adoption
    ASSERT_TRUE(coordinator.Update()); // CPUReadyHidden -> presentation wait

    const auto waiting = coordinator.GetStatus(handle);
    ASSERT_TRUE(waiting.has_value());
    ASSERT_EQ(waiting->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::MinimumResidentPendingPresentation);
    auto malformedReceipt = MakeMinimumResidentPresentationReceipt(*waiting);
    malformedReceipt.rootEntity = RVX::ECS::EntityHandle::Invalid();
    EXPECT_FALSE(coordinator.ConfirmMinimumResidentPresentation(handle, malformedReceipt));
    EXPECT_TRUE(coordinator.ConfirmMinimumResidentPresentation(
        handle, MakeMinimumResidentPresentationReceipt(*waiting)));
    ASSERT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::MinimumResident);

    // This fixture deliberately supplies no Resource render gateway, so deferred
    // streaming start is a deterministic failure that must retire, not retain.
    EXPECT_FALSE(coordinator.Update());
    ASSERT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::AwaitingRenderProof);
    EXPECT_FALSE(coordinator.GetStatus(handle)->textureStreamingStartReceipt.has_value());
    ASSERT_TRUE(coordinator.Update()); // exact Render proof -> Resource closure ticket
    resources.m_resources.Tick(0.0f);
    ASSERT_TRUE(coordinator.Update()); // terminal closure -> recycle
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation, AdoptionIsHiddenAndForeignRuntimeRollsBackWithoutEntityLeak)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    RVX::SceneECS::SceneEcsRuntime foreign;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources);

    std::string error;
    const auto mismatch = coordinator.RequestModel(
        {.path = "mismatch.ecsmodel", .expectedSceneRuntimeId = foreign.GetSceneRuntimeId()}, error);
    ASSERT_TRUE(mismatch.IsValid()) << error;
    PublishModel(resources.m_resources);
    EXPECT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.Update());
    ASSERT_TRUE(coordinator.GetStatus(mismatch).has_value());
    EXPECT_EQ(coordinator.GetStatus(mismatch)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::Failed);
    EXPECT_TRUE(coordinator.GetStatus(mismatch)->entityMappings.empty());
    EXPECT_TRUE(coordinator.GetStatus(mismatch)->members.empty());
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 0u);

    const auto modelKey = coordinator.GetStatus(mismatch)->assetKey;
    RVX::Resource::AssetResidencyLease retry =
        resources.m_resources.GetManager().AcquireAssetResidencyLease(modelKey);
    EXPECT_TRUE(retry.IsValid());
}

TEST(EcsSceneAssetLoadCoordinatorValidation, PresentationReceiptGatesStreamingAndRetirementIsIdempotent)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "visible.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    ASSERT_TRUE(coordinator.Update()); // Ready -> PendingAdopt
    ASSERT_TRUE(coordinator.Update()); // PendingAdopt -> hidden ECS adoption
    ASSERT_TRUE(coordinator.Update()); // hidden -> safe visibility write
    ASSERT_TRUE(coordinator.GetStatus(handle).has_value());
    EXPECT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::MinimumResidentPendingPresentation);
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 1u);

    EXPECT_TRUE(coordinator.Update());
    EXPECT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::MinimumResidentPendingPresentation);

    const auto waiting = coordinator.GetStatus(handle);
    ASSERT_TRUE(waiting.has_value());
    auto malformedReceipt = MakeMinimumResidentPresentationReceipt(*waiting);
    malformedReceipt.frozenSourceSnapshotRevision = 0;
    EXPECT_FALSE(coordinator.ConfirmMinimumResidentPresentation(handle, malformedReceipt));
    EXPECT_TRUE(coordinator.ConfirmMinimumResidentPresentation(
        handle, MakeMinimumResidentPresentationReceipt(*waiting)));
    EXPECT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::MinimumResident);

    EXPECT_TRUE(coordinator.Cancel(handle));
    EXPECT_TRUE(coordinator.Cancel(handle));
    EXPECT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::AwaitingRenderProof);
    ASSERT_TRUE(coordinator.Update()); // exact Render proof -> Resource closure ticket
    resources.m_resources.Tick(0.0f);
    ASSERT_TRUE(coordinator.Update()); // terminal closure -> Resources acknowledgement/recycle
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     ResourceClosureDoesNotRecycleUntilEveryInferredSubsystemAcknowledgesCleanup)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "multi-domain.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    ASSERT_TRUE(coordinator.Update());
    ASSERT_TRUE(coordinator.Update());
    const auto adopted = coordinator.GetStatus(handle);
    ASSERT_TRUE(adopted.has_value());
    ASSERT_TRUE(adopted->rootEntity.IsValid());
    ASSERT_TRUE(runtime.AddFragment<RVX::SceneECS::Animator>(adopted->rootEntity, {}));

    ASSERT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.Update());
    resources.m_resources.Tick(0.0f);
    ASSERT_TRUE(coordinator.Update());
    ASSERT_TRUE(coordinator.IsValid(handle));
    ASSERT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::AwaitingEcsRecycle);
    EXPECT_TRUE(runtime.GetRegistry().IsAlive(adopted->rootEntity));

    ASSERT_TRUE(runtime.AcknowledgeCleanup(
        adopted->rootEntity,
        RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Animation)));
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(adopted->rootEntity));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     UnresolvedMinimumGpuDependenciesKeepTheAtomicallyAdoptedMeshHidden)
{
    ResourceSubsystemScope resources;
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "gpu-pending.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    const bool prepared = coordinator.Update();
    const auto preparedStatus = coordinator.GetStatus(handle);
    ASSERT_TRUE(prepared) << static_cast<int>(preparedStatus->state) << ": "
                          << preparedStatus->diagnostic;
    // Ready -> PendingAdopt
    const bool adopted = coordinator.Update();
    const auto adoptedStatus = coordinator.GetStatus(handle);
    ASSERT_TRUE(adopted) << static_cast<int>(adoptedStatus->state) << ": "
                         << adoptedStatus->diagnostic;
    // PendingAdopt -> atomically hidden mesh ECS adoption
    const auto cpuHidden = coordinator.GetStatus(handle);
    ASSERT_TRUE(cpuHidden.has_value());
    ASSERT_TRUE(cpuHidden->modelMetadata.HasPublishedSource());
    ASSERT_EQ(cpuHidden->modelMetadata.meshAssetIds.size(), 1u);
    ASSERT_EQ(cpuHidden->modelMetadata.meshAssetIds.front().value, 7001u);
    ASSERT_EQ(cpuHidden->modelMetadata.materialAssetIds.size(), 1u);
    ASSERT_EQ(cpuHidden->modelMetadata.materialAssetIds.front().value, 7002u);
    EXPECT_TRUE(cpuHidden->modelMetadata.streamingTextureAssetIds.empty());
    EXPECT_FALSE(cpuHidden->modelMetadata.animationAssetId.IsValid());
    EXPECT_FALSE(cpuHidden->fullyResidentTextureReceipt.has_value());
    const RVX::ECS::EntityHandle root = cpuHidden->rootEntity;
    const RVX::SceneECS::Visibility* initialVisibility =
        runtime.GetRegistry().TryGet<RVX::SceneECS::Visibility>(root);
    ASSERT_NE(initialVisibility, nullptr);
    EXPECT_FALSE(initialVisibility->visible);

    ASSERT_TRUE(coordinator.Update()); // No RenderResource resolve entry: remains CPUReadyHidden.
    EXPECT_EQ(coordinator.GetStatus(handle)->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::CPUReadyHidden);
    const RVX::SceneECS::Visibility* pendingVisibility =
        runtime.GetRegistry().TryGet<RVX::SceneECS::Visibility>(root);
    ASSERT_NE(pendingVisibility, nullptr);
    EXPECT_FALSE(pendingVisibility->visible);

    ASSERT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.Update());
    resources.m_resources.Tick(0.0f);
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     StatusRetainsExactNodeMappingsFromCpuHiddenThroughFullResidency)
{
    ResourceSubsystemScope resources;
    NoopRenderResourceGateway resourceGateway;
    resources.m_resources.SetRenderResourceGateway(&resourceGateway);
    RVX::SceneECS::SceneEcsRuntime runtime;
    PresentedRetirementProof proof;
    RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
        runtime, resources.m_resources, &proof);

    std::string error;
    const auto handle = coordinator.RequestModel({.path = "identity-mapping.ecsmodel"}, error);
    ASSERT_TRUE(handle.IsValid()) << error;
    PublishModel(resources.m_resources);
    ASSERT_TRUE(coordinator.Update()); // Ready -> PendingAdopt
    ASSERT_TRUE(coordinator.Update()); // PendingAdopt -> CPUReadyHidden

    const auto cpuHidden = coordinator.GetStatus(handle);
    ASSERT_TRUE(cpuHidden.has_value());
    ASSERT_EQ(cpuHidden->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::CPUReadyHidden);
    EXPECT_TRUE(cpuHidden->modelMetadata.HasPublishedSource());
    EXPECT_EQ(cpuHidden->modelMetadata.sourceNodeCount, 5u);
    EXPECT_TRUE(cpuHidden->modelMetadata.meshAssetIds.empty());
    EXPECT_TRUE(cpuHidden->modelMetadata.materialAssetIds.empty());
    EXPECT_TRUE(cpuHidden->modelMetadata.streamingTextureAssetIds.empty());
    EXPECT_FALSE(cpuHidden->modelMetadata.animationAssetId.IsValid());
    ASSERT_EQ(cpuHidden->entityMappings.size(), 5u);
    ASSERT_EQ(cpuHidden->members.size(), cpuHidden->entityMappings.size());
    EXPECT_TRUE(std::is_sorted(
        cpuHidden->entityMappings.begin(), cpuHidden->entityMappings.end(),
        [](const RVX::ResourceSceneAdapters::PreparedModelEntityMapping& lhs,
           const RVX::ResourceSceneAdapters::PreparedModelEntityMapping& rhs)
        {
            return lhs.temporaryNodeId < rhs.temporaryNodeId;
        }));
    const auto panels = cpuHidden->FindEntitiesByExactSourceName("Panel");
    ASSERT_EQ(panels.size(), 2u);
    EXPECT_FALSE(cpuHidden->FindUniqueEntityByExactSourceName("Panel").has_value());
    const auto utf8Name = cpuHidden->FindEntitiesByExactSourceName(
        "\xE6\x9C\xBA\xE7\xBF\xBC");
    ASSERT_EQ(utf8Name.size(), 1u);
    const auto emptyName = cpuHidden->FindEntitiesByExactSourceName("");
    ASSERT_EQ(emptyName.size(), 1u);
    ASSERT_TRUE(cpuHidden->FindUniqueEntityByExactSourceName("Root").has_value());
    EXPECT_EQ(*cpuHidden->FindUniqueEntityByExactSourceName("Root"), cpuHidden->rootEntity);
    for (const RVX::ResourceSceneAdapters::PreparedModelEntityMapping& mapping :
         cpuHidden->entityMappings)
    {
        EXPECT_TRUE(mapping.sourceNodeIdentity.IsValid());
        EXPECT_EQ(cpuHidden->FindEntityByTemporaryNodeId(mapping.temporaryNodeId), mapping.entity);
    }

    ASSERT_TRUE(coordinator.Update()); // CPUReadyHidden -> presentation gate (no mesh dependencies)
    const auto pendingPresentation = coordinator.GetStatus(handle);
    ASSERT_TRUE(pendingPresentation.has_value());
    ASSERT_EQ(pendingPresentation->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::MinimumResidentPendingPresentation);
    ASSERT_TRUE(coordinator.ConfirmMinimumResidentPresentation(
        handle, MakeMinimumResidentPresentationReceipt(*pendingPresentation)));
    ASSERT_TRUE(coordinator.Update()); // MinimumResident -> Streaming
    ASSERT_TRUE(coordinator.Update()); // Streaming -> FullyResident (no texture sources)

    const auto fullyResident = coordinator.GetStatus(handle);
    ASSERT_TRUE(fullyResident.has_value());
    ASSERT_EQ(fullyResident->state,
              RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::FullyResident);
    ASSERT_TRUE(fullyResident->fullyResidentTextureReceipt.has_value());
    EXPECT_EQ(fullyResident->fullyResidentTextureReceipt->sourceModelAssetId,
              fullyResident->modelMetadata.sourceModelAssetId);
    EXPECT_TRUE(fullyResident->fullyResidentTextureReceipt->textures.empty());
    ASSERT_EQ(fullyResident->entityMappings.size(), cpuHidden->entityMappings.size());
    EXPECT_EQ(fullyResident->modelMetadata.sourceModelAssetId,
              cpuHidden->modelMetadata.sourceModelAssetId);
    EXPECT_EQ(fullyResident->modelMetadata.sourceNodeCount,
              cpuHidden->modelMetadata.sourceNodeCount);
    EXPECT_EQ(fullyResident->modelMetadata.meshAssetIds,
              cpuHidden->modelMetadata.meshAssetIds);
    EXPECT_EQ(fullyResident->modelMetadata.materialAssetIds,
              cpuHidden->modelMetadata.materialAssetIds);
    EXPECT_EQ(fullyResident->modelMetadata.streamingTextureAssetIds,
              cpuHidden->modelMetadata.streamingTextureAssetIds);
    EXPECT_EQ(fullyResident->modelMetadata.animationAssetId,
              cpuHidden->modelMetadata.animationAssetId);
    EXPECT_EQ(fullyResident->modelMetadata.contentVerificationReceipt.status,
              cpuHidden->modelMetadata.contentVerificationReceipt.status);
    for (size_t index = 0; index < fullyResident->entityMappings.size(); ++index)
    {
        const auto& expected = cpuHidden->entityMappings[index];
        const auto& actual = fullyResident->entityMappings[index];
        EXPECT_EQ(actual.temporaryNodeId, expected.temporaryNodeId);
        EXPECT_EQ(actual.entity, expected.entity);
        EXPECT_EQ(actual.sourceName, expected.sourceName);
        EXPECT_EQ(actual.sourceNodeIdentity, expected.sourceNodeIdentity);
        EXPECT_EQ(actual.nodeName, expected.nodeName);
        EXPECT_EQ(actual.derivedPrimitiveOrdinal, expected.derivedPrimitiveOrdinal);
    }

    ASSERT_TRUE(coordinator.Cancel(handle));
    ASSERT_TRUE(coordinator.Update());
    resources.m_resources.Tick(0.0f);
    ASSERT_TRUE(coordinator.Update());
    EXPECT_FALSE(coordinator.IsValid(handle));
}

TEST(EcsSceneAssetLoadCoordinatorValidation,
     RetirementAdmissionFailureRetainsTheInstanceAndReportsDeviceLossSeparately)
{
    for (const bool deviceLost : {false, true})
    {
        ResourceSubsystemScope resources;
        RVX::SceneECS::SceneEcsRuntime runtime;
        RejectingRetirementProof proof(deviceLost);
        RVX::ResourceSceneAdapters::EcsSceneAssetLoadCoordinator coordinator(
            runtime, resources.m_resources, &proof);

        std::string error;
        const auto handle = coordinator.RequestModel({.path = "retained.ecsmodel"}, error);
        ASSERT_TRUE(handle.IsValid()) << error;
        PublishModel(resources.m_resources);
        ASSERT_TRUE(coordinator.Update());
        ASSERT_TRUE(coordinator.Update());
        ASSERT_TRUE(coordinator.Update());
        ASSERT_EQ(runtime.GetRegistry().GetEntityCount(), 1u);

        EXPECT_FALSE(coordinator.Cancel(handle));
        ASSERT_TRUE(coordinator.GetStatus(handle).has_value());
        EXPECT_EQ(coordinator.GetStatus(handle)->state,
                  deviceLost ? RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::DeviceLost :
                               RVX::ResourceSceneAdapters::EcsSceneAssetLoadState::FailedRetained);
        // Admission precedes RequestDestroy, so a rejected proof cannot partially retire the ECS instance.
        EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), 1u);

        proof.AcceptSubsequentRetirement();
        ASSERT_TRUE(coordinator.Cancel(handle));
        ASSERT_TRUE(coordinator.Update());
        resources.m_resources.Tick(0.0f);
        ASSERT_TRUE(coordinator.Update());
        EXPECT_FALSE(coordinator.IsValid(handle));
    }
}
