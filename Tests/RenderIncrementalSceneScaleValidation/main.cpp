#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/RenderSceneDatabase.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderSceneUpdate.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Runtime/RenderResourceGateway.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    RVX::uint64 ElapsedMicroseconds(Clock::time_point begin,
                                    Clock::time_point end)
    {
        return static_cast<RVX::uint64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                end - begin)
                .count());
    }

    constexpr RVX::uint64 PackHandle(RVX::uint32 index,
                                     RVX::uint32 generation)
    {
        return (static_cast<RVX::uint64>(generation) << 32U) |
               (static_cast<RVX::uint64>(index) + 1U);
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

        RVX::RenderResourceHandle AddReady(
            RVX::AssetId asset,
            RVX::RenderResourceKind kind,
            bool meshMetadata)
        {
            const RVX::RenderResourceReserveResult reserved =
                gateway.ReserveResource(asset, kind);
            EXPECT_EQ(reserved.code, RVX::RenderResourceReserveCode::Reserved);
            const RVX::RenderResourceHandle handle = reserved.handle;
            const RVX::PackedRenderResourceStatus reservedStatus{
                handle.generation,
                RVX::RenderResourcePublicState::Reserved,
                RVX::RenderResourceFailureCode::None};
            RVX::PackedRenderResourceStatus queued = reservedStatus;
            queued.state = RVX::RenderResourcePublicState::UploadQueued;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle,
                reservedStatus,
                queued,
                RVX::RenderStatusWriter::Update));
            RVX::PackedRenderResourceStatus uploading = queued;
            uploading.state = RVX::RenderResourcePublicState::Uploading;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle,
                queued,
                uploading,
                RVX::RenderStatusWriter::Render));
            EXPECT_TRUE(registry.BeginPending(handle, kind, {}));
            if (meshMetadata)
            {
                RVX::MeshUploadCreateInfo createInfo;
                createInfo.indexCount = 3;
                EXPECT_TRUE(registry.SetPendingMeshMetadata(
                    handle,
                    createInfo,
                    {{0,
                      3,
                      0,
                      RVX::MeshUploadPrimitiveTopology::Triangles}}));
            }
            EXPECT_TRUE(registry.Commit(handle));
            RVX::PackedRenderResourceStatus ready = uploading;
            ready.state = RVX::RenderResourcePublicState::GPUReady;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle,
                uploading,
                ready,
                RVX::RenderStatusWriter::Render));
            return handle;
        }

        static RVX::RenderTransportConfig MakeConfig()
        {
            RVX::RenderTransportConfig config;
            config.statusSlotCapacity = 1024;
            config.uploadRequestCapacity = 8;
            config.uploadByteCapacity = 1024;
            return config;
        }

        RVX::RenderResourceGateway gateway;
        RVX::RenderRetirementQueue retirement;
        RVX::RenderResourceRegistry registry;
    };

    RVX::RenderPrimitiveSnapshot MakePrimitive(
        RVX::uint64 objectId,
        RVX::RenderResourceHandle mesh,
        RVX::RenderResourceHandle material,
        const RVX::Vec3& position)
    {
        RVX::RenderPrimitiveSnapshot primitive;
        primitive.objectId = objectId;
        primitive.mesh = mesh;
        primitive.material = material;
        primitive.worldTransform = RVX::Mat4Identity();
        primitive.worldTransform[3] = RVX::Vec4(position, 1.0F);
        primitive.previousWorldTransform = primitive.worldTransform;
        primitive.boundsMin = position - RVX::Vec3(1.0F);
        primitive.boundsMax = position + RVX::Vec3(1.0F);
        primitive.flags = 0x7U;
        primitive.layerMask = ~0U;
        primitive.sortKey = objectId;
        return primitive;
    }

    std::unique_ptr<const RVX::RenderFramePacketV5> MakeFrame(
        RVX::uint64 sequence,
        RVX::uint64 requiredRevision,
        RVX::uint64 worldRevision = 17)
    {
        RVX::RenderFrameHeaderV5 header;
        header.sequence = sequence;
        header.requiredSceneRevision = requiredRevision;
        header.worldRevision = worldRevision;
        header.temporalEpoch = 1;
        RVX::RenderViewSnapshot view;
        view.viewportWidth = 1280;
        view.viewportHeight = 720;
        RVX::RenderExtractionDiagnostics diagnostics;
        diagnostics.code = RVX::RenderExtractionCode::Complete;
        diagnostics.complete = true;
        return RVX::RenderFramePacketV5::Create(
            header,
            view,
            RVX::RenderFrameSettings{},
            RVX::RenderFrameCaptureRequest{},
            diagnostics);
    }

    void SettleTemporalHistory(
        RVX::RenderScene& scene,
        const RVX::RenderSceneDatabase& database,
        const RVX::RenderResourceRegistry& registry,
        RVX::uint64 sequence)
    {
        const auto frame = MakeFrame(sequence, database.GetRevision());
        ASSERT_NE(frame, nullptr);
        const RVX::RenderFrameApplyResult result =
            scene.ApplyFrameV5(*frame, database, registry);
        ASSERT_TRUE(result.IsApplied());
        scene.MarkAcceptedFrameRendered();
    }
} // namespace

TEST(RenderIncrementalSceneScaleValidation,
     StaticAndOnePercentDirtyRetainedWorkUsesExactChangedObjects)
{
    constexpr std::array<RVX::uint32, 3> objectCounts = {
        1'000,
        10'000,
        100'000,
    };
    RegistryFixture resources;
    const RVX::RenderResourceHandle mesh = resources.AddReady(
        {9100}, RVX::RenderResourceKind::Mesh, true);
    const RVX::RenderResourceHandle material = resources.AddReady(
        {9101}, RVX::RenderResourceKind::Material, false);

    for (RVX::uint32 objectCount : objectCounts)
    {
        SCOPED_TRACE(::testing::Message()
                     << "objectCount=" << objectCount);
        RVX::RenderSceneMutationAccumulator reset;
        reset.Begin(0, true);
        for (RVX::uint32 index = 0; index < objectCount; ++index)
        {
            ASSERT_TRUE(reset.UpsertPrimitive(
                MakePrimitive(
                    static_cast<RVX::uint64>(index) + 1U,
                    mesh,
                    material,
                    {static_cast<RVX::float32>(index), 0.0F, 0.0F}),
                true));
        }

        RVX::RenderSceneDatabase database;
        const Clock::time_point databaseFullBegin = Clock::now();
        ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());
        const Clock::time_point databaseFullEnd = Clock::now();
        RVX::RenderScene scene;
        const auto initialFrame = MakeFrame(1, 1);
        ASSERT_NE(initialFrame, nullptr);
        const Clock::time_point retainedFullBegin = Clock::now();
        ASSERT_TRUE(scene.ApplyFrameV5(
            *initialFrame, database, resources.registry).IsApplied());
        const Clock::time_point retainedFullEnd = Clock::now();
        EXPECT_EQ(scene.GetRetainedStats().lastRebuiltObjectCount,
                  objectCount);
        scene.MarkAcceptedFrameRendered();
        SettleTemporalHistory(scene, database, resources.registry, 2);

        const auto staticFrame = MakeFrame(3, 1);
        ASSERT_NE(staticFrame, nullptr);
        const Clock::time_point staticBegin = Clock::now();
        const RVX::RenderFrameApplyResult staticResult =
            scene.ApplyFrameV5(*staticFrame, database, resources.registry);
        const Clock::time_point staticEnd = Clock::now();
        ASSERT_TRUE(staticResult.IsApplied());
        EXPECT_FALSE(staticResult.sceneMutated);
        EXPECT_EQ(scene.GetRetainedStats().lastRebuiltObjectCount, 0U);
        EXPECT_TRUE(scene.GetGPUSceneChangedObjectIds().empty());
        scene.MarkAcceptedFrameRendered();

        const RVX::uint32 dirtyCount = objectCount / 100U;
        RVX::RenderSceneMutationAccumulator update;
        update.Begin(1);
        for (RVX::uint32 index = 0; index < objectCount; index += 100U)
        {
            ASSERT_TRUE(update.UpsertPrimitive(MakePrimitive(
                static_cast<RVX::uint64>(index) + 1U,
                mesh,
                material,
                {static_cast<RVX::float32>(index), 1.0F, 0.0F})));
        }
        const Clock::time_point databaseDirtyBegin = Clock::now();
        ASSERT_TRUE(database.Apply(update.Build(2)).IsApplied());
        const Clock::time_point databaseDirtyEnd = Clock::now();

        const auto dirtyFrame = MakeFrame(4, 2);
        ASSERT_NE(dirtyFrame, nullptr);
        const Clock::time_point retainedDirtyBegin = Clock::now();
        const RVX::RenderFrameApplyResult dirtyResult =
            scene.ApplyFrameV5(*dirtyFrame, database, resources.registry);
        const Clock::time_point retainedDirtyEnd = Clock::now();
        ASSERT_TRUE(dirtyResult.IsApplied());
        EXPECT_TRUE(dirtyResult.sceneMutated);
        EXPECT_EQ(scene.GetObjectCount(), objectCount);
        EXPECT_EQ(scene.GetDrawCount(), objectCount);
        EXPECT_EQ(scene.GetRetainedStats().lastRebuiltObjectCount,
                  dirtyCount);
        EXPECT_EQ(scene.GetRetainedStats().lastRemovedObjectCount, 0U);
        EXPECT_EQ(scene.GetGPUSceneChangedObjectIds().size(), dirtyCount);
        EXPECT_TRUE(scene.GetGPUSceneRemovedObjectIds().empty());

        const std::string scale = std::to_string(objectCount);
        RecordProperty(
            "render_scene_" + scale + "_database_full_us",
            ElapsedMicroseconds(databaseFullBegin, databaseFullEnd));
        RecordProperty(
            "render_scene_" + scale + "_retained_full_us",
            ElapsedMicroseconds(retainedFullBegin, retainedFullEnd));
        RecordProperty(
            "render_scene_" + scale + "_static_us",
            ElapsedMicroseconds(staticBegin, staticEnd));
        RecordProperty(
            "render_scene_" + scale + "_database_one_percent_us",
            ElapsedMicroseconds(databaseDirtyBegin, databaseDirtyEnd));
        RecordProperty(
            "render_scene_" + scale + "_retained_one_percent_us",
            ElapsedMicroseconds(retainedDirtyBegin, retainedDirtyEnd));
    }
}

TEST(RenderIncrementalSceneScaleValidation,
     HighChurnAndMultipleMaterialsKeepGenerationSafeIncrementalIdentity)
{
    constexpr RVX::uint32 objectCount = 10'000;
    constexpr RVX::uint32 replacedCount = 1'000;
    constexpr RVX::uint32 updatedCount = 1'000;
    RegistryFixture resources;
    const RVX::RenderResourceHandle mesh = resources.AddReady(
        {9200}, RVX::RenderResourceKind::Mesh, true);
    const RVX::RenderResourceHandle materialA = resources.AddReady(
        {9201}, RVX::RenderResourceKind::Material, false);
    const RVX::RenderResourceHandle materialB = resources.AddReady(
        {9202}, RVX::RenderResourceKind::Material, false);

    RVX::RenderSceneMutationAccumulator reset;
    reset.Begin(0, true);
    for (RVX::uint32 index = 0; index < objectCount; ++index)
    {
        ASSERT_TRUE(reset.UpsertPrimitive(
            MakePrimitive(
                PackHandle(index, 1),
                mesh,
                index % 2U == 0U ? materialA : materialB,
                {static_cast<RVX::float32>(index), 0.0F, 0.0F}),
            true));
    }
    RVX::RenderSceneDatabase database;
    ASSERT_TRUE(database.Apply(reset.Build(1)).IsApplied());
    RVX::RenderScene scene;
    const auto initialFrame = MakeFrame(1, 1);
    ASSERT_NE(initialFrame, nullptr);
    ASSERT_TRUE(scene.ApplyFrameV5(
        *initialFrame, database, resources.registry).IsApplied());
    scene.MarkAcceptedFrameRendered();
    SettleTemporalHistory(scene, database, resources.registry, 2);

    RVX::RenderSceneMutationAccumulator churn;
    churn.Begin(1);
    for (RVX::uint32 index = 0; index < replacedCount; ++index)
    {
        ASSERT_TRUE(churn.RemovePrimitive(PackHandle(index, 1)));
        ASSERT_TRUE(churn.UpsertPrimitive(
            MakePrimitive(
                PackHandle(index, 2),
                mesh,
                materialB,
                {static_cast<RVX::float32>(index), 2.0F, 0.0F}),
            true));
    }
    for (RVX::uint32 index = replacedCount;
         index < replacedCount + updatedCount;
         ++index)
    {
        ASSERT_TRUE(churn.UpsertPrimitive(MakePrimitive(
            PackHandle(index, 1),
            mesh,
            index % 2U == 0U ? materialB : materialA,
            {static_cast<RVX::float32>(index), 3.0F, 0.0F})));
    }
    ASSERT_TRUE(database.Apply(churn.Build(2)).IsApplied());

    const auto churnFrame = MakeFrame(3, 2);
    ASSERT_NE(churnFrame, nullptr);
    const RVX::RenderFrameApplyResult result =
        scene.ApplyFrameV5(*churnFrame, database, resources.registry);
    ASSERT_TRUE(result.IsApplied());
    EXPECT_EQ(scene.GetObjectCount(), objectCount);
    EXPECT_EQ(scene.GetRetainedStats().lastRebuiltObjectCount,
              replacedCount + updatedCount);
    EXPECT_EQ(scene.GetRetainedStats().lastRemovedObjectCount,
              replacedCount);
    EXPECT_EQ(scene.GetGPUSceneChangedObjectIds().size(),
              replacedCount + updatedCount);
    EXPECT_EQ(scene.GetGPUSceneRemovedObjectIds().size(), replacedCount);
    EXPECT_EQ(scene.FindObject(PackHandle(0, 1)), nullptr);
    ASSERT_NE(scene.FindObject(PackHandle(0, 2)), nullptr);
    EXPECT_EQ(scene.FindObject(PackHandle(0, 2))->material, materialB);
}
