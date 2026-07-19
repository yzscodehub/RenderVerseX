#include "Render/Renderer/RenderScene.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RenderExtraction/RenderFramePacketBuilder.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Runtime/RenderResourceGateway.h"

#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

using namespace RVX;

namespace
{
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
