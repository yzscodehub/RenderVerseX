#include "EcsSceneRenderProofGateway.h"

#include "RenderExtraction/EcsFrozenSceneBridge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>

namespace
{
using namespace RVX;
using namespace RVX::SceneECS;
using namespace RVX::ResourceSceneAdapters;

struct Resolver
{
    std::unordered_map<uint64, RenderResourceHandle> handles;
};

constexpr uint8 FEATURE_PARTICLE = 1U << 0U;
constexpr uint8 FEATURE_WATER = 1U << 1U;
constexpr uint8 FEATURE_TERRAIN = 1U << 2U;

[[nodiscard]] RenderResourceHandle ResolveAsset(
    void* context, AssetId id, RenderResourceKind) noexcept
{
    const auto* resolver = static_cast<const Resolver*>(context);
    if (resolver == nullptr)
    {
        return {};
    }
    const auto found = resolver->handles.find(id.value);
    return found != resolver->handles.end() ? found->second : RenderResourceHandle{};
}

[[nodiscard]] Mat4 Translation(float32 x)
{
    Mat4 matrix{1.0f};
    matrix[3].x = x;
    return matrix;
}

[[nodiscard]] FrozenSceneObjectId Id(ECS::SceneRuntimeId runtime,
                                     ECS::EntityHandle entity,
                                     FrozenSceneObjectType type,
                                     uint32 slot = 0)
{
    return MakeFrozenSceneObjectId(runtime, entity, type, slot);
}

[[nodiscard]] EcsFrozenSceneBridgeOutput MakeOutput(
    ECS::SceneRuntimeId runtime,
    uint64 snapshotRevision,
    uint64 visibilityVersion,
    bool includeMesh = true,
    uint32 meshEntityIndex = 11,
    uint8 featureMask = 0,
    uint32 featureEntityIndex = 51,
    uint32 featureEntityGeneration = 4,
    bool includeSkybox = false,
    uint64 skyboxWriteVersion = 0,
    uint32 skyboxEntityIndex = 31,
    uint32 skyboxEntityGeneration = 3)
{
    FrozenSceneSnapshot snapshot;
    snapshot.sceneRuntimeId = runtime;
    snapshot.revision = snapshotRevision;
    snapshot.structuralJournalSequence = snapshotRevision;
    snapshot.renderWorldTransformWriteVersion = snapshotRevision;

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
        const ECS::EntityHandle meshEntity =
            ECS::EntityHandle::Create(meshEntityIndex, 3);
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
        mesh.transformSourceRevision = snapshotRevision;
        mesh.previousSimulationSourceRevision = snapshotRevision;
        mesh.materialSlotsSource.count = 1;
        mesh.materialSlotsSource.values[0].materialAssetId = {.value = 201};
        mesh.materialSlots.push_back({
            .id = Id(runtime, meshEntity, FrozenSceneObjectType::MaterialSlot),
            .source = mesh.materialSlotsSource.values[0],
        });
        snapshot.meshes.push_back(std::move(mesh));
    }

    const ECS::EntityHandle featureEntity = ECS::EntityHandle::Create(
        featureEntityIndex, featureEntityGeneration);
    if ((featureMask & FEATURE_PARTICLE) != 0)
    {
        FrozenSceneParticle particle;
        particle.id = Id(runtime, featureEntity, FrozenSceneObjectType::Particle);
        particle.sourceEntity = featureEntity;
        particle.payloadRevision = snapshotRevision;
        snapshot.particles.push_back(std::move(particle));
    }
    if ((featureMask & FEATURE_WATER) != 0)
    {
        FrozenSceneWater water;
        water.id = Id(runtime, featureEntity, FrozenSceneObjectType::Water);
        water.sourceEntity = featureEntity;
        water.payloadRevision = snapshotRevision;
        snapshot.water.push_back(std::move(water));
    }
    if ((featureMask & FEATURE_TERRAIN) != 0)
    {
        FrozenSceneTerrain terrain;
        terrain.id = Id(runtime, featureEntity, FrozenSceneObjectType::Terrain);
        terrain.sourceEntity = featureEntity;
        terrain.payloadRevision = snapshotRevision;
        snapshot.terrain.push_back(std::move(terrain));
    }
    if (includeSkybox)
    {
        const ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Create(
            skyboxEntityIndex, skyboxEntityGeneration);
        FrozenSceneSkybox skybox;
        skybox.id = Id(runtime, skyboxEntity, FrozenSceneObjectType::Skybox);
        skybox.sourceEntity = skyboxEntity;
        skybox.source.mode = SkyboxMode::SolidColor;
        skybox.source.contributesToLighting = false;
        skybox.skyboxWriteVersion = skyboxWriteVersion;
        snapshot.skyboxes.push_back(std::move(skybox));
    }

    EcsFrozenSceneBridgeOutput output;
    EXPECT_TRUE(EcsFrozenSceneBridge{}.Build(snapshot, output));
    return output;
}

[[nodiscard]] EcsFrameExtractionInput Input(Resolver& resolver, uint64 sequence)
{
    EcsFrameExtractionInput input;
    input.sequence = sequence;
    input.outputWidth = 640;
    input.outputHeight = 480;
    input.deltaTime = 1.0f / 60.0f;
    input.assetResolver = {.context = &resolver, .resolve = &ResolveAsset};
    return input;
}

[[nodiscard]] EcsRenderSceneRetirementResolution ExtractAndResolve(
    EcsFrameExtractor& extractor,
    EcsSceneRenderProofGateway& gateway,
    Resolver& resolver,
    const EcsFrozenSceneBridgeOutput& output,
    uint64 sequence,
    EcsFramePublicationDisposition disposition,
    EcsSceneRenderPublicationProgress progress)
{
    EcsFrameExtractionResult extraction = extractor.Extract(Input(resolver, sequence), output);
    EXPECT_TRUE(extraction.IsComplete());
    EXPECT_TRUE(gateway.ObserveExtractionCandidate(output, extraction));
    const EcsRenderSceneRetirementResolution resolution =
        extractor.ResolveLastPublication(disposition);
    EXPECT_TRUE(gateway.ResolveObservedCandidate(disposition, resolution, progress));
    return resolution;
}

[[nodiscard]] EcsSceneAssetRetirementRequest RetirementRequest(
    ECS::SceneRuntimeId runtime,
    ECS::EntityHandle member)
{
    return {.sceneRuntimeId = runtime, .rootEntity = member, .members = {member}};
}

[[nodiscard]] std::vector<EcsSceneAssetRenderableVisibilityVersion> Versions(
    ECS::EntityHandle entity,
    uint64 version)
{
    return {{.entity = entity, .entityVisibilityWriteVersion = version}};
}
} // namespace

TEST(EcsSceneRenderProofGatewayValidation,
     MinimumResidentReceiptRequiresExactVisibleVersionAndCompletedPresentation)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{401};
    const ECS::EntityHandle meshEntity = ECS::EntityHandle::Create(11, 3);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);

    const EcsFrozenSceneBridgeOutput first = MakeOutput(runtime, 1, 77);
    const auto firstVersions = Versions(meshEntity, 77);
    ASSERT_FALSE(gateway.BuildMinimumResidentPresentationReceipt(
                     runtime, meshEntity, firstVersions)
                     .has_value());
    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        first,
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));

    const auto receipt = gateway.BuildMinimumResidentPresentationReceipt(
        runtime, meshEntity, firstVersions);
    ASSERT_TRUE(receipt.has_value());
    EXPECT_EQ(receipt->frozenSourceSnapshotRevision, 1U);
    EXPECT_EQ(receipt->renderSceneRevision, 1U);
    EXPECT_EQ(receipt->carryingPresentedFrameSequence, 1U);
    const auto mismatchedVersions = Versions(meshEntity, 78);
    EXPECT_FALSE(gateway.BuildMinimumResidentPresentationReceipt(
                     runtime, meshEntity, mismatchedVersions)
                     .has_value());

    EcsFrozenSceneBridgeOutput second = MakeOutput(runtime, 2, 88);
    second.renderProxies.primitives.front().worldMatrix[3].x += 1.0f;
    static_cast<void>(ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        second,
        2,
        EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 2}));
    const auto sceneOnlyVersions = Versions(meshEntity, 88);
    EXPECT_FALSE(gateway.BuildMinimumResidentPresentationReceipt(
                     runtime, meshEntity, sceneOnlyVersions)
                     .has_value());
}

TEST(EcsSceneRenderProofGatewayValidation,
     ObserveAndResolveRejectForeignStaleAndFabricatedCandidateProofs)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{406};
    EcsFrameExtractor extractor;
    EcsFrameExtractor foreignExtractor;
    EcsSceneRenderProofGateway gateway(extractor);
    const EcsFrozenSceneBridgeOutput source = MakeOutput(runtime, 1, 7);

    EcsFrameExtractionResult candidate =
        extractor.Extract(Input(resolver, 1), source);
    EcsFrameExtractionResult foreign =
        foreignExtractor.Extract(Input(resolver, 1), source);
    ASSERT_TRUE(candidate.IsComplete());
    ASSERT_TRUE(foreign.IsComplete());
    EXPECT_NE(candidate.candidateIdentity, foreign.candidateIdentity);

    EcsFrozenSceneBridgeOutput staleSource = source;
    ++staleSource.snapshotRevision;
    ++staleSource.renderProxies.metadata.sequence;
    EXPECT_FALSE(gateway.ObserveExtractionCandidate(staleSource, candidate));
    EXPECT_FALSE(gateway.ObserveExtractionCandidate(source, foreign));
    EXPECT_TRUE(gateway.ObserveExtractionCandidate(source, candidate));

    EcsRenderSceneRetirementResolution unresolvedFabrication;
    unresolvedFabrication.candidateIdentity = candidate.candidateIdentity;
    unresolvedFabrication.disposition = EcsFramePublicationDisposition::Accepted;
    unresolvedFabrication.resolvedCandidate = true;
    EXPECT_FALSE(gateway.ResolveObservedCandidate(
        EcsFramePublicationDisposition::Accepted,
        unresolvedFabrication,
        {.appliedRenderSceneRevision = 1, .presentedFrameSequence = 1}));

    const EcsRenderSceneRetirementResolution accepted =
        extractor.ResolveLastPublication(EcsFramePublicationDisposition::Accepted);
    EcsRenderSceneRetirementResolution fabricated = accepted;
    fabricated.candidateIdentity = foreign.candidateIdentity;
    EXPECT_FALSE(gateway.ResolveObservedCandidate(
        EcsFramePublicationDisposition::Accepted,
        fabricated,
        {.appliedRenderSceneRevision = 1, .presentedFrameSequence = 1}));
    EXPECT_TRUE(gateway.ResolveObservedCandidate(
        EcsFramePublicationDisposition::Accepted,
        accepted,
        {.appliedRenderSceneRevision = 1, .presentedFrameSequence = 1}));

    const EcsFrozenSceneBridgeOutput rejectedSource = MakeOutput(runtime, 2, 8);
    EcsFrameExtractionResult rejected =
        extractor.Extract(Input(resolver, 2), rejectedSource);
    ASSERT_TRUE(rejected.IsComplete());
    ASSERT_TRUE(gateway.ObserveExtractionCandidate(rejectedSource, rejected));
    const EcsRenderSceneRetirementResolution notAccepted =
        extractor.ResolveLastPublication(EcsFramePublicationDisposition::NotAccepted);
    EcsRenderSceneRetirementResolution mutatedDisposition = notAccepted;
    mutatedDisposition.disposition = EcsFramePublicationDisposition::Accepted;
    EXPECT_FALSE(gateway.ResolveObservedCandidate(
        EcsFramePublicationDisposition::Accepted,
        notAccepted,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 2}));
    EXPECT_FALSE(gateway.ResolveObservedCandidate(
        EcsFramePublicationDisposition::Accepted,
        mutatedDisposition,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 2}));
    EXPECT_TRUE(gateway.ResolveObservedCandidate(
        EcsFramePublicationDisposition::NotAccepted,
        notAccepted,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 2}));
}

TEST(EcsSceneRenderProofGatewayValidation,
     RetirementWaitsForCompletedPresentationAndAcknowledgementInvalidatesToken)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{411};
    const ECS::EntityHandle meshEntity = ECS::EntityHandle::Create(11, 3);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);
    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 1, 7),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));

    const EcsSceneAssetRetirementBeginReceipt begin =
        gateway.BeginRetirement(RetirementRequest(runtime, meshEntity));
    ASSERT_TRUE(begin.IsAccepted());
    const EcsRenderSceneRetirementResolution resolution = ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(runtime, 2, 0, false),
        2,
        EcsFramePublicationDisposition::Accepted,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 1});
    ASSERT_EQ(resolution.bindingCount, 1U);

    EcsSceneAssetRetirementProof proof = gateway.QueryRetirementProof(begin.token);
    EXPECT_EQ(proof.state, EcsSceneAssetRetirementProofState::AppliedNotPresented);
    EXPECT_EQ(proof.appliedRenderSceneRevision, 2U);
    EXPECT_EQ(proof.presentedFrameSequence, 2U);
    ASSERT_TRUE(gateway.ObservePublicationProgress(
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 2}));
    proof = gateway.QueryRetirementProof(begin.token);
    EXPECT_EQ(proof.state, EcsSceneAssetRetirementProofState::Presented);
    EXPECT_EQ(proof.request.sceneRuntimeId, runtime);
    EXPECT_EQ(proof.request.rootEntity, meshEntity);
    ASSERT_EQ(proof.request.members.size(), 1U);
    EXPECT_EQ(proof.request.members.front(), meshEntity);
    EXPECT_TRUE(gateway.AcknowledgeRetirementProof(begin.token));
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Failed);
}

TEST(EcsSceneRenderProofGatewayValidation,
     NeverPublishedAndSceneOnlyRemovalAreExplicitlySafeWithoutFabricatedFrames)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{421};
    const ECS::EntityHandle meshEntity = ECS::EntityHandle::Create(11, 3);
    const ECS::EntityHandle absent = ECS::EntityHandle::Create(99, 5);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);
    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 1, 7),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));

    const EcsSceneAssetRetirementBeginReceipt never =
        gateway.BeginRetirement(RetirementRequest(runtime, absent));
    ASSERT_TRUE(never.IsAccepted());
    const EcsSceneAssetRetirementProof neverProof =
        gateway.QueryRetirementProof(never.token);
    EXPECT_EQ(neverProof.state, EcsSceneAssetRetirementProofState::NeverPublished);
    EXPECT_EQ(neverProof.appliedRenderSceneRevision, 0U);
    EXPECT_EQ(neverProof.presentedFrameSequence, 0U);
    EXPECT_TRUE(gateway.AcknowledgeRetirementProof(never.token));

    const EcsSceneAssetRetirementBeginReceipt begin =
        gateway.BeginRetirement(RetirementRequest(runtime, meshEntity));
    ASSERT_TRUE(begin.IsAccepted());
    const EcsRenderSceneRetirementResolution sceneOnly = ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(runtime, 2, 0, false),
        2,
        EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 1});
    EXPECT_EQ(sceneOnly.bindingCount, 0U);
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Pending);

    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(ECS::SceneRuntimeId{462}, 1, 0, false),
                                        3,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 3,
                                         .presentedFrameSequence = 3}));
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Presented);
}

TEST(EcsSceneRenderProofGatewayValidation,
     SceneOnlyRemovalReappearingInAcceptedFrameCannotReleaseRetirementProof)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{426};
    const ECS::EntityHandle meshEntity = ECS::EntityHandle::Create(11, 3);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);
    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 1, 7),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));

    const EcsSceneAssetRetirementBeginReceipt begin =
        gateway.BeginRetirement(RetirementRequest(runtime, meshEntity));
    ASSERT_TRUE(begin.IsAccepted());

    const EcsRenderSceneRetirementResolution sceneOnly = ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(runtime, 2, 0, false),
        2,
        EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 1});
    EXPECT_TRUE(sceneOnly.Empty());
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Pending);

    const EcsRenderSceneRetirementResolution reintroduced = ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(runtime, 3, 8),
        3,
        EcsFramePublicationDisposition::Accepted,
        {.appliedRenderSceneRevision = 3, .presentedFrameSequence = 3});
    EXPECT_TRUE(reintroduced.Empty());
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Pending);
}

TEST(EcsSceneRenderProofGatewayValidation,
     LostBoundedHistoryRejectsAbsentRetirementInsteadOfCallingItNeverPublished)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{436};
    constexpr uint32 historyCapacity = 4096;
    const uint32 firstEntityIndex = 100;
    const uint32 missingHistoryEntityIndex =
        firstEntityIndex + historyCapacity;
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);

    for (uint64 sequence = 1;
         sequence <= historyCapacity + 1U;
         ++sequence)
    {
        static_cast<void>(ExtractAndResolve(
            extractor,
            gateway,
            resolver,
            MakeOutput(runtime,
                       sequence,
                       sequence,
                       true,
                       firstEntityIndex + static_cast<uint32>(sequence - 1U)),
            sequence,
            EcsFramePublicationDisposition::Accepted,
            {.appliedRenderSceneRevision = sequence,
             .presentedFrameSequence = sequence}));
    }

    static_cast<void>(ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(ECS::SceneRuntimeId{437}, 1, 0, false),
        historyCapacity + 2U,
        EcsFramePublicationDisposition::Accepted,
        {.appliedRenderSceneRevision = historyCapacity + 2U,
         .presentedFrameSequence = historyCapacity + 2U}));

    const EcsSceneAssetRetirementBeginReceipt begin = gateway.BeginRetirement(
        RetirementRequest(runtime,
                          ECS::EntityHandle::Create(missingHistoryEntityIndex, 3)));
    EXPECT_EQ(begin.code, EcsSceneAssetRetirementBeginCode::FailedRetained);
}

TEST(EcsSceneRenderProofGatewayValidation,
     RuntimeSwapFullResetRetainsActualRemovalEvidenceForLateRetirementAdmission)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId firstRuntime{431};
    const ECS::SceneRuntimeId secondRuntime{432};
    const ECS::EntityHandle meshEntity = ECS::EntityHandle::Create(11, 3);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);
    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(firstRuntime, 1, 9),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));
    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(secondRuntime, 1, 13),
                                        2,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 2,
                                         .presentedFrameSequence = 2}));

    const EcsSceneAssetRetirementBeginReceipt late =
        gateway.BeginRetirement(RetirementRequest(firstRuntime, meshEntity));
    ASSERT_TRUE(late.IsAccepted());
    const EcsSceneAssetRetirementProof proof = gateway.QueryRetirementProof(late.token);
    EXPECT_EQ(proof.state, EcsSceneAssetRetirementProofState::Presented);
    EXPECT_EQ(proof.appliedRenderSceneRevision, 2U);
    EXPECT_EQ(proof.presentedFrameSequence, 2U);
}

TEST(EcsSceneRenderProofGatewayValidation,
     FeatureRetirementCapturesEveryTypeOnOneEntityAndWaitsForPresentation)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{441};
    const ECS::EntityHandle featureEntity = ECS::EntityHandle::Create(51, 4);
    constexpr uint8 allFeatures = FEATURE_PARTICLE | FEATURE_WATER | FEATURE_TERRAIN;
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);

    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 1, 0, false, 11,
                                                   allFeatures),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));

    const EcsSceneAssetRetirementBeginReceipt begin =
        gateway.BeginRetirement(RetirementRequest(runtime, featureEntity));
    ASSERT_TRUE(begin.IsAccepted());

    EcsFrameExtractionResult partiallyRemoved = extractor.Extract(
        Input(resolver, 2),
        MakeOutput(runtime, 2, 0, false, 11, FEATURE_WATER));
    ASSERT_TRUE(partiallyRemoved.IsComplete());
    ASSERT_EQ(partiallyRemoved.sceneUpdate->particles.size(), 1U);
    EXPECT_EQ(partiallyRemoved.sceneUpdate->particles.front().operation,
              RenderSceneMutationOperation::Remove);
    ASSERT_EQ(partiallyRemoved.sceneUpdate->terrain.size(), 1U);
    EXPECT_EQ(partiallyRemoved.sceneUpdate->terrain.front().operation,
              RenderSceneMutationOperation::Remove);
    EXPECT_TRUE(partiallyRemoved.sceneUpdate->water.empty());
    ASSERT_TRUE(gateway.ObserveExtractionCandidate(
        MakeOutput(runtime, 2, 0, false, 11, FEATURE_WATER), partiallyRemoved));
    const EcsRenderSceneRetirementResolution partialResolution =
        extractor.ResolveLastPublication(EcsFramePublicationDisposition::Accepted);
    EXPECT_TRUE(partialResolution.Empty());
    ASSERT_TRUE(gateway.ResolveObservedCandidate(
        EcsFramePublicationDisposition::Accepted,
        partialResolution,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 2}));
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Pending);

    const EcsFrozenSceneBridgeOutput removed = MakeOutput(runtime, 3, 0, false);
    EcsFrameExtractionResult finalRemoval = extractor.Extract(Input(resolver, 3), removed);
    ASSERT_TRUE(finalRemoval.IsComplete());
    ASSERT_EQ(finalRemoval.sceneUpdate->water.size(), 1U);
    EXPECT_EQ(finalRemoval.sceneUpdate->water.front().operation,
              RenderSceneMutationOperation::Remove);
    ASSERT_TRUE(gateway.ObserveExtractionCandidate(removed, finalRemoval));
    const EcsRenderSceneRetirementResolution resolution =
        extractor.ResolveLastPublication(EcsFramePublicationDisposition::Accepted);
    ASSERT_EQ(resolution.bindingCount, 1U);
    ASSERT_TRUE(gateway.ResolveObservedCandidate(
        EcsFramePublicationDisposition::Accepted,
        resolution,
        {.appliedRenderSceneRevision = 3, .presentedFrameSequence = 2}));
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::AppliedNotPresented);
    ASSERT_TRUE(gateway.ObservePublicationProgress(
        {.appliedRenderSceneRevision = 3, .presentedFrameSequence = 3}));
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Presented);
}

TEST(EcsSceneRenderProofGatewayValidation,
     FeatureSceneOnlyRemovalReappearanceAndIdentityMismatchesCannotCrossRelease)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{446};
    const ECS::EntityHandle featureEntity = ECS::EntityHandle::Create(51, 4);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);

    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 1, 0, false, 11,
                                                   FEATURE_PARTICLE),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));

    EcsRenderSceneRetirementBarrier waterOnlyBarrier;
    waterOnlyBarrier.correlation = {.value = 991};
    waterOnlyBarrier.sceneRuntimeId = runtime;
    waterOnlyBarrier.members = {{.entity = featureEntity,
                                 .type = EcsRenderSceneRetainedMemberType::Water}};
    EXPECT_EQ(extractor.SubmitRetirementBarrier(std::move(waterOnlyBarrier)).code,
              EcsRenderSceneRetirementBarrierSubmitCode::NeverPublishedIdentity);

    const EcsSceneAssetRetirementBeginReceipt staleGeneration = gateway.BeginRetirement(
        RetirementRequest(runtime, ECS::EntityHandle::Create(51, 5)));
    ASSERT_TRUE(staleGeneration.IsAccepted());
    EXPECT_EQ(gateway.QueryRetirementProof(staleGeneration.token).state,
              EcsSceneAssetRetirementProofState::NeverPublished);

    const EcsSceneAssetRetirementBeginReceipt begin =
        gateway.BeginRetirement(RetirementRequest(runtime, featureEntity));
    ASSERT_TRUE(begin.IsAccepted());
    const EcsRenderSceneRetirementResolution sceneOnly = ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(runtime, 2, 0, false),
        2,
        EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 1});
    EXPECT_TRUE(sceneOnly.Empty());
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Pending);

    const EcsRenderSceneRetirementResolution reintroduced = ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(runtime, 3, 0, false, 11, FEATURE_PARTICLE),
        3,
        EcsFramePublicationDisposition::Accepted,
        {.appliedRenderSceneRevision = 3, .presentedFrameSequence = 3});
    EXPECT_TRUE(reintroduced.Empty());
    EXPECT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Pending);
}

TEST(EcsSceneRenderProofGatewayValidation,
     FeatureFullResetAbsenceProvidesRetirementEvidence)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId firstRuntime{451};
    const ECS::SceneRuntimeId secondRuntime{452};
    const ECS::EntityHandle featureEntity = ECS::EntityHandle::Create(51, 4);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);

    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(firstRuntime, 1, 0, false, 11,
                                                   FEATURE_TERRAIN),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));
    const EcsSceneAssetRetirementBeginReceipt begin =
        gateway.BeginRetirement(RetirementRequest(firstRuntime, featureEntity));
    ASSERT_TRUE(begin.IsAccepted());

    const EcsRenderSceneRetirementResolution reset = ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(secondRuntime, 1, 0, false),
        2,
        EcsFramePublicationDisposition::Accepted,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 2});
    ASSERT_EQ(reset.bindingCount, 1U);
    const EcsSceneAssetRetirementProof proof = gateway.QueryRetirementProof(begin.token);
    EXPECT_EQ(proof.state, EcsSceneAssetRetirementProofState::Presented);
    EXPECT_EQ(proof.appliedRenderSceneRevision, 2U);
    EXPECT_EQ(proof.presentedFrameSequence, 2U);
}

TEST(EcsSceneRenderProofGatewayValidation,
     EnvironmentReceiptRequiresTheLatestExactSkyboxCandidateToBePresented)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{461};
    const ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Create(31, 3);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);

    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 1, 0, false, 11, 0,
                                                   51, 4, true, 71),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 0}));
    EXPECT_FALSE(gateway.BuildEnvironmentPresentationReceipt(
                     runtime, skyboxEntity, 71)
                     .has_value());

    ASSERT_TRUE(gateway.ObservePublicationProgress(
        {.appliedRenderSceneRevision = 1, .presentedFrameSequence = 1}));
    const auto receipt = gateway.BuildEnvironmentPresentationReceipt(
        runtime, skyboxEntity, 71);
    ASSERT_TRUE(receipt.has_value());
    EXPECT_TRUE(receipt->IsValid());
    EXPECT_EQ(receipt->frozenSourceSnapshotRevision, 1U);
    EXPECT_EQ(receipt->renderSceneRevision, 1U);
    EXPECT_EQ(receipt->carryingPresentedFrameSequence, 1U);
    EXPECT_FALSE(gateway.BuildEnvironmentPresentationReceipt(
                     runtime, skyboxEntity, 72)
                     .has_value());
    EXPECT_FALSE(gateway.BuildEnvironmentPresentationReceipt(
                     runtime, ECS::EntityHandle::Create(31, 4), 71)
                     .has_value());
    EXPECT_FALSE(gateway.BuildEnvironmentPresentationReceipt(
                     ECS::SceneRuntimeId{462}, skyboxEntity, 71)
                     .has_value());

    EcsFrozenSceneBridgeOutput sceneOnlyOutput =
        MakeOutput(runtime, 2, 0, false, 11, 0, 51, 4, true, 72);
    sceneOnlyOutput.skyboxes.front().exposure += 0.1f;
    const EcsRenderSceneRetirementResolution sceneOnly = ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        sceneOnlyOutput,
        2,
        EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 1});
    EXPECT_TRUE(sceneOnly.Empty());
    EXPECT_FALSE(gateway.BuildEnvironmentPresentationReceipt(
                     runtime, skyboxEntity, 71)
                     .has_value());
    EXPECT_FALSE(gateway.BuildEnvironmentPresentationReceipt(
                     runtime, skyboxEntity, 72)
                     .has_value());

    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 3, 0, false),
                                        3,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 3,
                                         .presentedFrameSequence = 3}));
    EXPECT_FALSE(gateway.BuildEnvironmentPresentationReceipt(
                     runtime, skyboxEntity, 71)
                     .has_value());
}

TEST(EcsSceneRenderProofGatewayValidation,
     EnvironmentReceiptAcceptsTheRegistryInitialSkyboxWriteVersion)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{463};
    const ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Create(31, 3);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);

    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 1, 0, false, 11, 0,
                                                   51, 4, true, 0),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));

    const auto receipt = gateway.BuildEnvironmentPresentationReceipt(
        runtime, skyboxEntity, 0);
    ASSERT_TRUE(receipt.has_value());
    EXPECT_TRUE(receipt->IsValid());
    EXPECT_EQ(receipt->skyboxWriteVersion, 0U);
}

TEST(EcsSceneRenderProofGatewayValidation,
     EnvironmentReceiptUsesTheNewestCompletedCandidateForTheCurrentExactSkybox)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{464};
    const ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Create(31, 3);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);

    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 1, 0, false, 11, 0,
                                                   51, 4, true, 73),
                                        1,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 1,
                                         .presentedFrameSequence = 1}));
    static_cast<void>(ExtractAndResolve(extractor,
                                        gateway,
                                        resolver,
                                        MakeOutput(runtime, 2, 0, false, 11, 0,
                                                   51, 4, true, 73),
                                        2,
                                        EcsFramePublicationDisposition::Accepted,
                                        {.appliedRenderSceneRevision = 2,
                                         .presentedFrameSequence = 1}));

    const auto receipt = gateway.BuildEnvironmentPresentationReceipt(
        runtime, skyboxEntity, 73);
    ASSERT_TRUE(receipt.has_value());
    EXPECT_EQ(receipt->frozenSourceSnapshotRevision, 1U);
    EXPECT_EQ(receipt->renderSceneRevision, 1U);
    EXPECT_EQ(receipt->carryingPresentedFrameSequence, 1U);
}

TEST(EcsSceneRenderProofGatewayValidation,
     EnvironmentReceiptFailsClosedForDuplicateOrLostSkyboxHistoryAndDeviceLoss)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{471};
    const ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Create(31, 3);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);

    EcsFrozenSceneBridgeOutput duplicate =
        MakeOutput(runtime, 1, 0, false, 11, 0, 51, 4, true, 81);
    duplicate.skyboxes.push_back(duplicate.skyboxes.front());
    const EcsFrameExtractionResult invalid = extractor.Extract(Input(resolver, 1), duplicate);
    EXPECT_FALSE(invalid.IsComplete());

    constexpr uint64 presentationHistoryCapacity = 64;
    for (uint64 sequence = 1; sequence <= presentationHistoryCapacity + 1U;
         ++sequence)
    {
        static_cast<void>(ExtractAndResolve(
            extractor,
            gateway,
            resolver,
            MakeOutput(runtime, sequence, 0, false, 11, 0, 51, 4, true, sequence),
            sequence,
            EcsFramePublicationDisposition::Accepted,
            {.appliedRenderSceneRevision = sequence,
             .presentedFrameSequence = sequence}));
    }
    EXPECT_FALSE(gateway.BuildEnvironmentPresentationReceipt(
                     runtime,
                     skyboxEntity,
                     presentationHistoryCapacity + 1U)
                     .has_value());
    ASSERT_TRUE(gateway.ObservePublicationProgress(
        {.appliedRenderSceneRevision = presentationHistoryCapacity + 1U,
         .presentedFrameSequence = presentationHistoryCapacity + 1U,
         .deviceLost = true}));
    EXPECT_FALSE(gateway.BuildEnvironmentPresentationReceipt(
                     runtime,
                     skyboxEntity,
                     presentationHistoryCapacity + 1U)
                     .has_value());
}

TEST(EcsSceneRenderProofGatewayValidation,
     OutstandingProofsRemainVisibleUntilTheExactTerminalTokenIsAcknowledged)
{
    Resolver resolver{{{101, {1, 1}}, {201, {2, 1}}}};
    const ECS::SceneRuntimeId runtime{481};
    const ECS::EntityHandle meshEntity = ECS::EntityHandle::Create(11, 3);
    EcsFrameExtractor extractor;
    EcsSceneRenderProofGateway gateway(extractor);
    EXPECT_FALSE(gateway.HasOutstandingProofs());

    static_cast<void>(ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(runtime, 1, 31),
        1,
        EcsFramePublicationDisposition::Accepted,
        {.appliedRenderSceneRevision = 1, .presentedFrameSequence = 1}));
    const EcsSceneAssetRetirementBeginReceipt begin =
        gateway.BeginRetirement(RetirementRequest(runtime, meshEntity));
    ASSERT_TRUE(begin.IsAccepted());
    EXPECT_TRUE(gateway.HasOutstandingProofs());

    static_cast<void>(ExtractAndResolve(
        extractor,
        gateway,
        resolver,
        MakeOutput(runtime, 2, 0, false),
        2,
        EcsFramePublicationDisposition::Accepted,
        {.appliedRenderSceneRevision = 2, .presentedFrameSequence = 2}));
    ASSERT_EQ(gateway.QueryRetirementProof(begin.token).state,
              EcsSceneAssetRetirementProofState::Presented);
    EXPECT_TRUE(gateway.HasOutstandingProofs());
    EXPECT_TRUE(gateway.AcknowledgeRetirementProof(begin.token));
    EXPECT_FALSE(gateway.HasOutstandingProofs());
}
