#include "GPUScene/GPUSceneUpdate.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderDrawPacket.h"
#include "Render/Renderer/RenderSceneDatabase.h"
#include "Render/Renderer/SceneRenderer.h"
#include "Render/Renderer/RenderScene.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderSceneUpdate.h"
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
        static void SetGPUScenePublishAllocationFailure(
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

        void CompletePendingMesh(
            RenderResourceHandle handle,
            const MeshUploadCreateInfo& createInfo,
            const std::vector<MeshUploadSubmesh>& submeshes)
        {
            const RenderResourceStatus status =
                gateway.GetStatusTable().Query(handle);
            ASSERT_EQ(status.code, RenderResourceStatusCode::Current);
            ASSERT_EQ(status.state, RenderResourcePublicState::Uploading);
            ASSERT_TRUE(registry.SetPendingMeshMetadata(
                handle, createInfo, submeshes));
            ASSERT_TRUE(registry.Commit(handle));
            const PackedRenderResourceStatus uploading{
                handle.generation,
                RenderResourcePublicState::Uploading,
                RenderResourceFailureCode::None};
            PackedRenderResourceStatus ready = uploading;
            ready.state = RenderResourcePublicState::GPUReady;
            ASSERT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle,
                uploading,
                ready,
                RenderStatusWriter::Render));
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

    ParticleRenderSnapshotItem MakeParticleFeature()
    {
        ParticleRenderSnapshotItem particle;
        particle.instanceId = 7;
        particle.systemId = 8;
        particle.systemName = "packet-owned-feature";
        particle.worldBounds = AABB(Vec3{-1.0f}, Vec3{1.0f});
        return particle;
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

    struct V5FrameInput
    {
        RenderSceneDatabase database;
        std::unique_ptr<const RenderFramePacketV5> frame;
    };

    V5FrameInput MakeFrame(
        uint64 sequence,
        uint64 worldRevision,
        uint64 temporalEpoch,
        bool discontinuity,
        std::vector<RenderPrimitiveSnapshot> primitives)
    {
        V5FrameInput input;
        RenderSceneMutationAccumulator accumulator;
        accumulator.Begin(0, true);
        for (RenderPrimitiveSnapshot& primitive : primitives)
        {
            EXPECT_TRUE(accumulator.UpsertPrimitive(
                std::move(primitive), true));
        }
        EXPECT_TRUE(accumulator.UpsertParticle(MakeParticleFeature(), true));
        accumulator.UpsertSky({});
        accumulator.UpsertEnvironment({});
        EXPECT_TRUE(input.database.Apply(
            accumulator.Build(worldRevision)).IsApplied());

        RenderFrameHeaderV5 header;
        header.sequence = sequence;
        header.requiredSceneRevision = worldRevision;
        header.worldRevision = worldRevision;
        header.temporalEpoch = temporalEpoch;
        header.explicitDiscontinuity = discontinuity;

        RenderViewSnapshot view;
        view.viewportWidth = 1280;
        view.viewportHeight = 720;
        view.cameraPosition = {0.0f, 0.0f, 5.0f};
        RenderExtractionDiagnostics diagnostics;
        diagnostics.code = RenderExtractionCode::Complete;
        diagnostics.complete = true;
        input.frame = RenderFramePacketV5::Create(
            header,
            view,
            RenderFrameSettings{},
            RenderFrameCaptureRequest{},
            diagnostics);
        return input;
    }

    std::unique_ptr<const RenderFramePacketV5> MakeFramePacketV5(
        uint64 sequence,
        uint64 requiredSceneRevision,
        uint64 worldRevision = 1,
        uint64 temporalEpoch = 1,
        bool discontinuity = false)
    {
        RenderFrameHeaderV5 header;
        header.sequence = sequence;
        header.requiredSceneRevision = requiredSceneRevision;
        header.worldRevision = worldRevision;
        header.temporalEpoch = temporalEpoch;
        header.explicitDiscontinuity = discontinuity;
        RenderViewSnapshot view;
        view.viewportWidth = 1280;
        view.viewportHeight = 720;
        RenderExtractionDiagnostics diagnostics;
        diagnostics.code = RenderExtractionCode::Complete;
        diagnostics.complete = true;
        return RenderFramePacketV5::Create(
            header,
            view,
            RenderFrameSettings{},
            RenderFrameCaptureRequest{},
            diagnostics);
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
        V5FrameInput input = MakeFrame(
            sequence,
            worldRevision,
            temporalEpoch,
            discontinuity,
            {std::move(primitive)});
        EXPECT_NE(input.frame, nullptr);
        return scene.ApplyFrameV5(*input.frame, input.database, registry);
    }
} // namespace

TEST(RenderSceneValidation, AppliesTransactionallyAndOwnsPacketValues)
{
    RegistryFixture resources;
    const RenderResourceHandle mesh =
        resources.Add({1}, RenderResourceKind::Mesh, true);
    const RenderResourceHandle material =
        resources.Add({2}, RenderResourceKind::Material, true);
    V5FrameInput input =
        MakeFrame(10, 3, 4, false, {MakePrimitive(mesh, material, {2, 0, 0})});
    ASSERT_NE(input.frame, nullptr);

    RenderScene scene;
    RenderFrameApplyResult applied =
        scene.ApplyFrameV5(*input.frame, input.database, resources.registry);
    ASSERT_TRUE(applied.IsApplied());
    input.frame.reset();
    input.database.Clear();

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

TEST(RenderSceneValidation,
     StaticFramesDoNoRetainedWorkAndOnePercentDirtyRebuildsExactObjects)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 3;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {8100},
        createInfo,
        {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles}});

    constexpr uint32 objectCount = 100;
    RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    for (uint32 index = 0; index < objectCount; ++index)
    {
        RenderPrimitiveSnapshot primitive = MakePrimitive(
            mesh, {}, {static_cast<float32>(index), 0.0F, 0.0F});
        primitive.objectId = static_cast<uint64>(index) + 1U;
        ASSERT_TRUE(reset.UpsertPrimitive(std::move(primitive), true));
    }
    RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());

    RenderScene scene;
    const auto firstFrame = MakeFramePacketV5(1, 1, 7, 3);
    ASSERT_NE(firstFrame, nullptr);
    const RenderFrameApplyResult initial =
        scene.ApplyFrameV5(*firstFrame, database, resources.registry);
    ASSERT_TRUE(initial.IsApplied());
    ASSERT_TRUE(initial.sceneMutated);
    EXPECT_TRUE(scene.IsFullGPUSceneMutation());
    EXPECT_EQ(scene.GetRetainedStats().fullRebuildCount, 1U);
    EXPECT_EQ(scene.GetRetainedStats().lastRebuiltObjectCount, objectCount);
    EXPECT_EQ(scene.GetObjectCount(), objectCount);
    EXPECT_EQ(scene.GetDrawCount(), objectCount);
    scene.MarkAcceptedFrameRendered();

    // First reuse settles previous matrices once. The following reuse must be
    // a true static frame with no retained rebuild or GPU-scene mutation.
    const auto settleFrame = MakeFramePacketV5(2, 1, 7, 3);
    ASSERT_NE(settleFrame, nullptr);
    const RenderFrameApplyResult settled =
        scene.ApplyFrameV5(*settleFrame, database, resources.registry);
    ASSERT_TRUE(settled.IsApplied());
    ASSERT_TRUE(settled.sceneMutated);
    EXPECT_EQ(scene.GetGPUSceneChangedObjectIds().size(), objectCount);
    scene.MarkAcceptedFrameRendered();

    const RenderDrawPacketCacheStats beforeStatic =
        scene.GetDrawPacketCacheStats();
    const auto staticFrame = MakeFramePacketV5(3, 1, 7, 3);
    ASSERT_NE(staticFrame, nullptr);
    const RenderFrameApplyResult reused =
        scene.ApplyFrameV5(*staticFrame, database, resources.registry);
    ASSERT_TRUE(reused.IsApplied());
    EXPECT_FALSE(reused.sceneMutated);
    EXPECT_FALSE(scene.IsFullGPUSceneMutation());
    EXPECT_TRUE(scene.GetGPUSceneChangedObjectIds().empty());
    EXPECT_TRUE(scene.GetGPUSceneRemovedObjectIds().empty());
    EXPECT_EQ(scene.GetRetainedStats().staticReuseCount, 1U);
    EXPECT_EQ(scene.GetRetainedStats().lastRebuiltObjectCount, 0U);
    EXPECT_EQ(scene.GetDrawPacketCacheStats().packetBuildCount,
              beforeStatic.packetBuildCount);
    scene.MarkAcceptedFrameRendered();

    RenderPrimitiveSnapshot changed = MakePrimitive(
        mesh, {}, {25.0F, 4.0F, 0.0F});
    changed.objectId = 26;
    RenderSceneMutationAccumulator update;
    update.Begin(1);
    ASSERT_TRUE(update.UpsertPrimitive(std::move(changed)));
    ASSERT_TRUE(database.Apply(update.Build(2)).IsApplied());

    const RenderDrawPacketCacheStats beforeDirty =
        scene.GetDrawPacketCacheStats();
    const auto dirtyFrame = MakeFramePacketV5(4, 2, 7, 3);
    ASSERT_NE(dirtyFrame, nullptr);
    const RenderFrameApplyResult dirty =
        scene.ApplyFrameV5(*dirtyFrame, database, resources.registry);
    ASSERT_TRUE(dirty.IsApplied());
    EXPECT_TRUE(dirty.sceneMutated);
    EXPECT_EQ(scene.GetRetainedStats().incrementalUpdateCount, 1U);
    EXPECT_EQ(scene.GetRetainedStats().lastRebuiltObjectCount, 1U);
    EXPECT_EQ(scene.GetRetainedStats().lastRemovedObjectCount, 0U);
    EXPECT_EQ(scene.GetGPUSceneChangedObjectIds(),
              std::vector<uint64>({26U}));
    ASSERT_NE(scene.FindObject(26), nullptr);
    EXPECT_EQ(scene.FindObject(26)->objectRevision, 2U);
    EXPECT_EQ(Vec3(scene.FindObject(26)->worldMatrix[3]),
              (Vec3{25.0F, 4.0F, 0.0F}));
    const RenderDrawPacketCacheStats afterDirty =
        scene.GetDrawPacketCacheStats();
    EXPECT_EQ(afterDirty.entryCount, objectCount);
    EXPECT_EQ(afterDirty.packetBuildCount,
              beforeDirty.packetBuildCount + 1U);
    EXPECT_EQ(afterDirty.GetInvalidationCount(
                  RenderDrawPacketCacheInvalidationReason::ObjectRevisionChanged),
              beforeDirty.GetInvalidationCount(
                  RenderDrawPacketCacheInvalidationReason::ObjectRevisionChanged) +
                  1U);
}

TEST(RenderSceneValidation, GPUScenePublicationFailureCannotRejectAnAppliedFrame)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 3;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {700}, createInfo, {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles}});
    SceneRenderer renderer;

    V5FrameInput first = MakeFrame(
        101, 1, 1, false, {MakePrimitive(mesh)});
    ASSERT_NE(first.frame, nullptr);
    ASSERT_TRUE(renderer.ApplyFrameV5(
        *first.frame, first.database, resources.registry).IsApplied());
    renderer.GetRenderScene().MarkAcceptedFrameRendered();
    const GPUScenePublicationStats before =
        renderer.GetGPUScenePublicationStats();
    ASSERT_EQ(before.failureReason, GPUScenePublicationFailureReason::None);
    ASSERT_EQ(before.publishedObjectCount, 1U);

    SceneRendererTestAccess::SetGPUScenePublishAllocationFailure(renderer, true);
    V5FrameInput second = MakeFrame(
        102, 2, 1, false, {MakePrimitive(mesh)});
    second.frame = MakeFramePacketV5(102, 2, 1, 1, false);
    ASSERT_NE(second.frame, nullptr);
    const RenderFrameApplyResult applied =
        renderer.ApplyFrameV5(
            *second.frame, second.database, resources.registry);

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

    // The direct path may render the accepted scene even though publication
    // failed. A later static frame must therefore retry from the complete
    // retained scene instead of waiting indefinitely for another mutation.
    renderer.GetRenderScene().MarkAcceptedFrameRendered();
    SceneRendererTestAccess::SetGPUScenePublishAllocationFailure(renderer, false);
    const std::unique_ptr<const RenderFramePacketV5> retryFrame =
        MakeFramePacketV5(103, 2, 1, 1, false);
    const RenderFrameApplyResult retried = renderer.ApplyFrameV5(
        *retryFrame, second.database, resources.registry);
    ASSERT_TRUE(retried.IsApplied());
    EXPECT_FALSE(retried.sceneMutated);
    const GPUScenePublicationStats& recovered =
        renderer.GetGPUScenePublicationStats();
    EXPECT_EQ(recovered.failureReason,
              GPUScenePublicationFailureReason::None);
    EXPECT_TRUE(recovered.complete);
    EXPECT_EQ(recovered.committedSourceSequence, 103U);
    EXPECT_EQ(recovered.publishedObjectCount, 1U);
}

TEST(RenderSceneValidation,
     StaticFrameAdvancesGPUSceneIdentityWithoutVersionOrObjectMutation)
{
    RegistryFixture resources;
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 3;
    const RenderResourceHandle mesh = resources.AddMeshWithMetadata(
        {702}, createInfo, {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles}});
    SceneRenderer renderer;

    V5FrameInput initial = MakeFrame(201, 1, 1, false, {MakePrimitive(mesh)});
    ASSERT_NE(initial.frame, nullptr);
    ASSERT_TRUE(renderer.ApplyFrameV5(
        *initial.frame, initial.database, resources.registry).IsApplied());
    renderer.GetRenderScene().MarkAcceptedFrameRendered();

    const std::unique_ptr<const RenderFramePacketV5> settleFrame =
        MakeFramePacketV5(202, 1, 1, 1, false);
    ASSERT_TRUE(renderer.ApplyFrameV5(
        *settleFrame, initial.database, resources.registry).IsApplied());
    renderer.GetRenderScene().MarkAcceptedFrameRendered();
    const GPUScenePublicationStats beforeStatic =
        renderer.GetGPUScenePublicationStats();
    ASSERT_TRUE(beforeStatic.complete);
    ASSERT_NE(beforeStatic.committedVersion, 0U);

    const std::unique_ptr<const RenderFramePacketV5> staticFrame =
        MakeFramePacketV5(203, 1, 1, 1, false);
    const RenderFrameApplyResult staticApplied = renderer.ApplyFrameV5(
        *staticFrame, initial.database, resources.registry);
    ASSERT_TRUE(staticApplied.IsApplied());
    EXPECT_FALSE(staticApplied.sceneMutated);

    const GPUScenePublicationStats& afterStatic =
        renderer.GetGPUScenePublicationStats();
    EXPECT_EQ(afterStatic.committedVersion,
              beforeStatic.committedVersion);
    EXPECT_EQ(afterStatic.committedSourceSequence, 203U);
    EXPECT_EQ(afterStatic.addCount, 0U);
    EXPECT_EQ(afterStatic.updateCount, 0U);
    EXPECT_EQ(afterStatic.removeCount, 0U);
    EXPECT_TRUE(afterStatic.complete);
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
    V5FrameInput input = MakeFrame(
        1, 1, 1, false, {primitive});
    ASSERT_NE(input.frame, nullptr);

    RenderScene scene;
    ASSERT_TRUE(scene.ApplyFrameV5(
        *input.frame, input.database, resources.registry).IsApplied());
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
    V5FrameInput input = MakeFrame(
        2, 1, 1, false, {std::move(incomplete)});
    ASSERT_NE(input.frame, nullptr);
    EXPECT_EQ(scene.ApplyFrameV5(
                  *input.frame, input.database, resources.registry).code,
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
    V5FrameInput second = MakeFrame(
        2, 1, 1, false, {std::move(leading), std::move(retained)});
    ASSERT_NE(second.frame, nullptr);
    ASSERT_TRUE(scene.ApplyFrameV5(
        *second.frame, second.database, resources.registry).IsApplied());

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

    V5FrameInput unsupported =
        MakeFrame(11, 1, 1, false, {MakePrimitive(mesh, {}, {2, 0, 0})});
    ASSERT_NE(unsupported.frame, nullptr);
    const_cast<RenderFrameHeaderV5&>(
        unsupported.frame->GetHeader()).schemaVersion += 1;
    EXPECT_EQ(scene.ApplyFrameV5(
                  *unsupported.frame,
                  unsupported.database,
                  resources.registry).code,
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
     StaticDatabasePromotesFallbackWhenWatchedResourceBecomesReady)
{
    RegistryFixture resources;
    const RenderResourceHandle pending =
        resources.Add({41}, RenderResourceKind::Mesh, false);
    MeshUploadCreateInfo createInfo;
    createInfo.indexCount = 3;
    const std::vector<MeshUploadSubmesh> submeshes = {
        {0, 3, 0, MeshUploadPrimitiveTopology::Triangles}};
    const RenderResourceHandle fallback = resources.AddMeshWithMetadata(
        {42}, createInfo, submeshes);
    RenderPrimitiveSnapshot primitive = MakePrimitive(pending);
    primitive.fallbackMesh = fallback;

    V5FrameInput input = MakeFrame(1, 1, 1, false, {primitive});
    RenderScene scene;
    ASSERT_TRUE(scene.ApplyFrameV5(
        *input.frame, input.database, resources.registry).IsApplied());
    ASSERT_EQ(scene.GetObject(0).mesh, fallback);
    scene.MarkAcceptedFrameRendered();
    const uint64 fullRebuildsBefore =
        scene.GetRetainedStats().fullRebuildCount;

    resources.CompletePendingMesh(pending, createInfo, submeshes);
    const std::unique_ptr<const RenderFramePacketV5> staticSceneFrame =
        MakeFramePacketV5(2, 1, 1, 1, false);
    const RenderFrameApplyResult promoted = scene.ApplyFrameV5(
        *staticSceneFrame, input.database, resources.registry);
    ASSERT_TRUE(promoted.IsApplied());
    EXPECT_TRUE(promoted.sceneMutated);
    EXPECT_EQ(scene.GetObject(0).mesh, pending);
    EXPECT_EQ(scene.GetRetainedStats().fullRebuildCount,
              fullRebuildsBefore + 1U);
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

TEST(RenderSceneValidation,
     PersistentDatabaseRejectsDuplicateObjectIdsWithoutSceneMutation)
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
    RenderSceneUpdateBatch invalidBatch;
    invalidBatch.targetSceneRevision = 2;
    invalidBatch.fullReset = true;
    invalidBatch.primitives.push_back(
        {RenderSceneMutationOperation::Upsert, 99, std::move(first)});
    invalidBatch.primitives.push_back(
        {RenderSceneMutationOperation::Upsert, 99, std::move(duplicate)});
    RenderSceneDatabase invalidDatabase;
    EXPECT_EQ(invalidDatabase.Apply(invalidBatch).code,
              RenderSceneUpdateApplyCode::InvalidMutation);
    EXPECT_EQ(invalidDatabase.GetRevision(), 0U);

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
