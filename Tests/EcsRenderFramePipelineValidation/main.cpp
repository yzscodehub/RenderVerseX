#include "EcsRenderFramePipeline.h"

#include <gtest/gtest.h>

#include <unordered_map>

namespace
{
using namespace RVX;
using namespace RVX::SceneECS;
using namespace RVX::ResourceSceneAdapters;

struct AssetResolver
{
    std::unordered_map<uint64, RenderResourceHandle> handles;
    bool rejectAll = false;
};

[[nodiscard]] RenderResourceHandle ResolveAsset(
    void* context,
    AssetId assetId,
    RenderResourceKind) noexcept
{
    const auto* resolver = static_cast<const AssetResolver*>(context);
    if (resolver == nullptr || resolver->rejectAll)
    {
        return {};
    }
    const auto found = resolver->handles.find(assetId.value);
    return found != resolver->handles.end() ? found->second :
                                             RenderResourceHandle{};
}

class FakeTransport final : public IEcsRenderFrameTransport
{
public:
    EcsRenderFrameTransportDisposition nextDisposition =
        EcsRenderFrameTransportDisposition::Accepted;
    EcsSceneRenderPublicationProgress nextProgress;
    bool corruptReceipt = false;
    uint32 callCount = 0;
    bool lastCandidateHasSceneUpdate = false;
    bool lastSceneUpdateFullReset = false;
    uint64 lastSceneUpdateBaseRevision = 0;
    EcsRenderFrameTransportCandidate lastCandidate;

    [[nodiscard]] EcsRenderFrameTransportReceipt TryPublish(
        EcsRenderFrameTransportCandidate candidate) noexcept override
    {
        ++callCount;
        const bool valid = candidate.IsStructurallyValid();
        lastCandidateHasSceneUpdate = candidate.sceneUpdate != nullptr;
        lastSceneUpdateFullReset = lastCandidateHasSceneUpdate &&
                                  candidate.sceneUpdate->fullReset;
        lastSceneUpdateBaseRevision = lastCandidateHasSceneUpdate
                                          ? candidate.sceneUpdate->baseSceneRevision
                                          : 0;
        lastCandidate.candidateIdentity = candidate.candidateIdentity;
        lastCandidate.sourceSceneRuntimeId = candidate.sourceSceneRuntimeId;
        lastCandidate.sourceSnapshotRevision = candidate.sourceSnapshotRevision;
        lastCandidate.targetRenderSceneRevision = candidate.targetRenderSceneRevision;
        lastCandidate.frameSequence = candidate.frameSequence;

        EcsRenderFrameTransportReceipt receipt;
        receipt.disposition = valid ? nextDisposition :
                                      EcsRenderFrameTransportDisposition::NotAccepted;
        receipt.candidateIdentity = candidate.candidateIdentity;
        receipt.sourceSceneRuntimeId = candidate.sourceSceneRuntimeId;
        receipt.sourceSnapshotRevision = candidate.sourceSnapshotRevision;
        receipt.targetRenderSceneRevision = candidate.targetRenderSceneRevision;
        receipt.frameSequence = candidate.frameSequence;
        receipt.completedProgress = nextProgress;
        if (corruptReceipt)
        {
            ++receipt.frameSequence;
        }
        return receipt;
    }
};

[[nodiscard]] Mat4 Translation(float32 x)
{
    Mat4 matrix{1.0f};
    matrix[3].x = x;
    return matrix;
}

[[nodiscard]] FrozenSceneObjectId Id(
    ECS::SceneRuntimeId runtime,
    ECS::EntityHandle entity,
    FrozenSceneObjectType type,
    uint32 slot = 0)
{
    return MakeFrozenSceneObjectId(runtime, entity, type, slot);
}

[[nodiscard]] FrozenSceneSnapshot MakeSnapshot(
    ECS::SceneRuntimeId runtime,
    uint64 revision,
    uint64 visibilityVersion,
    bool includeMesh = true,
    bool includeSkybox = false,
    uint64 skyboxWriteVersion = 0)
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
    camera.worldTransform = Translation(0.0f);
    snapshot.cameras.push_back(camera);
    snapshot.selectedCamera = camera.id;

    if (includeMesh)
    {
        const ECS::EntityHandle meshEntity = ECS::EntityHandle::Create(11, 3);
        FrozenSceneMesh mesh;
        mesh.id = Id(runtime, meshEntity, FrozenSceneObjectType::Mesh);
        mesh.sourceEntity = meshEntity;
        mesh.source.meshAssetId = {.value = 101};
        mesh.source.submeshCount = 1;
        mesh.visibility.visible = true;
        mesh.visibilityWriteVersion = visibilityVersion;
        mesh.bounds.extents = {1.0f, 1.0f, 1.0f};
        mesh.worldTransform = Translation(4.0f);
        mesh.previousSimulationWorldTransform = Translation(3.0f);
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
    if (includeSkybox)
    {
        const ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Create(31, 3);
        FrozenSceneSkybox skybox;
        skybox.id = Id(runtime, skyboxEntity, FrozenSceneObjectType::Skybox);
        skybox.sourceEntity = skyboxEntity;
        skybox.source.mode = SkyboxMode::SolidColor;
        skybox.source.contributesToLighting = false;
        skybox.skyboxWriteVersion = skyboxWriteVersion;
        snapshot.skyboxes.push_back(std::move(skybox));
    }
    return snapshot;
}

[[nodiscard]] EcsFrameExtractionInput Input(AssetResolver& resolver,
                                             uint64 sequence)
{
    EcsFrameExtractionInput input;
    input.sequence = sequence;
    input.outputWidth = 640;
    input.outputHeight = 480;
    input.deltaTime = 1.0f / 60.0f;
    input.assetResolver = {.context = &resolver, .resolve = &ResolveAsset};
    return input;
}

[[nodiscard]] std::vector<EcsSceneAssetRenderableVisibilityVersion> Versions(
    ECS::EntityHandle entity,
    uint64 version)
{
    return {{.entity = entity, .entityVisibilityWriteVersion = version}};
}

[[nodiscard]] EcsSceneAssetRetirementRequest RetirementRequest(
    ECS::SceneRuntimeId runtime,
    ECS::EntityHandle member)
{
    return {.sceneRuntimeId = runtime, .rootEntity = member, .members = {member}};
}
} // namespace

TEST(EcsRenderFramePipelineValidation,
     AcceptedPublicationCarriesExactSnapshotReceiptAndMinimumResidentProof)
{
    AssetResolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    FakeTransport transport;
    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 1};
    EcsRenderFramePipeline pipeline(transport);
    const ECS::SceneRuntimeId runtime{1001};
    const ECS::EntityHandle mesh = ECS::EntityHandle::Create(11, 3);

    const EcsRenderFramePipelineResult result = pipeline.Publish(
        MakeSnapshot(runtime, 1, 17), Input(resolver, 1));
    ASSERT_EQ(result.code, EcsRenderFramePipelineCode::Published);
    ASSERT_TRUE(result.completedExtractionDiagnostics.has_value());
    EXPECT_TRUE(result.completedExtractionDiagnostics->complete);
    EXPECT_EQ(result.completedExtractionDiagnostics->code,
              RenderExtractionCode::Complete);
    EXPECT_EQ(result.completedExtractionDiagnostics->fullScanCount, 1U);
    ASSERT_TRUE(result.publication.has_value());
    EXPECT_TRUE(result.publication->IsValid());
    EXPECT_EQ(result.publication->sourceSceneRuntimeId, runtime);
    EXPECT_EQ(result.publication->frozenSourceSnapshotRevision, 1U);
    EXPECT_EQ(result.publication->targetRenderSceneRevision, 1U);
    EXPECT_EQ(result.publication->carryingFrameSequence, 1U);
    EXPECT_EQ(transport.callCount, 1U);
    EXPECT_EQ(transport.lastCandidate.sourceSceneRuntimeId, runtime);
    EXPECT_EQ(transport.lastCandidate.sourceSnapshotRevision, 1U);

    const auto receipt = pipeline.BuildMinimumResidentPresentationReceipt(
        runtime, mesh, Versions(mesh, 17));
    ASSERT_TRUE(receipt.has_value());
    EXPECT_EQ(receipt->frozenSourceSnapshotRevision, 1U);
    EXPECT_EQ(receipt->renderSceneRevision, 1U);
    EXPECT_EQ(receipt->carryingPresentedFrameSequence, 1U);
}

TEST(EcsRenderFramePipelineValidation,
     StaticSnapshotPublishesQualifiedFrameOnlyCandidateAndPreservesIncrementalBaseline)
{
    AssetResolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    FakeTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    const ECS::SceneRuntimeId runtime{1007};
    const ECS::EntityHandle mesh = ECS::EntityHandle::Create(11, 3);

    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 1};
    ASSERT_EQ(pipeline.Publish(MakeSnapshot(runtime, 1, 17), Input(resolver, 1)).code,
              EcsRenderFramePipelineCode::Published);
    EXPECT_TRUE(transport.lastCandidateHasSceneUpdate);

    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 2};
    const EcsRenderFramePipelineResult frameOnly =
        pipeline.Publish(MakeSnapshot(runtime, 2, 17), Input(resolver, 2));
    ASSERT_EQ(frameOnly.code, EcsRenderFramePipelineCode::Published);
    ASSERT_TRUE(frameOnly.publication.has_value());
    EXPECT_FALSE(transport.lastCandidateHasSceneUpdate);
    EXPECT_EQ(transport.lastCandidate.sourceSnapshotRevision, 2U);
    EXPECT_EQ(transport.lastCandidate.targetRenderSceneRevision, 1U);
    EXPECT_EQ(frameOnly.publication->targetRenderSceneRevision, 1U);
    EXPECT_EQ(pipeline.GetExtractor().GetAcceptedSceneRevision(), 1U);
    EXPECT_EQ(pipeline.GetExtractor().GetAcceptedFrameSequence(), 2U);
    const auto staticReceipt = pipeline.BuildMinimumResidentPresentationReceipt(
        runtime, mesh, Versions(mesh, 17));
    ASSERT_TRUE(staticReceipt.has_value());
    EXPECT_EQ(staticReceipt->frozenSourceSnapshotRevision, 2U);
    EXPECT_EQ(staticReceipt->renderSceneRevision, 1U);
    EXPECT_EQ(staticReceipt->carryingPresentedFrameSequence, 2U);

    FrozenSceneSnapshot mutation = MakeSnapshot(runtime, 3, 17);
    mutation.meshes.front().worldTransform = Translation(9.0f);
    transport.nextProgress = {.appliedRenderSceneRevision = 2,
                              .presentedFrameSequence = 3};
    const EcsRenderFramePipelineResult incremental =
        pipeline.Publish(mutation, Input(resolver, 3));
    ASSERT_EQ(incremental.code, EcsRenderFramePipelineCode::Published);
    EXPECT_TRUE(transport.lastCandidateHasSceneUpdate);
    EXPECT_FALSE(transport.lastSceneUpdateFullReset);
    EXPECT_EQ(transport.lastSceneUpdateBaseRevision, 1U);
    EXPECT_EQ(transport.lastCandidate.targetRenderSceneRevision, 2U);
}

TEST(EcsRenderFramePipelineValidation,
     FrameOnlyCandidateRejectsImpossibleSceneOnlyDisposition)
{
    AssetResolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    FakeTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    const ECS::SceneRuntimeId runtime{1008};

    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 1};
    ASSERT_EQ(pipeline.Publish(MakeSnapshot(runtime, 1, 17), Input(resolver, 1)).code,
              EcsRenderFramePipelineCode::Published);

    transport.nextDisposition =
        EcsRenderFrameTransportDisposition::SceneUpdateAcceptedWithoutFrame;
    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 1};
    const EcsRenderFramePipelineResult rejected =
        pipeline.Publish(MakeSnapshot(runtime, 2, 17), Input(resolver, 2));
    EXPECT_EQ(rejected.code, EcsRenderFramePipelineCode::TransportNotAccepted);
    ASSERT_TRUE(rejected.publication.has_value());
    EXPECT_EQ(rejected.publication->disposition,
              EcsFramePublicationDisposition::NotAccepted);
    EXPECT_FALSE(transport.lastCandidateHasSceneUpdate);
    EXPECT_EQ(pipeline.GetExtractor().GetAcceptedSceneRevision(), 1U);
    EXPECT_EQ(pipeline.GetExtractor().GetAcceptedFrameSequence(), 1U);
}

TEST(EcsRenderFramePipelineValidation,
     ForwardsOnlyExactPresentedEnvironmentProof)
{
    AssetResolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    FakeTransport transport;
    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 0};
    EcsRenderFramePipeline pipeline(transport);
    const ECS::SceneRuntimeId runtime{1011};
    const ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Create(31, 3);

    ASSERT_EQ(pipeline.Publish(
                  MakeSnapshot(runtime, 1, 0, false, true, 91), Input(resolver, 1))
                  .code,
              EcsRenderFramePipelineCode::Published);
    EXPECT_FALSE(pipeline.BuildEnvironmentPresentationReceipt(
                     runtime, skyboxEntity, 91)
                     .has_value());

    ASSERT_TRUE(pipeline.ObserveCompletedProgress(
        {.appliedRenderSceneRevision = 1, .presentedFrameSequence = 1}));
    const auto receipt = pipeline.BuildEnvironmentPresentationReceipt(
        runtime, skyboxEntity, 91);
    ASSERT_TRUE(receipt.has_value());
    EXPECT_EQ(receipt->frozenSourceSnapshotRevision, 1U);
    EXPECT_EQ(receipt->renderSceneRevision, 1U);
    EXPECT_EQ(receipt->carryingPresentedFrameSequence, 1U);
    EXPECT_FALSE(pipeline.BuildEnvironmentPresentationReceipt(
                     runtime, skyboxEntity, 92)
                     .has_value());
}

TEST(EcsRenderFramePipelineValidation,
     SceneOnlyAndNotAcceptedOutcomesResolveWithoutPendingCandidates)
{
    AssetResolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    FakeTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    const ECS::SceneRuntimeId runtime{1002};

    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 1};
    ASSERT_EQ(pipeline.Publish(MakeSnapshot(runtime, 1, 1), Input(resolver, 1)).code,
              EcsRenderFramePipelineCode::Published);

    transport.nextDisposition =
        EcsRenderFrameTransportDisposition::SceneUpdateAcceptedWithoutFrame;
    transport.nextProgress = {.appliedRenderSceneRevision = 2,
                              .presentedFrameSequence = 1};
    FrozenSceneSnapshot sceneOnlySnapshot = MakeSnapshot(runtime, 2, 2);
    sceneOnlySnapshot.meshes.front().worldTransform = Translation(5.0f);
    const EcsRenderFramePipelineResult sceneOnly = pipeline.Publish(
        sceneOnlySnapshot, Input(resolver, 2));
    ASSERT_EQ(sceneOnly.code,
              EcsRenderFramePipelineCode::SceneUpdatePublishedWithoutFrame);
    ASSERT_TRUE(sceneOnly.publication.has_value());
    EXPECT_EQ(sceneOnly.publication->disposition,
              EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame);
    EXPECT_EQ(pipeline.GetExtractor().GetAcceptedSceneRevision(), 2U);
    EXPECT_EQ(pipeline.GetExtractor().GetAcceptedFrameSequence(), 1U);

    transport.nextDisposition = EcsRenderFrameTransportDisposition::NotAccepted;
    transport.nextProgress = {.appliedRenderSceneRevision = 2,
                              .presentedFrameSequence = 1};
    ASSERT_EQ(pipeline.Publish(MakeSnapshot(runtime, 3, 3), Input(resolver, 3)).code,
              EcsRenderFramePipelineCode::TransportNotAccepted);
    EXPECT_EQ(pipeline.GetExtractor().GetAcceptedSceneRevision(), 2U);

    // A following candidate proves both the extractor and exact gateway have
    // no unresolved candidate after each terminal transport disposition.
    transport.nextDisposition = EcsRenderFrameTransportDisposition::Accepted;
    transport.nextProgress = {.appliedRenderSceneRevision = 3,
                              .presentedFrameSequence = 4};
    EXPECT_EQ(pipeline.Publish(MakeSnapshot(runtime, 4, 4), Input(resolver, 4)).code,
              EcsRenderFramePipelineCode::Published);
}

TEST(EcsRenderFramePipelineValidation,
     AssetResolutionFailureNeverTransfersOrLeavesPublicationState)
{
    AssetResolver resolver{{{101, {1, 1}}, {201, {2, 1}}}, true};
    FakeTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    const ECS::SceneRuntimeId runtime{1003};

    const EcsRenderFramePipelineResult failed = pipeline.Publish(
        MakeSnapshot(runtime, 1, 1), Input(resolver, 1));
    EXPECT_EQ(failed.code, EcsRenderFramePipelineCode::ExtractionFailed);
    EXPECT_EQ(failed.extractionCode,
              EcsFrameExtractionResultCode::RequiredAssetUnresolved);
    EXPECT_EQ(transport.callCount, 0U);

    resolver.rejectAll = false;
    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 1};
    EXPECT_EQ(pipeline.Publish(MakeSnapshot(runtime, 1, 1), Input(resolver, 1)).code,
              EcsRenderFramePipelineCode::Published);
}

TEST(EcsRenderFramePipelineValidation,
     RegressingProgressFailsClosedAndFutureProgressCanAdvanceProofs)
{
    AssetResolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    FakeTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    const ECS::SceneRuntimeId runtime{1004};

    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 1};
    ASSERT_EQ(pipeline.Publish(MakeSnapshot(runtime, 1, 1), Input(resolver, 1)).code,
              EcsRenderFramePipelineCode::Published);

    transport.nextProgress = {.appliedRenderSceneRevision = 0,
                              .presentedFrameSequence = 0};
    const EcsRenderFramePipelineResult regressed = pipeline.Publish(
        MakeSnapshot(runtime, 2, 2), Input(resolver, 2));
    ASSERT_EQ(regressed.code,
              EcsRenderFramePipelineCode::NonMonotonicTransportProgress);
    ASSERT_TRUE(regressed.publication.has_value());
    EXPECT_EQ(regressed.publication->disposition,
              EcsFramePublicationDisposition::NotAccepted);
    EXPECT_FALSE(regressed.publication->transportProgressWasMonotonic);
    EXPECT_EQ(pipeline.GetCompletedProgress().presentedFrameSequence, 1U);

    EXPECT_FALSE(pipeline.ObserveCompletedProgress(
        {.appliedRenderSceneRevision = 0, .presentedFrameSequence = 0}));
    EXPECT_TRUE(pipeline.ObserveCompletedProgress(
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 2}));

    transport.nextProgress = {.appliedRenderSceneRevision = 3,
                              .presentedFrameSequence = 3};
    EXPECT_EQ(pipeline.Publish(MakeSnapshot(runtime, 3, 3), Input(resolver, 3)).code,
              EcsRenderFramePipelineCode::Published);
}

TEST(EcsRenderFramePipelineValidation,
     RetirementProofIsReleasedOnlyAfterExactRemovalCandidateIsPresented)
{
    AssetResolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    FakeTransport transport;
    EcsRenderFramePipeline pipeline(transport);
    const ECS::SceneRuntimeId runtime{1005};
    const ECS::EntityHandle mesh = ECS::EntityHandle::Create(11, 3);

    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 1};
    ASSERT_EQ(pipeline.Publish(MakeSnapshot(runtime, 1, 1), Input(resolver, 1)).code,
              EcsRenderFramePipelineCode::Published);
    IEcsSceneAssetRetirementProofGateway& gateway =
        pipeline.GetAssetRetirementProofGateway();
    const EcsSceneAssetRetirementBeginReceipt begin = gateway.BeginRetirement(
        RetirementRequest(runtime, mesh));
    ASSERT_TRUE(begin.IsAccepted());
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Pending);

    transport.nextProgress = {.appliedRenderSceneRevision = 2,
                              .presentedFrameSequence = 2};
    ASSERT_EQ(pipeline.Publish(MakeSnapshot(runtime, 2, 0, false), Input(resolver, 2)).code,
              EcsRenderFramePipelineCode::Published);
    const EcsSceneAssetRetirementProof proof =
        gateway.QueryRetirementProof(begin.token);
    EXPECT_EQ(proof.state, EcsSceneAssetRetirementProofState::Presented);
    EXPECT_EQ(proof.appliedRenderSceneRevision, 2U);
    EXPECT_EQ(proof.presentedFrameSequence, 2U);
    EXPECT_TRUE(gateway.AcknowledgeRetirementProof(begin.token));
}

TEST(EcsRenderFramePipelineValidation,
     ReceiptMismatchDropsCandidateWithoutAProofLeak)
{
    AssetResolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    FakeTransport transport;
    transport.corruptReceipt = true;
    transport.nextProgress = {.appliedRenderSceneRevision = 1,
                              .presentedFrameSequence = 1};
    EcsRenderFramePipeline pipeline(transport);
    const ECS::SceneRuntimeId runtime{1006};

    EXPECT_EQ(pipeline.Publish(MakeSnapshot(runtime, 1, 1), Input(resolver, 1)).code,
              EcsRenderFramePipelineCode::TransportReceiptMismatch);
    transport.corruptReceipt = false;
    EXPECT_EQ(pipeline.Publish(MakeSnapshot(runtime, 2, 2), Input(resolver, 2)).code,
              EcsRenderFramePipelineCode::Published);
}
