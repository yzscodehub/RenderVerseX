#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/RenderDrawPacket.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RenderExtraction/RenderFramePacketBuilder.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Runtime/RenderResourceGateway.h"

#include <algorithm>
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
