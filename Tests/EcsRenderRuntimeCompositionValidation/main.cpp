#include "EcsRenderRuntimeComposition.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
using namespace RVX;
using namespace RVX::SceneECS;
using namespace RVX::ResourceSceneAdapters;

class FakeTransport final : public IEcsRenderFrameTransport
{
public:
    EcsRenderFrameTransportDisposition disposition =
        EcsRenderFrameTransportDisposition::Accepted;
    EcsSceneRenderPublicationProgress progress{};
    uint32 publishCount = 0;
    RenderFrameHeaderV5 lastHeader{};
    bool lastTemporalReset = false;
    RenderFrameCaptureRequest lastCaptureRequest{};

    EcsRenderFrameTransportReceipt TryPublish(
        EcsRenderFrameTransportCandidate candidate) noexcept override
    {
        ++publishCount;
        if (candidate.frameV5 != nullptr)
        {
            lastHeader = candidate.frameV5->GetHeader();
            lastTemporalReset = candidate.frameV5->GetSettings().temporal.resetHistory;
            lastCaptureRequest = candidate.frameV5->GetCaptureRequest();
        }
        return {
            .disposition = candidate.IsStructurallyValid()
                               ? disposition
                               : EcsRenderFrameTransportDisposition::NotAccepted,
            .candidateIdentity = candidate.candidateIdentity,
            .sourceSceneRuntimeId = candidate.sourceSceneRuntimeId,
            .sourceSnapshotRevision = candidate.sourceSnapshotRevision,
            .targetRenderSceneRevision = candidate.targetRenderSceneRevision,
            .frameSequence = candidate.frameSequence,
            .completedProgress = progress,
        };
    }
};

class FakeServices final : public IEcsRenderRuntimeCompositionServices
{
public:
    bool windowInitialized = true;
    bool renderReady = true;
    bool assetsReady = true;
    uint32 injectedGatewayCount = 0;
    uint32 configureCount = 0;
    EcsSceneRenderPublicationProgress completedProgress{};
    NativeSurfaceDesc surface{
        .platform = NativeSurfacePlatform::Win32,
        .nativeWindow = 1,
        .width = 640,
        .height = 480,
        .generation = 1,
    };
    RenderResizeResult resizeResult{
        .code = RenderResizeCode::Accepted,
        .generation = 2,
    };
    bool windowResizeAccepted = true;
    uint32 windowResizeRequestCount = 0;
    uint32 renderResizeRequestCount = 0;
    NativeSurfaceDesc lastRenderResizeSurface{};
    FakeTransport transport;
    RenderShutdownResult stopResult{.code = RenderShutdownCode::Completed};
    RenderFrameCaptureResult captureResult{};
    std::vector<std::string> shutdownOrder;

    void InjectRenderResourceGateway() override { ++injectedGatewayCount; }
    bool IsWindowInitialized() const noexcept override { return windowInitialized; }
    NativeSurfaceDesc CaptureRenderSurface() override { return surface; }
    void ReleaseGraphicsContextFromUpdateThread() override {}
    void ConfigureRender(const RenderRuntimeConfig&,
                         const NativeSurfaceDesc&) override
    {
        ++configureCount;
    }
    bool IsRenderReady() const noexcept override { return renderReady; }
    IEcsRenderFrameTransport& GetFrameTransport() noexcept override { return transport; }
    EcsSceneRenderPublicationProgress ReadCompletedProgress() const noexcept override
    {
        return completedProgress;
    }
    RenderFrameCaptureResult ReadLastCaptureResult() const noexcept override
    {
        return captureResult;
    }
    RenderResourceHandle ResolveRenderAsset(AssetId assetId,
                                            RenderResourceKind) const noexcept override
    {
        return assetsReady && assetId.value != 0
                   ? RenderResourceHandle{static_cast<uint32>(assetId.value), 1}
                   : RenderResourceHandle{};
    }
    RenderResizeResult RequestResize(const NativeSurfaceDesc& value) override
    {
        ++renderResizeRequestCount;
        lastRenderResizeSurface = value;
        return resizeResult;
    }
    bool RequestWindowResize(uint32 width, uint32 height) override
    {
        ++windowResizeRequestCount;
        if (!windowResizeAccepted)
        {
            return false;
        }
        surface.width = width;
        surface.height = height;
        ++surface.generation;
        return true;
    }
    void BeginResourceShutdown() override { shutdownOrder.emplace_back("resource"); }
    RenderShutdownResult StopRender() override
    {
        shutdownOrder.emplace_back("render");
        return stopResult;
    }
    void DrainTerminalRenderRequests() override { shutdownOrder.emplace_back("drain"); }
};

FrozenSceneObjectId Id(ECS::SceneRuntimeId runtime,
                       ECS::EntityHandle entity,
                       FrozenSceneObjectType type,
                       uint32 slot = 0)
{
    return MakeFrozenSceneObjectId(runtime, entity, type, slot);
}

FrozenSceneSnapshot MakeSnapshot(ECS::SceneRuntimeId runtime,
                                 uint64 revision,
                                 uint64 cutRevision,
                                 bool includeMesh = true)
{
    FrozenSceneSnapshot snapshot;
    snapshot.sceneRuntimeId = runtime;
    snapshot.revision = revision;
    snapshot.structuralJournalSequence = revision;
    snapshot.renderWorldTransformWriteVersion = revision;

    const ECS::EntityHandle cameraEntity = ECS::EntityHandle::Create(1, 1);
    FrozenSceneCamera camera;
    camera.id = Id(runtime, cameraEntity, FrozenSceneObjectType::Camera);
    camera.source.nearPlane = 0.1f;
    camera.source.farPlane = 100.0f;
    camera.source.aspectRatio = 1.0f;
    camera.source.normalizedViewport = {0.0f, 0.0f, 1.0f, 1.0f};
    camera.source.cutRevision = cutRevision;
    snapshot.cameras.push_back(camera);
    snapshot.selectedCamera = camera.id;

    if (includeMesh)
    {
        const ECS::EntityHandle meshEntity = ECS::EntityHandle::Create(11, 1);
        FrozenSceneMesh mesh;
        mesh.id = Id(runtime, meshEntity, FrozenSceneObjectType::Mesh);
        mesh.sourceEntity = meshEntity;
        mesh.source.meshAssetId = {.value = 101};
        mesh.source.submeshCount = 1;
        mesh.visibility.visible = true;
        mesh.visibilityWriteVersion = revision;
        mesh.bounds.extents = {1.0f, 1.0f, 1.0f};
        mesh.transformSourceRevision = revision;
        mesh.previousSimulationSourceRevision = revision;
        mesh.materialSlotsSource.count = 1;
        mesh.materialSlotsSource.values[0].materialAssetId = {.value = 201};
        mesh.materialSlots.push_back({
            .id = Id(runtime, meshEntity, FrozenSceneObjectType::MaterialSlot),
            .source = mesh.materialSlotsSource.values[0],
        });
        snapshot.meshes.push_back(std::move(mesh));
    }
    return snapshot;
}

std::unique_ptr<EcsRenderRuntimeComposition> MakeComposition(FakeServices*& outServices)
{
    auto services = std::make_unique<FakeServices>();
    outServices = services.get();
    RenderRuntimeConfig config;
    config.backendType = RHIBackendType::DX12;
    return std::make_unique<EcsRenderRuntimeComposition>(
        config, RenderFrameSettings{}, std::move(services));
}

void Prepare(EcsRenderRuntimeComposition& composition)
{
    ASSERT_TRUE(composition.PrepareBeforeSubsystemInitialization());
    composition.BeforeRenderSubsystemInitialize();
}
} // namespace

TEST(EcsRenderRuntimeCompositionValidation,
     PreparesBeforeWindowButCapturesOnlyAtTheRenderInitializationHook)
{
    FakeServices* services = nullptr;
    auto composition = MakeComposition(services);
    services->windowInitialized = false;
    EXPECT_TRUE(composition->PrepareBeforeSubsystemInitialization());
    EXPECT_EQ(services->injectedGatewayCount, 1U);
    EXPECT_EQ(services->configureCount, 0U);
    EXPECT_THROW(composition->BeforeRenderSubsystemInitialize(), std::runtime_error);

    services->windowInitialized = true;
    composition->BeforeRenderSubsystemInitialize();
    EXPECT_EQ(services->configureCount, 1U);
    EXPECT_EQ(composition->GetStats().surfaceGeneration, 1U);
    EXPECT_TRUE(composition->TryShutdown());
}

TEST(EcsRenderRuntimeCompositionValidation,
     ConfiguresOnlyEcsPipelineAndFailsClosedForUnavailableAssets)
{
    FakeServices* services = nullptr;
    auto composition = MakeComposition(services);
    Prepare(*composition);
    ASSERT_EQ(services->injectedGatewayCount, 1U);
    ASSERT_EQ(services->configureCount, 1U);

    const FrozenSceneSnapshot snapshot = MakeSnapshot(ECS::SceneRuntimeId{1001}, 1, 1);
    services->assetsReady = false;
    const auto missing = composition->Tick(&snapshot, 1.0f / 60.0f, 1.0f);
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->code, EcsRenderFramePipelineCode::ExtractionFailed);
    EXPECT_EQ(missing->extractionCode,
              EcsFrameExtractionResultCode::RequiredAssetUnresolved);
    EXPECT_EQ(services->transport.publishCount, 0U);

    services->assetsReady = true;
    services->transport.progress = {.appliedRenderSceneRevision = 1,
                                    .presentedFrameSequence = 1};
    const auto published = composition->Tick(&snapshot, 1.0f / 60.0f, 2.0f);
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published->code, EcsRenderFramePipelineCode::Published);
    EXPECT_EQ(services->transport.publishCount, 1U);
    EXPECT_EQ(composition->GetStats().publicationAccepted, 1U);
    EXPECT_TRUE(composition->TryShutdown());
}

TEST(EcsRenderRuntimeCompositionValidation,
     CameraWorldAndOneShotValuesProduceExactTemporalBoundaries)
{
    FakeServices* services = nullptr;
    auto composition = MakeComposition(services);
    Prepare(*composition);
    services->transport.progress = {.appliedRenderSceneRevision = 1,
                                    .presentedFrameSequence = 0};

    FrozenSceneSnapshot first = MakeSnapshot(ECS::SceneRuntimeId{1002}, 1, 1);
    ASSERT_EQ(composition->Tick(&first, 0.01f, 1.0f)->code,
              EcsRenderFramePipelineCode::Published);
    EXPECT_EQ(composition->GetStats().worldRevision, 1U);
    EXPECT_EQ(composition->GetStats().temporalEpoch, 2U);
    EXPECT_TRUE(services->transport.lastHeader.explicitDiscontinuity);
    services->completedProgress = services->transport.progress;

    const RenderFrameCaptureRequest capture{
        .requestId = 17,
        .kind = RenderFrameCaptureKind::Color,
        .width = 64,
        .height = 64,
    };
    ASSERT_TRUE(composition->QueueCapture(capture));
    services->transport.disposition =
        EcsRenderFrameTransportDisposition::SceneUpdateAcceptedWithoutFrame;
    services->transport.progress = {.appliedRenderSceneRevision = 2,
                                    .presentedFrameSequence = 1};
    FrozenSceneSnapshot cut = MakeSnapshot(ECS::SceneRuntimeId{1002}, 2, 2);
    cut.meshes.front().worldTransform[3].x += 1.0f;
    ASSERT_EQ(composition->Tick(&cut, 0.01f, 2.0f)->code,
              EcsRenderFramePipelineCode::SceneUpdatePublishedWithoutFrame);
    EXPECT_EQ(composition->GetStats().lastAcknowledgedCaptureRequestId, 0U);
    EXPECT_EQ(composition->GetStats().temporalEpoch, 3U);
    services->completedProgress = services->transport.progress;

    services->transport.disposition = EcsRenderFrameTransportDisposition::Accepted;
    services->transport.progress = {.appliedRenderSceneRevision = 3,
                                    .presentedFrameSequence = 3};
    FrozenSceneSnapshot accepted = MakeSnapshot(ECS::SceneRuntimeId{1002}, 3, 2);
    ASSERT_EQ(composition->Tick(&accepted, 0.01f, 3.0f)->code,
              EcsRenderFramePipelineCode::Published);
    EXPECT_EQ(composition->GetStats().lastAcknowledgedCaptureRequestId, 0U);
    services->completedProgress = services->transport.progress;
    services->captureResult = {
        .code = RenderFrameCaptureResultCode::Completed,
        .requestId = 17,
        .frameSequence = services->transport.lastHeader.sequence,
        .kind = RenderFrameCaptureKind::Color,
        .width = 64,
        .height = 64,
    };
    EXPECT_FALSE(composition->Tick(nullptr, 0.01f, 3.5f).has_value());
    EXPECT_EQ(composition->GetStats().lastAcknowledgedCaptureRequestId, 17U);

    ASSERT_TRUE(composition->RequestSurfaceResize(800, 600));
    services->surface.nativeWindow = 2;
    EXPECT_EQ(services->windowResizeRequestCount, 1U);
    EXPECT_EQ(composition->GetStats().surfaceGeneration, 1U);
    EXPECT_EQ(composition->GetStats().temporalEpoch, 3U);

    services->transport.progress = {.appliedRenderSceneRevision = 4,
                                    .presentedFrameSequence = 4};
    FrozenSceneSnapshot swapped = MakeSnapshot(ECS::SceneRuntimeId{1003}, 1, 2);
    ASSERT_EQ(composition->Tick(&swapped, 0.01f, 4.0f)->code,
              EcsRenderFramePipelineCode::Published);
    EXPECT_EQ(services->renderResizeRequestCount, 1U);
    EXPECT_EQ(services->lastRenderResizeSurface.generation, 2U);
    EXPECT_EQ(services->lastRenderResizeSurface.width, 800U);
    EXPECT_EQ(services->lastRenderResizeSurface.height, 600U);
    EXPECT_EQ(services->lastRenderResizeSurface.nativeWindow, 2U);
    EXPECT_EQ(composition->GetStats().surfaceGeneration, 2U);
    EXPECT_EQ(composition->GetStats().worldRevision, 2U);
    EXPECT_EQ(composition->GetStats().temporalEpoch, 5U);
    EXPECT_TRUE(composition->TryShutdown());
}

TEST(EcsRenderRuntimeCompositionValidation,
     CompletedProgressAndExactProofsGateOrderedShutdown)
{
    FakeServices* services = nullptr;
    auto composition = MakeComposition(services);
    Prepare(*composition);
    services->transport.progress = {.appliedRenderSceneRevision = 1,
                                    .presentedFrameSequence = 0};
    const ECS::SceneRuntimeId runtime{1004};
    FrozenSceneSnapshot first = MakeSnapshot(runtime, 1, 1);
    ASSERT_EQ(composition->Tick(&first, 0.01f, 1.0f)->code,
              EcsRenderFramePipelineCode::Published);
    services->completedProgress = services->transport.progress;

    IEcsSceneAssetRetirementProofGateway& proof =
        composition->GetPipeline().GetAssetRetirementProofGateway();
    const EcsSceneAssetRetirementBeginReceipt begin = proof.BeginRetirement({
        .sceneRuntimeId = runtime,
        .rootEntity = ECS::EntityHandle::Create(11, 1),
        .members = {ECS::EntityHandle::Create(11, 1)},
    });
    ASSERT_TRUE(begin.IsAccepted());

    services->transport.progress = {.appliedRenderSceneRevision = 2,
                                    .presentedFrameSequence = 0};
    FrozenSceneSnapshot removed = MakeSnapshot(runtime, 2, 1, false);
    ASSERT_EQ(composition->Tick(&removed, 0.01f, 2.0f)->code,
              EcsRenderFramePipelineCode::Published);
    EXPECT_EQ(proof.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::AppliedNotPresented);

    services->completedProgress = {.appliedRenderSceneRevision = 3,
                                   .presentedFrameSequence = 3};
    EXPECT_FALSE(composition->Tick(nullptr, 0.01f, 3.0f).has_value());
    EXPECT_EQ(composition->GetStats().completedProgress.presentedFrameSequence, 3U);
    services->completedProgress = {.appliedRenderSceneRevision = 2,
                                   .presentedFrameSequence = 2};
    EXPECT_FALSE(composition->Tick(nullptr, 0.01f, 4.0f).has_value());
    EXPECT_EQ(composition->GetStats().completedProgressRejected, 1U);

    composition->BeginShutdown();
    EXPECT_TRUE(composition->HasOutstandingProofs());
    EXPECT_FALSE(composition->CanStop());
    EXPECT_FALSE(composition->TryShutdown());
    EXPECT_TRUE(services->shutdownOrder.empty());

    services->completedProgress = {.appliedRenderSceneRevision = 2,
                                   .presentedFrameSequence = 2};
    EXPECT_FALSE(composition->Tick(nullptr, 0.01f, 2.5f).has_value());
    EXPECT_EQ(proof.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Presented);
    EXPECT_TRUE(composition->HasOutstandingProofs());

    ASSERT_TRUE(proof.AcknowledgeRetirementProof(begin.token));
    EXPECT_FALSE(composition->HasOutstandingProofs());
    EXPECT_TRUE(composition->CanStop());
    EXPECT_TRUE(composition->TryShutdown());
    EXPECT_EQ(services->shutdownOrder,
              (std::vector<std::string>{"resource", "render", "drain"}));
}

TEST(EcsRenderRuntimeCompositionValidation,
     CaptureAndTemporalResetAreResentUntilTheirLatestExactFramePresents)
{
    FakeServices* services = nullptr;
    auto composition = MakeComposition(services);
    Prepare(*composition);
    ASSERT_TRUE(composition->QueueCapture({
        .requestId = 22,
        .kind = RenderFrameCaptureKind::Color,
        .width = 64,
        .height = 64,
    }));

    const ECS::SceneRuntimeId runtime{1005};
    services->transport.progress = {.appliedRenderSceneRevision = 1,
                                    .presentedFrameSequence = 0};
    FrozenSceneSnapshot first = MakeSnapshot(runtime, 1, 1);
    ASSERT_EQ(composition->Tick(&first, 0.01f, 1.0f)->code,
              EcsRenderFramePipelineCode::Published);
    const uint64 replacedCaptureSequence = services->transport.lastHeader.sequence;
    EXPECT_TRUE(services->transport.lastTemporalReset);
    EXPECT_EQ(services->transport.lastCaptureRequest.requestId, 22U);

    services->completedProgress = services->transport.progress;
    services->transport.progress = {.appliedRenderSceneRevision = 2,
                                    .presentedFrameSequence = 0};
    FrozenSceneSnapshot replacement = MakeSnapshot(runtime, 2, 1);
    ASSERT_EQ(composition->Tick(&replacement, 0.01f, 2.0f)->code,
              EcsRenderFramePipelineCode::Published);
    const uint64 carryingCaptureSequence = services->transport.lastHeader.sequence;
    EXPECT_GT(carryingCaptureSequence, replacedCaptureSequence);
    EXPECT_TRUE(services->transport.lastTemporalReset);
    EXPECT_EQ(composition->GetStats().pendingCaptureFrameSequence,
              carryingCaptureSequence);

    services->completedProgress = {.appliedRenderSceneRevision = 2,
                                   .presentedFrameSequence = replacedCaptureSequence};
    services->captureResult = {
        .code = RenderFrameCaptureResultCode::Completed,
        .requestId = 22,
        .frameSequence = replacedCaptureSequence,
        .kind = RenderFrameCaptureKind::Color,
        .width = 64,
        .height = 64,
    };
    EXPECT_FALSE(composition->Tick(nullptr, 0.01f, 2.5f).has_value());
    EXPECT_EQ(composition->GetStats().lastAcknowledgedCaptureRequestId, 0U);
    EXPECT_NE(composition->GetStats().pendingTemporalFrameSequence, 0U);

    services->completedProgress = {.appliedRenderSceneRevision = 2,
                                   .presentedFrameSequence = carryingCaptureSequence};
    services->captureResult.frameSequence = carryingCaptureSequence;
    EXPECT_FALSE(composition->Tick(nullptr, 0.01f, 3.0f).has_value());
    EXPECT_EQ(composition->GetStats().lastAcknowledgedCaptureRequestId, 22U);
    EXPECT_EQ(composition->GetStats().pendingTemporalFrameSequence, 0U);

    services->transport.progress = {.appliedRenderSceneRevision = 3,
                                    .presentedFrameSequence = 3};
    FrozenSceneSnapshot afterPresentation = MakeSnapshot(runtime, 3, 1);
    ASSERT_EQ(composition->Tick(&afterPresentation, 0.01f, 4.0f)->code,
              EcsRenderFramePipelineCode::Published);
    EXPECT_FALSE(services->transport.lastTemporalReset);
    EXPECT_EQ(services->transport.lastCaptureRequest.requestId, 0U);
    EXPECT_TRUE(composition->TryShutdown());
}

TEST(EcsRenderRuntimeCompositionValidation,
     ReliableAcceptedEvidenceIgnoresRejectedAndSceneOnlyCandidates)
{
    FakeServices* services = nullptr;
    auto composition = MakeComposition(services);
    Prepare(*composition);

    const ECS::SceneRuntimeId firstRuntime{1006};
    services->transport.progress = {.appliedRenderSceneRevision = 1,
                                    .presentedFrameSequence = 1};
    FrozenSceneSnapshot first = MakeSnapshot(firstRuntime, 1, 7);
    ASSERT_EQ(composition->Tick(&first, 0.01f, 1.0f)->code,
              EcsRenderFramePipelineCode::Published);
    ASSERT_TRUE(composition->GetStats().lastAcceptedFrame.has_value());
    const EcsRenderAcceptedFrameEvidence firstEvidence =
        *composition->GetStats().lastAcceptedFrame;
    EXPECT_TRUE(firstEvidence.IsValid());
    EXPECT_EQ(firstEvidence.sceneRuntimeId, firstRuntime);
    EXPECT_EQ(firstEvidence.frozenSnapshotRevision, 1U);
    EXPECT_EQ(firstEvidence.targetRenderSceneRevision, 1U);
    EXPECT_EQ(firstEvidence.carryingFrameSequence, 1U);
    EXPECT_EQ(firstEvidence.selectedCamera, ECS::EntityHandle::Create(1, 1));
    EXPECT_EQ(firstEvidence.selectedCameraCutRevision, 7U);
    EXPECT_EQ(firstEvidence.bridge.primitiveCount, 1U);
    EXPECT_EQ(firstEvidence.bridge.materialBindingCount, 1U);
    EXPECT_EQ(firstEvidence.bridge.cameraCount, 1U);
    EXPECT_TRUE(firstEvidence.extraction.complete);
    EXPECT_EQ(firstEvidence.extraction.code, RenderExtractionCode::Complete);

    services->completedProgress = services->transport.progress;
    services->transport.disposition = EcsRenderFrameTransportDisposition::NotAccepted;
    services->transport.progress = {.appliedRenderSceneRevision = 1,
                                    .presentedFrameSequence = 1};
    FrozenSceneSnapshot rejected = MakeSnapshot(firstRuntime, 2, 8);
    ASSERT_EQ(composition->Tick(&rejected, 0.01f, 2.0f)->code,
              EcsRenderFramePipelineCode::TransportNotAccepted);
    ASSERT_TRUE(composition->GetStats().lastAcceptedFrame.has_value());
    EXPECT_EQ(composition->GetStats().lastAcceptedFrame->sceneRuntimeId,
              firstEvidence.sceneRuntimeId);
    EXPECT_EQ(composition->GetStats().lastAcceptedFrame->frozenSnapshotRevision,
              firstEvidence.frozenSnapshotRevision);
    EXPECT_EQ(composition->GetStats().lastAcceptedFrame->carryingFrameSequence,
              firstEvidence.carryingFrameSequence);

    services->transport.disposition =
        EcsRenderFrameTransportDisposition::SceneUpdateAcceptedWithoutFrame;
    services->transport.progress = {.appliedRenderSceneRevision = 2,
                                    .presentedFrameSequence = 1};
    FrozenSceneSnapshot sceneOnly = MakeSnapshot(firstRuntime, 3, 9);
    sceneOnly.meshes.front().worldTransform[3].x += 1.0f;
    ASSERT_EQ(composition->Tick(&sceneOnly, 0.01f, 3.0f)->code,
              EcsRenderFramePipelineCode::SceneUpdatePublishedWithoutFrame);
    ASSERT_TRUE(composition->GetStats().lastSceneUpdatePublication.has_value());
    EXPECT_EQ(composition->GetStats().lastSceneUpdatePublication->
                  frozenSourceSnapshotRevision,
              3U);
    EXPECT_EQ(composition->GetStats().lastAcceptedFrame->frozenSnapshotRevision,
              firstEvidence.frozenSnapshotRevision);

    services->completedProgress = services->transport.progress;
    services->transport.disposition = EcsRenderFrameTransportDisposition::Accepted;
    services->transport.progress = {.appliedRenderSceneRevision = 3,
                                    .presentedFrameSequence = 3};
    const ECS::SceneRuntimeId secondRuntime{1007};
    FrozenSceneSnapshot worldSwap = MakeSnapshot(secondRuntime, 1, 11);
    ASSERT_EQ(composition->Tick(&worldSwap, 0.01f, 4.0f)->code,
              EcsRenderFramePipelineCode::Published);
    ASSERT_TRUE(composition->GetStats().lastAcceptedFrame.has_value());
    EXPECT_EQ(composition->GetStats().lastAcceptedFrame->sceneRuntimeId,
              secondRuntime);
    EXPECT_EQ(composition->GetStats().lastAcceptedFrame->selectedCameraCutRevision,
              11U);
    EXPECT_EQ(composition->GetStats().lastSceneUpdatePublication->
                  frozenSourceSnapshotRevision,
              3U);

    services->completedProgress = services->transport.progress;
    services->transport.progress = {.appliedRenderSceneRevision = 4,
                                    .presentedFrameSequence = 4};
    FrozenSceneSnapshot cameraCut = MakeSnapshot(secondRuntime, 2, 12);
    ASSERT_EQ(composition->Tick(&cameraCut, 0.01f, 5.0f)->code,
              EcsRenderFramePipelineCode::Published);
    EXPECT_EQ(composition->GetStats().lastAcceptedFrame->sceneRuntimeId,
              secondRuntime);
    EXPECT_EQ(composition->GetStats().lastAcceptedFrame->frozenSnapshotRevision,
              2U);
    EXPECT_EQ(composition->GetStats().lastAcceptedFrame->selectedCameraCutRevision,
              12U);
    EXPECT_TRUE(composition->TryShutdown());
}

TEST(EcsRenderRuntimeCompositionValidation,
     ShutdownFailureRetainsRetryStateAndStillDrainsResourceRequests)
{
    FakeServices* services = nullptr;
    auto composition = MakeComposition(services);
    Prepare(*composition);

    services->stopResult.code = RenderShutdownCode::TimedOut;
    EXPECT_FALSE(composition->TryShutdown());
    EXPECT_FALSE(composition->GetStats().shutdownComplete);
    EXPECT_EQ(composition->GetStats().lastShutdownResult.code,
              RenderShutdownCode::TimedOut);
    EXPECT_EQ(composition->GetStats().shutdownStopAttempts, 1U);
    EXPECT_EQ(composition->GetStats().shutdownStopFailures, 1U);
    EXPECT_EQ(composition->GetStats().terminalDrainAttempts, 1U);
    EXPECT_EQ(services->shutdownOrder,
              (std::vector<std::string>{"resource", "render", "drain"}));

    services->stopResult.code = RenderShutdownCode::ExecutorJoinFailed;
    EXPECT_FALSE(composition->TryShutdown());
    EXPECT_EQ(composition->GetStats().lastShutdownResult.code,
              RenderShutdownCode::ExecutorJoinFailed);
    EXPECT_EQ(composition->GetStats().shutdownStopAttempts, 2U);
    EXPECT_EQ(composition->GetStats().shutdownStopFailures, 2U);

    services->stopResult.code = RenderShutdownCode::None;
    EXPECT_FALSE(composition->TryShutdown());
    EXPECT_EQ(composition->GetStats().lastShutdownResult.code,
              RenderShutdownCode::None);
    EXPECT_EQ(composition->GetStats().shutdownStopFailures, 3U);

    services->stopResult.code = RenderShutdownCode::Completed;
    EXPECT_TRUE(composition->TryShutdown());
    EXPECT_TRUE(composition->GetStats().shutdownComplete);
    EXPECT_EQ(composition->GetStats().shutdownStopAttempts, 4U);
    EXPECT_EQ(composition->GetStats().terminalDrainAttempts, 4U);
    EXPECT_EQ(services->shutdownOrder,
              (std::vector<std::string>{"resource", "render", "drain",
                                        "render", "drain", "render", "drain",
                                        "render", "drain"}));
}

TEST(EcsRenderRuntimeCompositionValidation,
     AlreadyStoppedAndDeviceLostAreTerminalShutdownOutcomes)
{
    {
        FakeServices* services = nullptr;
        auto composition = MakeComposition(services);
        Prepare(*composition);
        services->stopResult.code = RenderShutdownCode::AlreadyStopped;
        EXPECT_TRUE(composition->TryShutdown());
        EXPECT_TRUE(composition->GetStats().shutdownComplete);
    }
    {
        FakeServices* services = nullptr;
        auto composition = MakeComposition(services);
        Prepare(*composition);
        services->stopResult.code = RenderShutdownCode::DeviceLost;
        EXPECT_TRUE(composition->TryShutdown());
        EXPECT_TRUE(composition->GetStats().shutdownComplete);
    }
}
