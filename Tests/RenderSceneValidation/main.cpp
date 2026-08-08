#include "GPUScene/GPUSceneUpdate.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderDrawPacket.h"
#include "Render/Renderer/RenderSceneDatabase.h"
#include "Render/Renderer/SceneRenderer.h"
#include "Render/Renderer/RenderScene.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RenderExtraction/RenderFramePacketBuilder.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionTracker.h"
#include "Runtime/RenderResourceGateway.h"

#include <algorithm>
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

namespace RVX
{
    class SceneRendererTestAccess final
    {
    public:
        static void SetShadowPublishAllocationFailure(
            SceneRenderer& renderer,
            bool enabled) noexcept
        {
            if (renderer.m_gpuSceneUpdate)
            {
                renderer.m_gpuSceneUpdate->SetThrowOnPublishForTesting(enabled);
            }
        }

        static void SetGPUSceneTier2DiagnosticVersions(
            SceneRenderer& renderer,
            uint64 residentVersion,
            uint64 leaseVersion) noexcept
        {
            renderer.m_renderPolicyDiagnostics.gpuSceneResidentVersion =
                residentVersion;
            renderer.m_renderPolicyDiagnostics.gpuSceneLeaseVersion =
                leaseVersion;
        }

        static void InvalidateRenderFramePlan(SceneRenderer& renderer)
        {
            renderer.InvalidateRenderFramePlan();
        }

        static void SetExecutionReport(
            SceneRenderer& renderer,
            std::vector<RenderPassExecutionReport> passes)
        {
            renderer.m_renderPolicyDiagnostics.planAvailable = true;
            renderer.m_renderPolicyDiagnostics.reportAvailable = false;
            renderer.m_renderPolicyDiagnostics.executionReport = {};
            renderer.m_renderPolicyDiagnostics.executionReport.passes =
                std::move(passes);
        }

        static void FinalizeExecutionReport(
            SceneRenderer& renderer,
            bool graphExecuted) noexcept
        {
            renderer.FinalizeRenderExecutionReportStatus(graphExecuted);
        }

        static void SetSubmissionReadyDiagnostics(
            SceneRenderer& renderer) noexcept
        {
            renderer.m_frameDiagnostics.rendered = true;
            renderer.m_frameDiagnostics.graphCompileValid = true;
        }

        static bool HasSubmissionFailure(const SceneRenderer& renderer) noexcept
        {
            return renderer.HasSubmissionFailure();
        }

        static void SetPersistentAccessSnapshots(
            SceneRenderer& renderer,
            const RHITextureAccessSnapshot& depthAccess,
            std::vector<RHITextureAccessSnapshot> backBufferAccesses,
            uint32 activeBackBufferIndex)
        {
            renderer.m_depthAccessSnapshot = depthAccess;
            renderer.m_backBufferAccessSnapshots = std::move(backBufferAccesses);
            renderer.m_activeBackBufferIndex = activeBackBufferIndex;
        }

        static void PublishProvisionalAccessSnapshots(
            SceneRenderer& renderer,
            const RHITextureAccessSnapshot& depthAccess,
            const RHITextureAccessSnapshot& backBufferAccess) noexcept
        {
            renderer.PublishProvisionalFrameAccessSnapshots(
                &depthAccess, &backBufferAccess);
        }

        static const RHITextureAccessSnapshot& GetDepthAccessSnapshot(
            const SceneRenderer& renderer) noexcept
        {
            return renderer.m_depthAccessSnapshot;
        }

        static const RHITextureAccessSnapshot& GetBackBufferAccessSnapshot(
            const SceneRenderer& renderer,
            uint32 index) noexcept
        {
            return renderer.m_backBufferAccessSnapshots[index];
        }

        static bool HasProvisionalAccessSnapshots(
            const SceneRenderer& renderer) noexcept
        {
            return renderer.m_frameAccessSnapshotRollback.pending;
        }
    };
} // namespace RVX

using namespace RVX;

namespace
{
    TEST(RenderSceneValidation,
         AbortedFrameRestoresProvisionalDepthAndActiveBackBufferAccessSnapshots)
    {
        SceneRenderer renderer;
        const RHITextureAccessSnapshot previousDepth =
            MakeRHITextureAccessSnapshot(RHIResourceState::DepthWrite,
                                         RHIShaderStage::None,
                                         GPUQueueDomain::Graphics,
                                         RHIContentValidity::Valid);
        const RHITextureAccessSnapshot previousBackBuffer0 =
            MakeRHITextureAccessSnapshot(RHIResourceState::Present,
                                         RHIShaderStage::None,
                                         GPUQueueDomain::Graphics,
                                         RHIContentValidity::Valid);
        const RHITextureAccessSnapshot previousBackBuffer1 =
            MakeRHITextureAccessSnapshot(RHIResourceState::RenderTarget,
                                         RHIShaderStage::Pixel,
                                         GPUQueueDomain::Graphics,
                                         RHIContentValidity::Valid);
        const RHITextureAccessSnapshot realizedDepth =
            MakeRHITextureAccessSnapshot(RHIResourceState::ShaderResource,
                                         RHIShaderStage::Pixel,
                                         GPUQueueDomain::Graphics,
                                         RHIContentValidity::Valid);
        const RHITextureAccessSnapshot realizedBackBuffer =
            MakeRHITextureAccessSnapshot(RHIResourceState::Present,
                                         RHIShaderStage::None,
                                         GPUQueueDomain::Graphics,
                                         RHIContentValidity::Valid);

        SceneRendererTestAccess::SetPersistentAccessSnapshots(
            renderer,
            previousDepth,
            {previousBackBuffer0, previousBackBuffer1},
            1u);
        SceneRendererTestAccess::PublishProvisionalAccessSnapshots(
            renderer, realizedDepth, realizedBackBuffer);
        EXPECT_TRUE(SceneRendererTestAccess::HasProvisionalAccessSnapshots(renderer));
        EXPECT_EQ(realizedDepth,
                  SceneRendererTestAccess::GetDepthAccessSnapshot(renderer));
        EXPECT_EQ(previousBackBuffer0,
                  SceneRendererTestAccess::GetBackBufferAccessSnapshot(renderer, 0u));
        EXPECT_EQ(realizedBackBuffer,
                  SceneRendererTestAccess::GetBackBufferAccessSnapshot(renderer, 1u));

        // This is the same release path used after RenderAcceptedFrame fails
        // or EndFrame returns a zero submission point.
        renderer.ReleaseUnsubmittedFrame();
        EXPECT_FALSE(SceneRendererTestAccess::HasProvisionalAccessSnapshots(renderer));
        EXPECT_EQ(previousDepth,
                  SceneRendererTestAccess::GetDepthAccessSnapshot(renderer));
        EXPECT_EQ(previousBackBuffer0,
                  SceneRendererTestAccess::GetBackBufferAccessSnapshot(renderer, 0u));
        EXPECT_EQ(previousBackBuffer1,
                  SceneRendererTestAccess::GetBackBufferAccessSnapshot(renderer, 1u));

        SceneRendererTestAccess::PublishProvisionalAccessSnapshots(
            renderer, realizedDepth, realizedBackBuffer);
        GPUCompletionToken completion;
        ASSERT_TRUE(InsertGPUCompletionPoint(
            completion, {GPUQueueDomain::Graphics, 1u}));
        renderer.NotifySubmission(completion);
        EXPECT_FALSE(SceneRendererTestAccess::HasProvisionalAccessSnapshots(renderer));
        EXPECT_EQ(realizedDepth,
                  SceneRendererTestAccess::GetDepthAccessSnapshot(renderer));
        EXPECT_EQ(realizedBackBuffer,
                  SceneRendererTestAccess::GetBackBufferAccessSnapshot(renderer, 1u));
    }

    class RegistryFixture final
    {
    public:
        RegistryFixture()
            : gateway(MakeConfig())
        {
            EXPECT_TRUE(registry.Initialize(&gateway.GetStatusTable(),
                                            &retirement));
        }

        ~RegistryFixture()
        {
            registry.Shutdown();
        }

        RenderResourceHandle Add(
            AssetId asset,
            RenderResourceKind kind,
            bool ready,
            std::vector<RenderResourceHandle> dependencies = {})
        {
            const RenderResourceReserveResult reserved =
                gateway.ReserveResource(asset, kind);
            EXPECT_EQ(reserved.code, RenderResourceReserveCode::Reserved);
            const RenderResourceHandle handle = reserved.handle;
            PackedRenderResourceStatus status{
                handle.generation,
                RenderResourcePublicState::Reserved,
                RenderResourceFailureCode::None};
            PackedRenderResourceStatus queued = status;
            queued.state = RenderResourcePublicState::UploadQueued;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle, status, queued, RenderStatusWriter::Update));
            PackedRenderResourceStatus uploading = queued;
            uploading.state = RenderResourcePublicState::Uploading;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle, queued, uploading, RenderStatusWriter::Render));
            EXPECT_TRUE(registry.BeginPending(handle,
                                              kind,
                                              dependencies));
            if (ready)
            {
                EXPECT_TRUE(registry.Commit(handle));
                PackedRenderResourceStatus gpuReady = uploading;
                gpuReady.state = RenderResourcePublicState::GPUReady;
                EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                    handle,
                    uploading,
                    gpuReady,
                    RenderStatusWriter::Render));
            }
            return handle;
        }

        RenderResourceHandle AddQueuedWithoutRegistry(
            AssetId asset,
            RenderResourceKind kind)
        {
            const RenderResourceReserveResult reserved =
                gateway.ReserveResource(asset, kind);
            EXPECT_EQ(reserved.code, RenderResourceReserveCode::Reserved);
            const RenderResourceHandle handle = reserved.handle;
            const PackedRenderResourceStatus status{
                handle.generation,
                RenderResourcePublicState::Reserved,
                RenderResourceFailureCode::None};
            PackedRenderResourceStatus queued = status;
            queued.state = RenderResourcePublicState::UploadQueued;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle, status, queued, RenderStatusWriter::Update));
            return handle;
        }

        RenderResourceHandle AddMeshWithMetadata(
            AssetId asset,
            const MeshUploadCreateInfo& createInfo,
            const std::vector<MeshUploadSubmesh>& submeshes)
        {
            const RenderResourceReserveResult reserved =
                gateway.ReserveResource(asset, RenderResourceKind::Mesh);
            EXPECT_EQ(reserved.code, RenderResourceReserveCode::Reserved);
            const RenderResourceHandle handle = reserved.handle;
            const PackedRenderResourceStatus status{
                handle.generation,
                RenderResourcePublicState::Reserved,
                RenderResourceFailureCode::None};
            PackedRenderResourceStatus queued = status;
            queued.state = RenderResourcePublicState::UploadQueued;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle, status, queued, RenderStatusWriter::Update));
            PackedRenderResourceStatus uploading = queued;
            uploading.state = RenderResourcePublicState::Uploading;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle, queued, uploading, RenderStatusWriter::Render));
            EXPECT_TRUE(registry.BeginPending(handle, RenderResourceKind::Mesh, {}));
            EXPECT_TRUE(registry.SetPendingMeshMetadata(
                handle, createInfo, submeshes));
            EXPECT_TRUE(registry.Commit(handle));
            PackedRenderResourceStatus gpuReady = uploading;
            gpuReady.state = RenderResourcePublicState::GPUReady;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle, uploading, gpuReady, RenderStatusWriter::Render));
            return handle;
        }

        static RenderTransportConfig MakeConfig()
        {
            RenderTransportConfig config;
            config.statusSlotCapacity = 1024;
            config.uploadRequestCapacity = 8;
            config.uploadByteCapacity = 1024;
            return config;
        }

        RenderResourceGateway gateway;
        RenderRetirementQueue retirement;
        RenderResourceRegistry registry;
    };

    RenderFeatureSnapshot MakeFeatures(uint64 sequence)
    {
        RenderFeatureSnapshot features;
        features.BeginBuild(sequence);
        ParticleRenderSnapshotItem particle;
        particle.instanceId = 7;
        particle.systemId = 8;
        particle.systemName = "packet-owned-feature";
        particle.worldBounds = AABB(Vec3{-1.0f}, Vec3{1.0f});
        features.particles.items.push_back(std::move(particle));
        features.MarkComplete();
        return features;
    }

    RenderPrimitiveSnapshot MakePrimitive(
        RenderResourceHandle mesh,
        RenderResourceHandle material = {},
        const Vec3& position = Vec3{0.0f})
    {
        RenderPrimitiveSnapshot primitive;
        primitive.objectId = 42;
        primitive.mesh = mesh;
        primitive.material = material;
        primitive.worldTransform = Mat4Identity();
        primitive.worldTransform[3] = Vec4(position, 1.0f);
        primitive.previousWorldTransform = primitive.worldTransform;
        primitive.boundsMin = position - Vec3{1.0f};
        primitive.boundsMax = position + Vec3{1.0f};
        primitive.flags = 0x7U;
        primitive.layerMask = 0x55U;
        primitive.sortKey = 900;
        primitive.skinMatrices.push_back(Mat4Identity());
        return primitive;
    }

    std::unique_ptr<const RenderFramePacket> MakePacket(
        uint64 sequence,
        uint64 worldRevision,
        uint64 temporalEpoch,
        bool discontinuity,
        std::vector<RenderPrimitiveSnapshot> primitives)
    {
        RenderFramePacketBuilder builder;
        RenderFrameHeader header;
        header.sequence = sequence;
        header.worldRevision = worldRevision;
        header.temporalEpoch = temporalEpoch;
        header.explicitDiscontinuity = discontinuity;
        header.expectedPrimitiveCount =
            static_cast<uint32>(primitives.size());
        header.extractedPrimitiveCount = header.expectedPrimitiveCount;
        static_cast<void>(builder.SetHeader(header));

        RenderViewSnapshot view;
        view.viewportWidth = 1280;
        view.viewportHeight = 720;
        view.cameraPosition = {0.0f, 0.0f, 5.0f};
        static_cast<void>(builder.SetView(view));
        for (RenderPrimitiveSnapshot& primitive : primitives)
        {
            static_cast<void>(builder.AddPrimitive(std::move(primitive)));
        }
        static_cast<void>(builder.SetSky({}));
        static_cast<void>(builder.SetEnvironment({}));
        static_cast<void>(builder.SetSettings({}));
        static_cast<void>(builder.SetCaptureRequest({}));
        static_cast<void>(builder.SetFeatures(MakeFeatures(sequence)));
        RenderExtractionDiagnostics diagnostics;
        diagnostics.code = RenderExtractionCode::Complete;
        diagnostics.complete = true;
        static_cast<void>(builder.SetExtractionDiagnostics(diagnostics));
        return builder.Seal();
    }

    RenderFrameApplyResult Apply(
        RenderScene& scene,
        const RenderResourceRegistry& registry,
        uint64 sequence,
        uint64 worldRevision,
        uint64 temporalEpoch,
        bool discontinuity,
        RenderPrimitiveSnapshot primitive)
    {
        std::unique_ptr<const RenderFramePacket> packet = MakePacket(
            sequence,
            worldRevision,
            temporalEpoch,
            discontinuity,
            {std::move(primitive)});
        EXPECT_NE(packet, nullptr);
        return scene.ApplyFramePacket(*packet, registry);
    }
} // namespace

TEST(RenderSceneValidation, AppliesTransactionallyAndOwnsPacketValues)
{
    RegistryFixture resources;
    const RenderResourceHandle mesh =
        resources.Add({1}, RenderResourceKind::Mesh, true);
    const RenderResourceHandle material =
        resources.Add({2}, RenderResourceKind::Material, true);
    std::unique_ptr<const RenderFramePacket> packet =
        MakePacket(10, 3, 4, false, {MakePrimitive(mesh, material, {2, 0, 0})});
    ASSERT_NE(packet, nullptr);

    RenderScene scene;
    RenderFrameApplyResult applied =
        scene.ApplyFramePacket(*packet, resources.registry);
    ASSERT_TRUE(applied.IsApplied());
    packet.reset();

    ASSERT_EQ(scene.GetObjectCount(), 1U);
    const RenderObject& object = scene.GetObject(0);
    EXPECT_EQ(object.mesh, mesh);
    EXPECT_EQ(object.material, material);
    EXPECT_EQ(Vec3(object.worldMatrix[3]), (Vec3{2, 0, 0}));
    EXPECT_EQ(object.skinningMatrices.size(), 1U);
    EXPECT_EQ(scene.GetFeatures().particles.items[0].systemName,
              "packet-owned-feature");
    EXPECT_EQ(scene.GetAcceptedHeader().sequence, 10U);
}

TEST(RenderSceneValidation,
     AppliesV5DirectlyFromPersistentDatabaseWithoutCompatibilityPacket)
{
    RegistryFixture resources;
    const RenderResourceHandle mesh =
        resources.Add({31}, RenderResourceKind::Mesh, true);
    const RenderResourceHandle material =
        resources.Add({32}, RenderResourceKind::Material, true);

    RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    ASSERT_TRUE(reset.UpsertPrimitive(
        MakePrimitive(mesh, material, {4.0F, 0.0F, 0.0F}), true));
    reset.UpsertSky(RenderSkySnapshot{});
    reset.UpsertEnvironment(RenderEnvironmentSnapshot{});
    RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(3)).IsApplied());

    RenderFrameHeaderV5 header;
    header.sequence = 77;
    header.requiredSceneRevision = 3;
    header.worldRevision = 9;
    header.temporalEpoch = 2;
    RenderViewSnapshot view;
    view.viewportWidth = 1280;
    view.viewportHeight = 720;
    RenderExtractionDiagnostics diagnostics;
    diagnostics.complete = true;
    const auto frame = RenderFramePacketV5::Create(
        header,
        view,
        RenderFrameSettings{},
        RenderFrameCaptureRequest{},
        diagnostics);
    ASSERT_NE(frame, nullptr);

    RenderScene scene;
    const RenderFrameApplyResult result =
        scene.ApplyFrameV5(*frame, database, resources.registry);
    ASSERT_TRUE(result.IsApplied());
    EXPECT_EQ(scene.GetAcceptedHeader().sequence, 77U);
    ASSERT_EQ(scene.GetObjectCount(), 1U);
    EXPECT_EQ(scene.GetObject(0).entityId, 42U);
    EXPECT_EQ(Vec3(scene.GetObject(0).worldMatrix[3]),
              (Vec3{4.0F, 0.0F, 0.0F}));
}

TEST(RenderSceneValidation, ShadowPublicationFailureCannotRejectAnAppliedFrame)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 3;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {700}, createInfo, {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles}});
    SceneRenderer renderer;

    std::unique_ptr<const RenderFramePacket> first = MakePacket(
        101, 1, 1, false, {MakePrimitive(mesh)});
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(renderer.ApplyFramePacket(*first, resources.registry).IsApplied());
    const GPUScenePublicationStats before =
        renderer.GetGPUScenePublicationStats();
    ASSERT_EQ(before.failureReason, GPUScenePublicationFailureReason::None);
    ASSERT_EQ(before.publishedObjectCount, 1U);

    SceneRendererTestAccess::SetShadowPublishAllocationFailure(renderer, true);
    std::unique_ptr<const RenderFramePacket> second = MakePacket(
        102, 2, 1, false, {MakePrimitive(mesh, {}, {3.0F, 0.0F, 0.0F})});
    ASSERT_NE(second, nullptr);
    const RenderFrameApplyResult applied =
        renderer.ApplyFramePacket(*second, resources.registry);

    EXPECT_TRUE(applied.IsApplied());
    EXPECT_EQ(renderer.GetRenderScene().GetAcceptedHeader().sequence, 102U);
    const GPUScenePublicationStats& failed =
        renderer.GetGPUScenePublicationStats();
    EXPECT_EQ(failed.failureReason, GPUScenePublicationFailureReason::AllocationFailed);
    EXPECT_EQ(failed.committedVersion, before.committedVersion);
    EXPECT_EQ(failed.committedSourceSequence, before.committedSourceSequence);
    EXPECT_EQ(failed.publishedObjectCount, before.publishedObjectCount);
    EXPECT_EQ(failed.publishedDrawCount, before.publishedDrawCount);
    EXPECT_FALSE(failed.executionEligible);

    const GPUSceneDiagnostics snapshot = renderer.GetGPUSceneDiagnostics();
    EXPECT_TRUE(snapshot.available);
    EXPECT_TRUE(snapshot.informationalOnly);
    EXPECT_TRUE(snapshot.publicationAttempted);
    EXPECT_TRUE(snapshot.publicationFailed);
    EXPECT_FALSE(snapshot.publicationPublished);
    EXPECT_FALSE(snapshot.publicationComplete);
    EXPECT_EQ(snapshot.publicationFailureReason,
              GPUScenePublicationFailureReason::AllocationFailed);
    EXPECT_EQ(snapshot.committedVersion, before.committedVersion);
    const GPUSceneDiagnostics copied = snapshot;
    EXPECT_EQ(copied.committedVersion, snapshot.committedVersion);
    EXPECT_EQ(copied.publicationPublished, snapshot.publicationPublished);
}

TEST(RenderSceneValidation, GPUSceneTier2DiagnosticVersionsAreValueOnlyAndResetWithFramePlan)
{
    SceneRenderer renderer;
    SceneRendererTestAccess::SetGPUSceneTier2DiagnosticVersions(
        renderer, 47u, 47u);

    const RenderPolicyDiagnostics copied =
        renderer.GetRenderPolicyDiagnostics();
    EXPECT_EQ(copied.gpuSceneResidentVersion, 47u);
    EXPECT_EQ(copied.gpuSceneLeaseVersion, 47u);

    SceneRendererTestAccess::InvalidateRenderFramePlan(renderer);
    const RenderPolicyDiagnostics& reset = renderer.GetRenderPolicyDiagnostics();
    EXPECT_EQ(reset.gpuSceneResidentVersion, 0u);
    EXPECT_EQ(reset.gpuSceneLeaseVersion, 0u);
}

TEST(RenderSceneValidation,
     FrameExecutionStatusCompletesWithMixedCompletedAndNotAttemptedPasses)
{
    SceneRenderer renderer;
    RenderPassExecutionReport completed;
    completed.pass = RenderPassKind::Opaque;
    completed.status = RenderExecutionStatus::Completed;
    RenderPassExecutionReport omitted;
    omitted.pass = RenderPassKind::Depth;
    omitted.status = RenderExecutionStatus::NotAttempted;

    SceneRendererTestAccess::SetExecutionReport(
        renderer, {completed, omitted});
    SceneRendererTestAccess::FinalizeExecutionReport(renderer, true);

    const RenderPolicyDiagnostics& diagnostics =
        renderer.GetRenderPolicyDiagnostics();
    EXPECT_TRUE(diagnostics.reportAvailable);
    EXPECT_EQ(diagnostics.executionReport.status,
              RenderExecutionStatus::Completed);
    ASSERT_EQ(diagnostics.executionReport.passes.size(), 2U);
    EXPECT_EQ(diagnostics.executionReport.passes[0].status,
              RenderExecutionStatus::Completed);
    EXPECT_EQ(diagnostics.executionReport.passes[1].status,
              RenderExecutionStatus::NotAttempted);
}

TEST(RenderSceneValidation,
     FrameExecutionStatusFailsAtomicallyWhenAnyRecordedPassFails)
{
    SceneRenderer renderer;
    RenderPassExecutionReport completed;
    completed.pass = RenderPassKind::Depth;
    completed.status = RenderExecutionStatus::Completed;
    RenderPassExecutionReport failed;
    failed.pass = RenderPassKind::Opaque;
    failed.status = RenderExecutionStatus::Failed;

    SceneRendererTestAccess::SetExecutionReport(renderer, {completed, failed});
    SceneRendererTestAccess::FinalizeExecutionReport(renderer, true);

    const RenderPolicyDiagnostics& diagnostics =
        renderer.GetRenderPolicyDiagnostics();
    EXPECT_TRUE(diagnostics.reportAvailable);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              diagnostics.executionReport.status);
    SceneRendererTestAccess::SetSubmissionReadyDiagnostics(renderer);
    EXPECT_TRUE(SceneRendererTestAccess::HasSubmissionFailure(renderer));
    ASSERT_EQ(2U, diagnostics.executionReport.passes.size());
    EXPECT_EQ(RenderExecutionStatus::Completed,
              diagnostics.executionReport.passes[0].status);
    EXPECT_EQ(RenderExecutionStatus::Failed,
              diagnostics.executionReport.passes[1].status);
}

TEST(RenderSceneValidation,
     FrameExecutionStatusStaysNotAttemptedWhenNoPassRecords)
{
    SceneRenderer renderer;
    RenderPassExecutionReport depth;
    depth.pass = RenderPassKind::Depth;
    RenderPassExecutionReport opaque;
    opaque.pass = RenderPassKind::Opaque;

    SceneRendererTestAccess::SetExecutionReport(renderer, {depth, opaque});
    SceneRendererTestAccess::FinalizeExecutionReport(renderer, true);

    const RenderPolicyDiagnostics& diagnostics =
        renderer.GetRenderPolicyDiagnostics();
    EXPECT_TRUE(diagnostics.reportAvailable);
    EXPECT_EQ(diagnostics.executionReport.status,
              RenderExecutionStatus::NotAttempted);
    ASSERT_EQ(diagnostics.executionReport.passes.size(), 2U);
    EXPECT_EQ(diagnostics.executionReport.passes[0].status,
              RenderExecutionStatus::NotAttempted);
    EXPECT_EQ(diagnostics.executionReport.passes[1].status,
              RenderExecutionStatus::NotAttempted);
}

TEST(RenderSceneValidation,
     FrameExecutionReportStaysUnavailableWhenTheGraphDoesNotExecute)
{
    SceneRenderer renderer;
    RenderPassExecutionReport completed;
    completed.pass = RenderPassKind::Opaque;
    completed.status = RenderExecutionStatus::Completed;

    SceneRendererTestAccess::SetExecutionReport(renderer, {completed});
    SceneRendererTestAccess::FinalizeExecutionReport(renderer, false);

    const RenderPolicyDiagnostics& diagnostics =
        renderer.GetRenderPolicyDiagnostics();
    EXPECT_FALSE(diagnostics.reportAvailable);
    EXPECT_EQ(diagnostics.executionReport.status,
              RenderExecutionStatus::NotAttempted);
    ASSERT_EQ(diagnostics.executionReport.passes.size(), 1U);
    EXPECT_EQ(diagnostics.executionReport.passes[0].status,
              RenderExecutionStatus::Completed);
}

TEST(RenderSceneValidation, ShadowPreviousTransformFollowsRenderedHistoryAndDiscontinuities)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 3;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {701}, createInfo, {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles}});
    RenderScene scene;
    GPUSceneUpdate shadow;

    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      1,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {1.0F, 0.0F, 0.0F}))
                    .IsApplied());
    const GPUScenePublicationStats firstPublication =
        shadow.Publish(scene, resources.registry);
    ASSERT_EQ(firstPublication.failureReason,
              GPUScenePublicationFailureReason::None);
    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      2,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {1.0F, 0.0F, 0.0F}))
                    .IsApplied());
    const GPUScenePublicationStats noOpPublication =
        shadow.Publish(scene, resources.registry);
    EXPECT_EQ(noOpPublication.committedVersion,
              firstPublication.committedVersion);
    EXPECT_EQ(noOpPublication.committedSourceSequence, 2U);
    EXPECT_EQ(noOpPublication.noOpCount, 1U);

    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      3,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {2.0F, 0.0F, 0.0F}))
                    .IsApplied());
    ASSERT_EQ(shadow.Publish(scene, resources.registry).failureReason,
              GPUScenePublicationFailureReason::None);
    const GPUSceneTransformRow& acceptedButNotRendered =
        shadow.GetCommittedMirrorForTesting().transforms[1];
    EXPECT_FALSE(HasGPUSceneTransformFlag(
        acceptedButNotRendered.transformFlags,
        GPUSceneTransformFlags::PreviousWorldFromLocalValid));

    scene.MarkAcceptedFrameRendered();
    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      4,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {3.0F, 0.0F, 0.0F}))
                    .IsApplied());
    ASSERT_EQ(shadow.Publish(scene, resources.registry).failureReason,
              GPUScenePublicationFailureReason::None);
    const GPUSceneTransformRow& renderedHistory =
        shadow.GetCommittedMirrorForTesting().transforms[1];
    EXPECT_TRUE(HasGPUSceneTransformFlag(
        renderedHistory.transformFlags,
        GPUSceneTransformFlags::PreviousWorldFromLocalValid));
    EXPECT_FLOAT_EQ(renderedHistory.previousWorldFromLocal.rows[0].w, 2.0F);

    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      5,
                      1,
                      1,
                      true,
                      MakePrimitive(mesh, {}, {4.0F, 0.0F, 0.0F}))
                    .IsApplied());
    ASSERT_EQ(shadow.Publish(scene, resources.registry).failureReason,
              GPUScenePublicationFailureReason::None);
    const GPUSceneTransformRow& discontinuity =
        shadow.GetCommittedMirrorForTesting().transforms[1];
    EXPECT_FALSE(HasGPUSceneTransformFlag(
        discontinuity.transformFlags,
        GPUSceneTransformFlags::PreviousWorldFromLocalValid));
}

TEST(RenderSceneValidation,
     BuildsAuthoritativeBatchesFromRegistrySubmeshesAndBindings)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 6;
    createInfo.indexType = MeshUploadIndexType::UInt16;
    createInfo.boundsMin = {-1.0f, -1.0f, -1.0f};
    createInfo.boundsMax = {1.0f, 1.0f, 1.0f};
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {101}, createInfo,
        {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles},
         {3, 3, 0, MeshUploadPrimitiveTopology::Triangles}});
    const RenderResourceHandle maskedMaterial =
        resources.Add({102}, RenderResourceKind::Material, true);
    const RenderResourceHandle transparentMaterial =
        resources.Add({103}, RenderResourceKind::Material, true);

    RenderPrimitiveSnapshot primitive = MakePrimitive(mesh, maskedMaterial);
    primitive.submeshes = {
        {0, maskedMaterial, RenderMaterialMode::Masked},
        {1, transparentMaterial, RenderMaterialMode::Transparent}};
    std::unique_ptr<const RenderFramePacket> packet = MakePacket(
        1, 1, 1, false, {primitive});
    ASSERT_NE(packet, nullptr);

    RenderScene scene;
    ASSERT_TRUE(scene.ApplyFramePacket(*packet, resources.registry).IsApplied());
    const RenderObject& object = scene.GetObject(0);
    ASSERT_TRUE(object.meshBatchesAuthoritative);
    ASSERT_EQ(object.meshBatches.size(), 2U);
    EXPECT_EQ(object.meshBatches[0].material, maskedMaterial);
    EXPECT_EQ(object.meshBatches[1].material, transparentMaterial);
    EXPECT_EQ(object.meshBatches[0].materialMode, RenderMaterialMode::Masked);
    EXPECT_EQ(object.meshBatches[1].materialMode,
              RenderMaterialMode::Transparent);
    EXPECT_EQ(object.material, maskedMaterial);
    ASSERT_EQ(object.materialModes.size(), 2U);
    EXPECT_EQ(object.materialModes[1], RenderMaterialMode::Transparent);
    const auto& references = scene.GetReferencedResources();
    EXPECT_NE(std::find(references.begin(), references.end(), maskedMaterial),
              references.end());
    EXPECT_NE(std::find(references.begin(), references.end(), transparentMaterial),
              references.end());
}

TEST(RenderSceneValidation,
     RejectsIncompleteAuthoritativeBindingsWithoutMutatingScene)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 6;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {111}, createInfo,
        {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles},
         {3, 3, 0, MeshUploadPrimitiveTopology::Triangles}});
    const RenderResourceHandle material =
        resources.Add({112}, RenderResourceKind::Material, true);
    RenderScene scene;
    ASSERT_TRUE(Apply(scene, resources.registry, 1, 1, 1, false,
                      MakePrimitive(mesh, material)).IsApplied());

    RenderPrimitiveSnapshot incomplete = MakePrimitive(mesh, material);
    incomplete.submeshes = {{0, material, RenderMaterialMode::Opaque}};
    std::unique_ptr<const RenderFramePacket> packet = MakePacket(
        2, 1, 1, false, {std::move(incomplete)});
    ASSERT_NE(packet, nullptr);
    EXPECT_EQ(scene.ApplyFramePacket(*packet, resources.registry).code,
              RenderFrameApplyCode::InvalidPacket);
    EXPECT_EQ(scene.GetAcceptedHeader().sequence, 1U);
}

TEST(RenderSceneValidation, MissingSubmeshMaterialStillBuildsDrawableBatch)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 3;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {121}, createInfo,
        {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles}});
    RenderPrimitiveSnapshot primitive = MakePrimitive(mesh);
    primitive.submeshes = {{0, {}, RenderMaterialMode::Opaque}};
    RenderScene scene;
    const RenderFrameApplyResult result = Apply(
        scene, resources.registry, 1, 1, 1, false, std::move(primitive));
    ASSERT_TRUE(result.IsApplied());
    const RenderObject& object = scene.GetObject(0);
    EXPECT_TRUE(object.drawable);
    ASSERT_EQ(object.meshBatches.size(), 1U);
    EXPECT_FALSE(object.meshBatches[0].material.IsValid());
    EXPECT_TRUE(HasRenderBatchFlag(object.meshBatches[0].flags,
                                   RenderBatchFlags::MissingMaterial));
}

TEST(RenderSceneValidation,
     RetainsAcceptedPacketTemplatesAndRejectsInvalidPublications)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 3;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {122}, createInfo,
        {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles}});
    const RenderResourceHandle material =
        resources.Add({123}, RenderResourceKind::Material, true);
    RenderScene scene;

    RenderPrimitiveSnapshot initial = MakePrimitive(
        mesh, material, {1.0f, 0.0f, 0.0f});
    initial.skinMatrices = {Mat4Identity()};
    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      1,
                      1,
                      1,
                      false,
                      std::move(initial))
                    .IsApplied());
    const RenderDrawPacketCacheStats afterFirst =
        scene.GetDrawPacketCacheStats();
    ASSERT_EQ(afterFirst.entryCount, 1U);
    EXPECT_EQ(afterFirst.packetBuildCount, 1U);

    RenderPrimitiveSnapshot leading = MakePrimitive(mesh, material);
    leading.objectId = 43;
    RenderPrimitiveSnapshot retained = MakePrimitive(
        mesh, material, {7.0f, 0.0f, 0.0f});
    retained.skinMatrices = {Mat4Identity(), Mat4Identity()};
    retained.skinMatrices[1][3] = Vec4{2.0f, 0.0f, 0.0f, 1.0f};
    std::unique_ptr<const RenderFramePacket> secondPacket = MakePacket(
        2, 1, 1, false, {std::move(leading), std::move(retained)});
    ASSERT_NE(secondPacket, nullptr);
    ASSERT_TRUE(scene.ApplyFramePacket(*secondPacket, resources.registry)
                    .IsApplied());

    const RenderDrawPacketCacheStats afterSecond =
        scene.GetDrawPacketCacheStats();
    EXPECT_EQ(afterSecond.packetBuildCount, afterFirst.packetBuildCount + 1U);
    EXPECT_EQ(afterSecond.entryCreationCount,
              afterFirst.entryCreationCount + 1U);
    EXPECT_EQ(afterSecond.hitCount, afterFirst.hitCount + 1U);

    std::vector<RenderDrawItem> opaque;
    std::vector<RenderDrawItem> masked;
    std::vector<RenderDrawItem> transparent;
    BuildMaterialDrawLists(
        scene, {1}, Vec3{0.0f}, opaque, masked, transparent);
    ASSERT_EQ(opaque.size(), 1U);
    EXPECT_EQ(opaque[0].packet,
              BuildLegacyMaterialDrawPacket(scene.GetObject(1).meshBatches[0]));
    EXPECT_EQ(opaque[0].packet.primitiveData, 1U);
    const auto& referencedResources = scene.GetReferencedResources();
    EXPECT_NE(std::find(referencedResources.begin(), referencedResources.end(),
                        opaque[0].packet.geometryKey.mesh),
              referencedResources.end());
    EXPECT_NE(std::find(referencedResources.begin(), referencedResources.end(),
                        opaque[0].packet.materialKey.material),
              referencedResources.end());
    const RenderDrawPacketCacheStats afterFirstDrawList =
        scene.GetDrawPacketCacheStats();
    EXPECT_EQ(afterFirstDrawList.packetBuildCount,
              afterSecond.packetBuildCount);
    EXPECT_EQ(afterFirstDrawList.entryCreationCount,
              afterSecond.entryCreationCount);

    BuildMaterialDrawLists(
        scene, {1}, Vec3{0.0f}, opaque, masked, transparent);
    const RenderDrawPacketCacheStats afterSecondDrawList =
        scene.GetDrawPacketCacheStats();
    EXPECT_EQ(afterSecondDrawList.packetBuildCount,
              afterSecond.packetBuildCount);
    EXPECT_EQ(afterSecondDrawList.entryCreationCount,
              afterSecond.entryCreationCount);

    const RenderResourceHandle stale{mesh.slot, mesh.generation + 1};
    EXPECT_EQ(Apply(scene,
                    resources.registry,
                    3,
                    1,
                    1,
                    false,
                    MakePrimitive(stale, material))
                  .code,
              RenderFrameApplyCode::StaleRequiredHandle);
    const RenderDrawPacketCacheStats afterRejected =
        scene.GetDrawPacketCacheStats();
    EXPECT_EQ(afterRejected.entryCount, afterSecond.entryCount);
    EXPECT_EQ(afterRejected.packetBuildCount, afterSecond.packetBuildCount);
    EXPECT_EQ(afterRejected.entryCreationCount,
              afterSecond.entryCreationCount);
}

TEST(RenderSceneValidation, RejectsSchemaOrderAndStaleHandlesWithoutMutation)
{
    RegistryFixture resources;
    const RenderResourceHandle mesh =
        resources.Add({3}, RenderResourceKind::Mesh, true);
    RenderScene scene;
    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      10,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {1, 0, 0}))
                    .IsApplied());

    std::unique_ptr<const RenderFramePacket> unsupported =
        MakePacket(11, 1, 1, false, {MakePrimitive(mesh, {}, {2, 0, 0})});
    ASSERT_NE(unsupported, nullptr);
    const_cast<RenderFrameHeader&>(unsupported->GetHeader()).schemaVersion += 1;
    EXPECT_EQ(scene.ApplyFramePacket(*unsupported, resources.registry).code,
              RenderFrameApplyCode::UnsupportedSchema);
    EXPECT_EQ(Vec3(scene.GetObject(0).worldMatrix[3]), (Vec3{1, 0, 0}));

    EXPECT_EQ(Apply(scene,
                    resources.registry,
                    9,
                    1,
                    1,
                    false,
                    MakePrimitive(mesh, {}, {3, 0, 0}))
                  .code,
              RenderFrameApplyCode::OutOfOrder);
    EXPECT_EQ(Vec3(scene.GetObject(0).worldMatrix[3]), (Vec3{1, 0, 0}));

    const RenderResourceHandle stale{mesh.slot, mesh.generation + 1};
    EXPECT_EQ(Apply(scene,
                    resources.registry,
                    12,
                    1,
                    1,
                    false,
                    MakePrimitive(stale, {}, {4, 0, 0}))
                  .code,
              RenderFrameApplyCode::StaleRequiredHandle);
    EXPECT_EQ(scene.GetAcceptedHeader().sequence, 10U);
    EXPECT_EQ(Vec3(scene.GetObject(0).worldMatrix[3]), (Vec3{1, 0, 0}));
}

TEST(RenderSceneValidation, UsesReadyFallbackForPendingRequiredMesh)
{
    RegistryFixture resources;
    const RenderResourceHandle pending =
        resources.Add({4}, RenderResourceKind::Mesh, false);
    const RenderResourceHandle fallback =
        resources.Add({5}, RenderResourceKind::Mesh, true);
    RenderPrimitiveSnapshot primitive = MakePrimitive(pending);
    primitive.fallbackMesh = fallback;

    RenderScene scene;
    RenderFrameApplyResult result = Apply(
        scene, resources.registry, 1, 1, 1, false, std::move(primitive));
    ASSERT_TRUE(result.IsApplied());
    EXPECT_EQ(result.pendingFallbackCount, 1U);
    EXPECT_EQ(result.skippedDrawCount, 0U);
    EXPECT_EQ(scene.GetObject(0).mesh, fallback);
    EXPECT_TRUE(scene.GetObject(0).drawable);
}

TEST(RenderSceneValidation,
     UsesFallbackMeshMetadataWithLegacyMaterialForEverySubmesh)
{
    RegistryFixture resources;
    const RenderResourceHandle pendingPreferred =
        resources.Add({44}, RenderResourceKind::Mesh, false);
    MeshUploadCreateInfo fallbackCreateInfo;
    fallbackCreateInfo.indexCount = 6;
    fallbackCreateInfo.indexType = MeshUploadIndexType::UInt16;
    const RenderResourceHandle readyFallback = resources.AddMeshWithMetadata(
        {45}, fallbackCreateInfo,
        {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles},
         {3, 3, 0, MeshUploadPrimitiveTopology::TriangleStrip}});
    const RenderResourceHandle legacyMaterial =
        resources.Add({46}, RenderResourceKind::Material, true);
    const RenderResourceHandle ignoredSecondMaterial =
        resources.Add({47}, RenderResourceKind::Material, true);

    RenderPrimitiveSnapshot primitive = MakePrimitive(
        pendingPreferred, legacyMaterial);
    primitive.fallbackMesh = readyFallback;
    primitive.flags |= static_cast<uint32>(RenderMaterialMode::Masked) << 8U;
    primitive.submeshes = {
        {0, legacyMaterial, RenderMaterialMode::Masked},
        {1, ignoredSecondMaterial, RenderMaterialMode::Transparent}};

    RenderScene scene;
    const RenderFrameApplyResult result = Apply(
        scene, resources.registry, 1, 1, 1, false, std::move(primitive));
    ASSERT_TRUE(result.IsApplied());
    EXPECT_EQ(result.pendingFallbackCount, 1U);
    const RenderObject& object = scene.GetObject(0);
    EXPECT_EQ(object.mesh, readyFallback);
    ASSERT_TRUE(object.meshBatchesAuthoritative);
    ASSERT_EQ(object.meshBatches.size(), 2U);
    for (const MeshBatch& batch : object.meshBatches)
    {
        EXPECT_EQ(batch.material, legacyMaterial);
        EXPECT_EQ(batch.materialMode, RenderMaterialMode::Masked);
    }
    ASSERT_EQ(object.materialModes.size(), 2U);
    EXPECT_EQ(object.material, legacyMaterial);
    EXPECT_EQ(object.materialModes[0], RenderMaterialMode::Masked);
    EXPECT_EQ(object.materialModes[1], RenderMaterialMode::Masked);
}

TEST(RenderSceneValidation,
     SynthesizesSingleSubmeshFromCreateInfoForGeometryAndDrawArguments)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 9;
    createInfo.indexType = MeshUploadIndexType::UInt16;
    createInfo.topology = MeshUploadPrimitiveTopology::TriangleStrip;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {48}, createInfo, {});
    const RenderResourceHandle material =
        resources.Add({49}, RenderResourceKind::Material, true);

    RenderScene scene;
    ASSERT_TRUE(Apply(scene, resources.registry, 1, 1, 1, false,
                      MakePrimitive(mesh, material)).IsApplied());
    const RenderObject& object = scene.GetObject(0);
    ASSERT_TRUE(object.meshBatchesAuthoritative);
    ASSERT_EQ(object.meshBatches.size(), 1U);
    const MeshBatch& batch = object.meshBatches[0];
    EXPECT_EQ(batch.submeshIndex, 0U);
    EXPECT_EQ(batch.indexType, MeshUploadIndexType::UInt16);
    EXPECT_EQ(batch.geometry.indexOffset, 0U);
    EXPECT_EQ(batch.geometry.indexCount, 9U);
    EXPECT_EQ(batch.geometry.baseVertex, 0);
    EXPECT_EQ(batch.geometry.topology, MeshUploadPrimitiveTopology::TriangleStrip);

    const RenderDrawPacket packet = BuildLegacyMaterialDrawPacket(batch);
    EXPECT_EQ(packet.arguments.indexCount, 9U);
    EXPECT_EQ(packet.arguments.firstIndex, 0U);
    EXPECT_EQ(packet.arguments.vertexOffset, 0);
    EXPECT_EQ(packet.arguments.instanceCount, 1U);
    EXPECT_EQ(packet.arguments.firstInstance, 0U);
}

TEST(RenderSceneValidation, AcceptsQueuedRequiredMeshBeforeRegistryCreation)
{
    RegistryFixture resources;
    const RenderResourceHandle queued =
        resources.AddQueuedWithoutRegistry({41}, RenderResourceKind::Mesh);

    RenderScene scene;
    const RenderFrameApplyResult result = Apply(
        scene, resources.registry, 1, 1, 1, false, MakePrimitive(queued));

    ASSERT_TRUE(result.IsApplied());
    EXPECT_EQ(result.skippedDrawCount, 1U);
    EXPECT_FALSE(scene.GetObject(0).mesh.IsValid());
    EXPECT_FALSE(scene.GetObject(0).drawable);
}

TEST(RenderSceneValidation, SequenceGapPreservesRenderedTemporalHistory)
{
    RegistryFixture resources;
    const RenderResourceHandle mesh =
        resources.Add({6}, RenderResourceKind::Mesh, true);
    RenderScene scene;
    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      10,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {1, 0, 0}))
                    .IsApplied());
    scene.MarkAcceptedFrameRendered();

    RenderFrameApplyResult gap = Apply(scene,
                                       resources.registry,
                                       13,
                                       1,
                                       1,
                                       false,
                                       MakePrimitive(mesh, {}, {3, 0, 0}));
    ASSERT_TRUE(gap.IsApplied());
    EXPECT_EQ(gap.sequenceGap, 2U);
    EXPECT_FALSE(gap.temporalHistoryReset);
    EXPECT_EQ(scene.GetObject(0).previousWorldMatrixValid, 1U);
    EXPECT_EQ(Vec3(scene.GetObject(0).previousWorldMatrix[3]),
              (Vec3{1, 0, 0}));
}

TEST(RenderSceneValidation, ResetsHistoryForEachContinuityBoundary)
{
    RegistryFixture resources;
    const RenderResourceHandle mesh =
        resources.Add({7}, RenderResourceKind::Mesh, true);

    const auto verifyReset = [&](uint64 worldRevision,
                                 uint64 temporalEpoch,
                                 bool discontinuity,
                                 bool surfaceChange)
    {
        RenderScene scene;
        scene.SetSurfaceCompatibilityKey(1);
        EXPECT_TRUE(Apply(scene,
                          resources.registry,
                          1,
                          1,
                          1,
                          false,
                          MakePrimitive(mesh))
                        .IsApplied());
        scene.MarkAcceptedFrameRendered();
        if (surfaceChange)
        {
            scene.SetSurfaceCompatibilityKey(2);
        }
        const RenderFrameApplyResult result = Apply(
            scene,
            resources.registry,
            2,
            worldRevision,
            temporalEpoch,
            discontinuity,
            MakePrimitive(mesh, {}, {2, 0, 0}));
        EXPECT_TRUE(result.IsApplied());
        EXPECT_TRUE(result.temporalHistoryReset);
        EXPECT_EQ(scene.GetObject(0).previousWorldMatrixValid, 0U);
    };

    verifyReset(2, 1, false, false);
    verifyReset(1, 2, false, false);
    verifyReset(1, 1, true, false);
    verifyReset(1, 1, false, true);
}

TEST(RenderSceneValidation, ReplacedOrRejectedPacketDoesNotAdvancePreviousState)
{
    RegistryFixture resources;
    const RenderResourceHandle mesh =
        resources.Add({8}, RenderResourceKind::Mesh, true);
    RenderScene scene;
    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      20,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {1, 0, 0}))
                    .IsApplied());
    scene.MarkAcceptedFrameRendered();
    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      21,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {2, 0, 0}))
                    .IsApplied());
    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      22,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {3, 0, 0}))
                    .IsApplied());
    EXPECT_EQ(Vec3(scene.GetObject(0).previousWorldMatrix[3]),
              (Vec3{1, 0, 0}));
    EXPECT_EQ(scene.GetLastRenderedFrameSequence(), 20U);

    const RenderResourceHandle stale{mesh.slot, mesh.generation + 1};
    EXPECT_EQ(Apply(scene,
                    resources.registry,
                    23,
                    1,
                    1,
                    false,
                    MakePrimitive(stale))
                  .code,
              RenderFrameApplyCode::StaleRequiredHandle);
    EXPECT_EQ(scene.GetLastRenderedFrameSequence(), 20U);
    EXPECT_EQ(scene.GetAcceptedHeader().sequence, 22U);
}

TEST(RenderSceneValidation, StampsExactResourceClosureTransactionally)
{
    RegistryFixture resources;
    const RenderResourceHandle texture =
        resources.Add({20}, RenderResourceKind::Texture, true);
    const RenderResourceHandle material = resources.Add(
        {21}, RenderResourceKind::Material, true, {texture});

    GPUCompletionToken completion;
    ASSERT_TRUE(InsertGPUCompletionPoint(
        completion,
        GPUCompletionPoint{GPUQueueDomain::Graphics, 17}));
    const std::vector<RenderResourceHandle> roots{material};
    ASSERT_TRUE(resources.registry.MergeLastUseClosure(roots, completion));
    EXPECT_EQ(resources.registry.GetLastUse(material).count, 1U);
    EXPECT_EQ(resources.registry.GetLastUse(material).points[0].value, 17U);
    EXPECT_EQ(resources.registry.GetLastUse(texture).points[0].value, 17U);

    const std::vector<RenderResourceHandle> staleRoots{
        RenderResourceHandle{material.slot, material.generation + 1}};
    GPUCompletionToken later;
    ASSERT_TRUE(InsertGPUCompletionPoint(
        later,
        GPUCompletionPoint{GPUQueueDomain::Graphics, 99}));
    EXPECT_FALSE(resources.registry.MergeLastUseClosure(staleRoots, later));
    EXPECT_EQ(resources.registry.GetLastUse(material).points[0].value, 17U);
    EXPECT_EQ(resources.registry.GetLastUse(texture).points[0].value, 17U);
}

TEST(RenderSceneValidation, RejectsDuplicateNonzeroObjectIdsWithoutCacheOrSceneMutation)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 3;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {30}, createInfo, {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles}});
    RenderScene scene;
    ASSERT_TRUE(Apply(scene,
                      resources.registry,
                      1,
                      1,
                      1,
                      false,
                      MakePrimitive(mesh, {}, {1.0F, 0.0F, 0.0F}))
                    .IsApplied());
    const RenderDrawPacketCacheStats beforeCache =
        scene.GetDrawPacketCacheStats();
    ASSERT_EQ(scene.GetAcceptedHeader().sequence, 1U);
    ASSERT_EQ(scene.GetObjectCount(), 1U);

    RenderPrimitiveSnapshot first = MakePrimitive(
        mesh, {}, {2.0F, 0.0F, 0.0F});
    RenderPrimitiveSnapshot duplicate = MakePrimitive(
        mesh, {}, {3.0F, 0.0F, 0.0F});
    first.objectId = 99;
    duplicate.objectId = 99;
    std::unique_ptr<const RenderFramePacket> packet = MakePacket(
        2, 1, 1, false, {std::move(first), std::move(duplicate)});
    ASSERT_NE(packet, nullptr);

    EXPECT_EQ(scene.ApplyFramePacket(*packet, resources.registry).code,
              RenderFrameApplyCode::InvalidPacket);
    EXPECT_EQ(scene.GetAcceptedHeader().sequence, 1U);
    EXPECT_EQ(scene.GetObjectCount(), 1U);
    EXPECT_EQ(Vec3(scene.GetObject(0).worldMatrix[3]),
              (Vec3{1.0F, 0.0F, 0.0F}));
    const RenderDrawPacketCacheStats afterCache =
        scene.GetDrawPacketCacheStats();
    EXPECT_EQ(afterCache.entryCount, beforeCache.entryCount);
    EXPECT_EQ(afterCache.resolveCount, beforeCache.resolveCount);
    EXPECT_EQ(afterCache.entryCreationCount, beforeCache.entryCreationCount);
}
