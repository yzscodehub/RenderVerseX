#include "GPUScene/GPUSceneDatabase.h"
#include "GPUScene/GPUSceneUpdate.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Visibility/RenderVisibility.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Runtime/RenderResourceGateway.h"

#include <gtest/gtest.h>

#include <limits>
#include <type_traits>
#include <vector>

using namespace RVX;

namespace
{
    static_assert(!std::is_convertible_v<GPUSceneBoundsRef, GPUScenePrimitiveRef>);
    static_assert(!std::is_convertible_v<GPUSceneTransformRef, GPUScenePrimitiveRef>);
    static_assert(!std::is_convertible_v<GPUSceneMaterialRef, GPUScenePrimitiveRef>);
    static_assert(!std::is_convertible_v<GPUSceneGeometryRef, GPUScenePrimitiveRef>);
    static_assert(!std::is_convertible_v<GPUSceneDrawRef, GPUScenePrimitiveRef>);
    static_assert(!std::is_move_constructible_v<GPUSceneDatabase>);
    static_assert(!std::is_move_assignable_v<GPUSceneDatabase>);
    static_assert(std::is_trivially_copyable_v<GPUSceneDiagnostics>);
    static_assert(std::is_trivially_copyable_v<GPUSceneTableDiagnostics>);
    static_assert(std::is_trivially_copyable_v<GPUSceneSlotLifecycleDiagnostics>);
    static_assert(std::is_trivially_copyable_v<GPUSceneTimingDiagnostics>);

    GPUSceneMatrix4x4 MakeAffineMatrix(float32 marker)
    {
        GPUSceneMatrix4x4 matrix;
        matrix.rows[0] = {marker, marker + 1.0F, marker + 2.0F, marker + 3.0F};
        matrix.rows[1] = {marker + 4.0F, marker + 5.0F, marker + 6.0F, marker + 7.0F};
        matrix.rows[2] = {marker + 8.0F, marker + 9.0F, marker + 10.0F, marker + 11.0F};
        matrix.rows[3] = {0.0F, 0.0F, 0.0F, 1.0F};
        return matrix;
    }

    GPUSceneObjectData MakeObject(uint64 objectId, float32 marker, uint32 drawCount = 1)
    {
        GPUSceneObjectData object;
        object.objectId = objectId;
        object.bounds.minimum = {marker, marker + 1.0F, marker + 2.0F, 0.0F};
        object.bounds.maximum = {marker + 3.0F, marker + 4.0F, marker + 5.0F, 0.0F};
        object.bounds.sphere = {marker + 6.0F, marker + 7.0F, marker + 8.0F, marker + 9.0F};
        object.bounds.boundsFlags = static_cast<uint32>(GPUSceneBoundsFlags::ForceVisible);
        object.transform.worldFromLocal = PackGPUSceneAffineMatrix(MakeAffineMatrix(marker));
        object.transform.previousWorldFromLocal =
            PackGPUSceneAffineMatrix(MakeAffineMatrix(marker - 1.0F));
        object.transform.normalFromLocal =
            PackGPUSceneAffineMatrix(MakeAffineMatrix(marker + 20.0F));
        object.transform.transformFlags =
            static_cast<uint32>(GPUSceneTransformFlags::PreviousWorldFromLocalValid) |
            static_cast<uint32>(GPUSceneTransformFlags::NormalFromLocalValid);
        object.primitiveFlags = static_cast<uint32>(marker + 5.0F);
        object.layerMask = 0x80000000U | static_cast<uint32>(marker);
        object.sortKey = (0x12345678ULL << 32U) | static_cast<uint32>(marker + 6.0F);
        object.draws.resize(drawCount);

        for (uint32 index = 0; index < drawCount; ++index)
        {
            GPUSceneDrawData& drawData = object.draws[index];
            drawData.material.resourceSlot = static_cast<uint32>(100U + index);
            drawData.material.resourceGeneration = static_cast<uint32>(10U + index);
            drawData.material.materialId = PackGPUSceneUint64(objectId * 10U + index);
            drawData.material.baseColor = {marker, marker + 1.0F, marker + 2.0F, 1.0F};
            drawData.material.roughness = marker + 3.0F;
            drawData.geometry.resourceSlot = static_cast<uint32>(200U + index);
            drawData.geometry.resourceGeneration = static_cast<uint32>(20U + index);
            drawData.geometry.geometryId = PackGPUSceneUint64(objectId * 100U + index);
            drawData.geometry.submeshIndex = index;
            drawData.geometry.firstIndex = index * 3U;
            drawData.geometry.vertexOffset = static_cast<int32>(index);
            drawData.geometry.indexCount = 36U + index;
            drawData.draw.indexCount = 36U + index;
            drawData.draw.instanceCount = 1;
            drawData.draw.firstIndex = index * 3U;
            drawData.draw.vertexOffset = static_cast<int32>(index);
            drawData.draw.passMask = 1U << index;
            drawData.draw.materialVariant = 10U + index;
            drawData.draw.pipelineKey =
                PackGPUSceneUint64((objectId << 8U) | index);
        }

        // Commit owns all row headers and typed linkage, never caller-provided values.
        object.bounds.header.objectId = PackGPUSceneUint64(9999U);
        object.bounds.header.schemaVersion = 0;
        object.bounds.header.generation = 0;
        object.bounds.header.flags = 0;
        return object;
    }

    GPUScenePrimitiveRef AddSingleObject(GPUSceneDatabase& database, uint64 objectId)
    {
        GPUSceneTransaction transaction;
        transaction.Add(MakeObject(objectId, 1.0F));
        EXPECT_TRUE(database.Commit(transaction).Succeeded());

        const std::optional<GPUScenePrimitiveRef> primitive =
            database.FindPrimitive(objectId);
        EXPECT_TRUE(primitive.has_value());
        return primitive.value_or(GPUScenePrimitiveRef{});
    }

    class PublicationRegistryFixture final
    {
    public:
        PublicationRegistryFixture()
            : gateway(MakeConfig())
        {
            EXPECT_TRUE(registry.Initialize(&gateway.GetStatusTable(), &retirement));
        }

        ~PublicationRegistryFixture()
        {
            registry.Shutdown();
        }

        RenderResourceHandle AddReadyMesh(AssetId asset)
        {
            const RenderResourceReserveResult reserved =
                gateway.ReserveResource(asset, RenderResourceKind::Mesh);
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
            EXPECT_TRUE(registry.BeginPending(handle, RenderResourceKind::Mesh, {}));
            MeshUploadCreateInfo createInfo;
            createInfo.indexCount = 3;
            EXPECT_TRUE(registry.SetPendingMeshMetadata(
                handle,
                createInfo,
                {{0, 3, 0, MeshUploadPrimitiveTopology::Triangles}}));
            EXPECT_TRUE(registry.Commit(handle));
            PackedRenderResourceStatus ready = uploading;
            ready.state = RenderResourcePublicState::GPUReady;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle, uploading, ready, RenderStatusWriter::Render));
            return handle;
        }

        RenderResourceHandle AddReadyMaterialWithoutMetadata(AssetId asset)
        {
            const RenderResourceReserveResult reserved =
                gateway.ReserveResource(asset, RenderResourceKind::Material);
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
            EXPECT_TRUE(registry.BeginPending(handle, RenderResourceKind::Material, {}));
            EXPECT_TRUE(registry.Commit(handle));
            PackedRenderResourceStatus ready = uploading;
            ready.state = RenderResourcePublicState::GPUReady;
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                handle, uploading, ready, RenderStatusWriter::Render));
            return handle;
        }

        RenderResourceHandle ReleaseAndReuseReadyMesh(
            RenderResourceHandle previous,
            AssetId replacementAsset)
        {
            EXPECT_EQ(gateway.RequestRelease(previous).code,
                      RenderReleaseCode::Accepted);
            EXPECT_TRUE(registry.Release(previous));
            EXPECT_EQ(gateway.TryDequeueRelease(), previous);
            EXPECT_TRUE(gateway.GetStatusTable().CompareExchange(
                previous,
                {previous.generation,
                 RenderResourcePublicState::Evicting,
                 RenderResourceFailureCode::None},
                {previous.generation,
                 RenderResourcePublicState::Released,
                 RenderResourceFailureCode::None},
                RenderStatusWriter::Render));
            return AddReadyMesh(replacementAsset);
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

    RenderObject MakePublishedRenderObject(
        uint64 objectId,
        RenderResourceHandle mesh,
        float32 x)
    {
        RenderObject object;
        object.entityId = objectId;
        object.drawable = true;
        object.mesh = mesh;
        object.worldMatrix = Mat4Identity();
        object.worldMatrix[3] = {x, 0.0F, 0.0F, 1.0F};
        object.previousWorldMatrix = object.worldMatrix;
        object.normalMatrix = Mat4Identity();
        object.bounds = AABB({x - 1.0F, -1.0F, -1.0F}, {x + 1.0F, 1.0F, 1.0F});
        object.meshBatches.push_back(MeshBatch{
            objectId,
            mesh,
            {},
            0,
            0,
            MeshUploadIndexType::UInt32,
            {0, 3, 0, MeshUploadPrimitiveTopology::Triangles},
            RenderMaterialMode::Opaque,
            RenderBatchFlags::CastsShadow});
        return object;
    }
} // namespace

TEST(GPUSceneValidation, NullReferencesAndPortableSchemaABIStayExplicit)
{
    EXPECT_FALSE(GPUScenePrimitiveRef{}.IsValid());
    EXPECT_FALSE((GPUSceneBoundsRef{1, 0}.IsValid()));
    EXPECT_FALSE((GPUSceneTransformRef{0, 1}.IsValid()));
    EXPECT_FALSE((GPUSceneMaterialRef{0, 0}.IsValid()));
    EXPECT_FALSE((GPUSceneGeometryRef{7, 0}.IsValid()));
    EXPECT_FALSE((GPUSceneDrawRef{0, 7}.IsValid()));
    EXPECT_TRUE((GPUScenePrimitiveRef{1, 1}.IsValid()));

    EXPECT_EQ(RVX_GPU_SCENE_SCHEMA_VERSION, 1U);
    EXPECT_EQ(sizeof(GPUSceneRowHeader), 32U);
    EXPECT_EQ(sizeof(GPUScenePrimitiveRow), 80U);
    EXPECT_EQ(sizeof(GPUSceneBoundsRow), 96U);
    EXPECT_EQ(sizeof(GPUSceneTransformRow), 192U);
    EXPECT_EQ(sizeof(GPUSceneDrawMetadataRow), 112U);
    EXPECT_EQ(alignof(GPUSceneDrawMetadataRow), 16U);
    EXPECT_EQ(offsetof(GPUScenePrimitiveRow, sortKey), 72U);
    EXPECT_EQ(offsetof(GPUSceneTransformRow, normalFromLocal), 128U);
    EXPECT_EQ(offsetof(GPUSceneDrawMetadataRow, geometry), 48U);
    EXPECT_EQ(offsetof(GPUSceneDrawMetadataRow, pipelineKey), 88U);
    EXPECT_TRUE(HasGPUSceneBoundsFlag(
        static_cast<uint32>(GPUSceneBoundsFlags::ForceVisible) |
            static_cast<uint32>(GPUSceneBoundsFlags::Invalid),
        GPUSceneBoundsFlags::ForceVisible));
}

TEST(GPUSceneValidation, Uint64AndMatrixPackingRoundTripNumerically)
{
    constexpr uint64 sourceId = 0xFEDCBA9876543210ULL;
    const GPUSceneUint64 packedId = PackGPUSceneUint64(sourceId);
    EXPECT_EQ(packedId.low, 0x76543210U);
    EXPECT_EQ(packedId.high, 0xFEDCBA98U);
    EXPECT_EQ(UnpackGPUSceneUint64(packedId), sourceId);

    const GPUSceneMatrix4x4 world = MakeAffineMatrix(2.0F);
    const GPUSceneMatrix4x4 previous = MakeAffineMatrix(4.0F);
    const GPUSceneMatrix4x4 normal = MakeAffineMatrix(6.0F);
    const GPUSceneTransformRow transform{
        {},
        PackGPUSceneAffineMatrix(world),
        PackGPUSceneAffineMatrix(previous),
        PackGPUSceneAffineMatrix(normal),
        static_cast<uint32>(GPUSceneTransformFlags::PreviousWorldFromLocalValid) |
            static_cast<uint32>(GPUSceneTransformFlags::NormalFromLocalValid)};

    EXPECT_EQ(UnpackGPUSceneAffineMatrix(transform.worldFromLocal), world);
    EXPECT_EQ(UnpackGPUSceneAffineMatrix(transform.previousWorldFromLocal), previous);
    EXPECT_EQ(UnpackGPUSceneAffineMatrix(transform.normalFromLocal), normal);
    EXPECT_TRUE(HasGPUSceneTransformFlag(
        transform.transformFlags,
        GPUSceneTransformFlags::PreviousWorldFromLocalValid));
    EXPECT_TRUE(HasGPUSceneTransformFlag(
        transform.transformFlags,
        GPUSceneTransformFlags::NormalFromLocalValid));
}

TEST(GPUSceneValidation, StableHandlesAndIndependentMultiDrawRowsSurviveUpdate)
{
    GPUSceneDatabase database;
    GPUSceneTransaction initial;
    initial.Add(MakeObject(101, 3.0F, 2));
    initial.Add(MakeObject(202, 7.0F, 1));
    const GPUSceneCommitResult initialResult = database.Commit(initial);
    ASSERT_TRUE(initialResult.Succeeded());
    ASSERT_EQ(initialResult.committedVersion, 1U);

    const GPUScenePrimitiveRef first = database.FindPrimitive(101).value();
    const GPUScenePrimitiveRef second = database.FindPrimitive(202).value();
    ASSERT_TRUE(database.IsLive(first));
    ASSERT_TRUE(database.IsLive(second));

    const GPUScenePrimitiveRow* firstRow = database.GetRow(first);
    ASSERT_NE(firstRow, nullptr);
    ASSERT_EQ(firstRow->drawCount, 2U);
    ASSERT_TRUE(database.IsLive(firstRow->bounds));
    ASSERT_TRUE(database.IsLive(firstRow->transform));
    ASSERT_TRUE(database.IsLive(firstRow->firstDraw));
    const GPUSceneDrawRef originalFirstDraw = firstRow->firstDraw;
    std::vector<GPUSceneDrawRef> originalDraws;
    std::vector<GPUSceneMaterialRef> originalMaterials;
    std::vector<GPUSceneGeometryRef> originalGeometries;
    for (uint32 offset = 0; offset < firstRow->drawCount; ++offset)
    {
        const GPUSceneDrawRef drawRef{
            firstRow->firstDraw.slot + offset,
            firstRow->firstDraw.generation};
        const GPUSceneDrawMetadataRow* rangeDraw = database.GetRow(drawRef);
        ASSERT_NE(rangeDraw, nullptr);
        EXPECT_EQ(rangeDraw->header.generation, firstRow->firstDraw.generation);
        EXPECT_EQ(rangeDraw->primitive, first);
        originalDraws.push_back(drawRef);
        originalMaterials.push_back(rangeDraw->material);
        originalGeometries.push_back(rangeDraw->geometry);
    }
    const GPUSceneDrawMetadataRow* draw = database.GetRow(firstRow->firstDraw);
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->primitive, first);
    EXPECT_TRUE(database.IsLive(draw->material));
    EXPECT_TRUE(database.IsLive(draw->geometry));
    EXPECT_EQ(database.GetRow(draw->material)->resourceSlot, 100U);
    EXPECT_EQ(database.GetRow(draw->material)->resourceGeneration, 10U);
    EXPECT_EQ(database.GetRow(draw->geometry)->submeshIndex, 0U);
    EXPECT_EQ(draw->passMask, 1U);
    EXPECT_EQ(draw->materialVariant, 10U);
    EXPECT_EQ(UnpackGPUSceneUint64(draw->pipelineKey), 101U << 8U);
    EXPECT_EQ(UnpackGPUSceneUint64(draw->sortKey), 0x1234567800000009ULL);
    EXPECT_EQ(UnpackGPUSceneUint64(database.GetRow(draw->material)->header.objectId), 101U);

    GPUSceneTransaction update;
    update.Update(first, MakeObject(101, 13.0F, 2));
    const GPUSceneCommitResult updateResult = database.Commit(update);
    ASSERT_TRUE(updateResult.Succeeded());
    EXPECT_EQ(updateResult.committedVersion, 2U);
    EXPECT_EQ(database.FindPrimitive(101).value(), first);
    EXPECT_EQ(database.FindPrimitive(202).value(), second);
    ASSERT_NE(database.GetRow(first), nullptr);
    EXPECT_EQ(database.GetRow(first)->firstDraw, originalFirstDraw);
    ASSERT_EQ(database.GetRow(first)->drawCount, originalDraws.size());
    for (uint32 index = 0; index < originalDraws.size(); ++index)
    {
        const GPUSceneDrawRef updatedDrawRef{
            database.GetRow(first)->firstDraw.slot + index,
            database.GetRow(first)->firstDraw.generation};
        const GPUSceneDrawMetadataRow* updatedDraw = database.GetRow(updatedDrawRef);
        ASSERT_NE(updatedDraw, nullptr);
        EXPECT_EQ(updatedDrawRef, originalDraws[index]);
        EXPECT_EQ(updatedDraw->header.generation,
                  database.GetRow(first)->firstDraw.generation);
        EXPECT_EQ(updatedDraw->material, originalMaterials[index]);
        EXPECT_EQ(updatedDraw->geometry, originalGeometries[index]);
    }
    EXPECT_EQ(database.GetRow(first)->layerMask, 0x8000000DU);
    EXPECT_FLOAT_EQ(
        database.GetRow(database.GetRow(first)->bounds)->minimum.x,
        13.0F);
    EXPECT_EQ(database.GetRow(first)->header.flags,
              static_cast<uint32>(GPUSceneRowFlags::Live));
}

TEST(GPUSceneValidation, ZeroAndDuplicateIdsRejectWithoutPublishingChanges)
{
    GPUSceneDatabase database;
    const GPUScenePrimitiveRef original = AddSingleObject(database, 101);
    const uint64 priorVersion = database.GetCommittedVersion();
    const uint32 priorCapacity = database.GetSlotCapacity();
    const uint32 priorCount = database.GetObjectCount();
    const float32 priorMinimum =
        database.GetRow(database.GetRow(original)->bounds)->minimum.x;

    GPUSceneTransaction zeroId;
    zeroId.Add(MakeObject(0, 2.0F));
    EXPECT_EQ(database.Commit(zeroId).status, GPUSceneCommitStatus::InvalidObjectId);

    GPUSceneTransaction duplicateExisting;
    duplicateExisting.Add(MakeObject(101, 2.0F));
    EXPECT_EQ(database.Commit(duplicateExisting).status,
              GPUSceneCommitStatus::ObjectAlreadyExists);

    GPUSceneTransaction duplicateInTransaction;
    duplicateInTransaction.Add(MakeObject(202, 2.0F));
    duplicateInTransaction.Add(MakeObject(202, 3.0F));
    EXPECT_EQ(database.Commit(duplicateInTransaction).status,
              GPUSceneCommitStatus::DuplicateObjectId);

    EXPECT_EQ(database.GetCommittedVersion(), priorVersion);
    EXPECT_EQ(database.GetSlotCapacity(), priorCapacity);
    EXPECT_EQ(database.GetObjectCount(), priorCount);
    EXPECT_EQ(database.FindPrimitive(101).value(), original);
    EXPECT_FALSE(database.FindPrimitive(202).has_value());
    EXPECT_FLOAT_EQ(
        database.GetRow(database.GetRow(original)->bounds)->minimum.x,
        priorMinimum);
}

TEST(GPUSceneValidation, StaleReferencesRejectAndFailedTransactionRollsBackFully)
{
    GPUSceneDatabase database;
    const GPUScenePrimitiveRef original = AddSingleObject(database, 101);
    const uint64 priorVersion = database.GetCommittedVersion();
    const uint32 priorCapacity = database.GetSlotCapacity();

    GPUSceneTransaction failed;
    failed.Add(MakeObject(202, 2.0F));
    failed.Add(MakeObject(0, 3.0F));
    const GPUSceneCommitResult failedResult = database.Commit(failed);
    EXPECT_EQ(failedResult.status, GPUSceneCommitStatus::InvalidObjectId);
    EXPECT_EQ(failedResult.committedVersion, priorVersion);
    EXPECT_EQ(database.GetCommittedVersion(), priorVersion);
    EXPECT_EQ(database.GetSlotCapacity(), priorCapacity);
    EXPECT_FALSE(database.FindPrimitive(202).has_value());
    EXPECT_EQ(database.FindPrimitive(101).value(), original);

    GPUSceneTransaction remove;
    remove.Remove(original);
    ASSERT_TRUE(database.Commit(remove).Succeeded());

    GPUSceneTransaction staleUpdate;
    staleUpdate.Update(original, MakeObject(101, 9.0F));
    EXPECT_EQ(database.Commit(staleUpdate).status,
              GPUSceneCommitStatus::StalePrimitiveRef);
    EXPECT_FALSE(database.IsLive(original));
}

TEST(GPUSceneValidation, RemoveWritesTombstonesAndDefersEverySlotReuse)
{
    GPUSceneDatabase database;
    const GPUScenePrimitiveRef removed = AddSingleObject(database, 101);
    const GPUScenePrimitiveRow originalRow = *database.GetRow(removed);

    GPUSceneTransaction remove;
    remove.Remove(removed);
    const GPUSceneCommitResult removeResult = database.Commit(remove);
    ASSERT_TRUE(removeResult.Succeeded());
    EXPECT_EQ(database.GetSlotState(removed), GPUSceneSlotState::Retired);
    EXPECT_FALSE(database.IsLive(removed));
    EXPECT_FALSE(database.IsLive(originalRow.bounds));
    EXPECT_FALSE(database.IsLive(originalRow.transform));
    EXPECT_FALSE(database.IsLive(originalRow.firstDraw));
    EXPECT_EQ(database.GetRow(removed), nullptr);
    EXPECT_FALSE(database.FindPrimitive(101).has_value());

    const GPUScenePrimitiveRow& tombstone =
        database.GetCommittedMirror().primitives[removed.slot];
    EXPECT_EQ(UnpackGPUSceneUint64(tombstone.header.objectId), 101U);
    EXPECT_EQ(tombstone.header.generation, removed.generation);
    EXPECT_TRUE(HasGPUSceneRowFlag(
        tombstone.header.flags, GPUSceneRowFlags::Tombstone));
    EXPECT_FALSE(tombstone.bounds.IsValid());

    const GPUScenePrimitiveRef replacement = AddSingleObject(database, 202);
    EXPECT_NE(replacement.slot, removed.slot);
    EXPECT_EQ(database.GetSlotCapacity(), 2U);
    EXPECT_EQ(replacement.generation, 1U);
}

TEST(GPUSceneValidation, MaximumGenerationIsPermanentlyRetiredRatherThanWrapping)
{
    GPUSceneDatabase database(std::numeric_limits<uint32>::max());
    const GPUScenePrimitiveRef maximumGeneration = AddSingleObject(database, 101);
    ASSERT_EQ(maximumGeneration.generation, std::numeric_limits<uint32>::max());

    GPUSceneTransaction remove;
    remove.Remove(maximumGeneration);
    ASSERT_TRUE(database.Commit(remove).Succeeded());
    EXPECT_EQ(database.GetSlotState(maximumGeneration),
              GPUSceneSlotState::PermanentlyRetired);

    const GPUScenePrimitiveRef replacement = AddSingleObject(database, 202);
    EXPECT_NE(replacement.slot, maximumGeneration.slot);
    EXPECT_EQ(database.GetSlotCapacity(), 2U);
    EXPECT_NE(replacement.generation, 0U);
}

TEST(GPUSceneValidation, TestCapacityLimitRejectsTransactionAtomically)
{
    GPUSceneDatabase database(1, 1);
    const GPUScenePrimitiveRef original = AddSingleObject(database, 101);
    const uint64 priorVersion = database.GetCommittedVersion();
    const uint32 priorCapacity = database.GetSlotCapacity();
    const uint32 priorCount = database.GetObjectCount();

    GPUSceneTransaction overCapacity;
    overCapacity.Add(MakeObject(202, 2.0F));
    const GPUSceneCommitResult result = database.Commit(overCapacity);
    EXPECT_EQ(result.status, GPUSceneCommitStatus::CapacityExhausted);
    EXPECT_EQ(result.committedVersion, priorVersion);
    EXPECT_EQ(database.GetCommittedVersion(), priorVersion);
    EXPECT_EQ(database.GetSlotCapacity(), priorCapacity);
    EXPECT_EQ(database.GetObjectCount(), priorCount);
    EXPECT_EQ(database.FindPrimitive(101).value(), original);
    EXPECT_FALSE(database.FindPrimitive(202).has_value());
}

TEST(GPUSceneValidation, VersionExhaustionFailsClosedWhileEmptyCommitStaysNoOp)
{
    GPUSceneDatabase database(
        1,
        std::numeric_limits<uint32>::max(),
        std::numeric_limits<uint64>::max());
    const GPUSceneCommittedMirror* const mirror = &database.GetCommittedMirror();
    const size_t primitiveRowCount = mirror->primitives.size();
    const size_t drawRowCount = mirror->draws.size();
    const GPUSceneRowHeader primitiveSentinel = mirror->primitives[0].header;

    GPUSceneTransaction empty;
    const GPUSceneCommitResult emptyResult = database.Commit(empty);
    EXPECT_TRUE(emptyResult.Succeeded());
    EXPECT_EQ(emptyResult.committedVersion, std::numeric_limits<uint64>::max());

    GPUSceneTransaction nonEmpty;
    nonEmpty.Add(MakeObject(101, 1.0F));
    const GPUSceneCommitResult result = database.Commit(nonEmpty);
    EXPECT_EQ(result.status, GPUSceneCommitStatus::VersionExhausted);
    EXPECT_EQ(result.committedVersion, std::numeric_limits<uint64>::max());
    EXPECT_EQ(&database.GetCommittedMirror(), mirror);
    EXPECT_EQ(database.GetCommittedVersion(), std::numeric_limits<uint64>::max());
    EXPECT_EQ(database.GetSlotCapacity(), 0U);
    EXPECT_EQ(database.GetObjectCount(), 0U);
    EXPECT_FALSE(database.FindPrimitive(101).has_value());
    EXPECT_EQ(database.GetCommittedMirror().primitives.size(), primitiveRowCount);
    EXPECT_EQ(database.GetCommittedMirror().draws.size(), drawRowCount);
    const GPUSceneRowHeader& finalSentinel =
        database.GetCommittedMirror().primitives[0].header;
    EXPECT_EQ(finalSentinel.objectId, primitiveSentinel.objectId);
    EXPECT_EQ(finalSentinel.schemaVersion, primitiveSentinel.schemaVersion);
    EXPECT_EQ(finalSentinel.generation, primitiveSentinel.generation);
    EXPECT_EQ(finalSentinel.flags, primitiveSentinel.flags);
}

TEST(GPUSceneValidation, ClearTombstonesAllIdentityWithoutResettingSlotsOrVersion)
{
    GPUSceneDatabase database;
    GPUSceneTransaction initial;
    initial.Add(MakeObject(101, 1.0F, 2));
    initial.Add(MakeObject(202, 2.0F, 1));
    ASSERT_TRUE(database.Commit(initial).Succeeded());

    const GPUScenePrimitiveRef first = database.FindPrimitive(101).value();
    const GPUScenePrimitiveRef second = database.FindPrimitive(202).value();
    const GPUScenePrimitiveRow firstRow = *database.GetRow(first);
    const GPUSceneBoundsRef firstBounds = firstRow.bounds;
    const GPUSceneTransformRef firstTransform = firstRow.transform;
    const GPUSceneDrawRef firstDraw = firstRow.firstDraw;
    const GPUSceneDrawMetadataRow firstDrawRow = *database.GetRow(firstDraw);
    const uint64 versionBeforeClear = database.GetCommittedVersion();
    const uint32 capacityBeforeClear = database.GetSlotCapacity();

    database.Clear();

    EXPECT_EQ(database.GetCommittedVersion(), versionBeforeClear + 1U);
    EXPECT_EQ(database.GetSlotCapacity(), capacityBeforeClear);
    EXPECT_EQ(database.GetObjectCount(), 0U);
    EXPECT_FALSE(database.FindPrimitive(101).has_value());
    EXPECT_FALSE(database.FindPrimitive(202).has_value());
    EXPECT_FALSE(database.IsLive(first));
    EXPECT_FALSE(database.IsLive(second));
    EXPECT_FALSE(database.IsLive(firstBounds));
    EXPECT_FALSE(database.IsLive(firstTransform));
    EXPECT_FALSE(database.IsLive(firstDraw));
    EXPECT_FALSE(database.IsLive(firstDrawRow.material));
    EXPECT_FALSE(database.IsLive(firstDrawRow.geometry));
    EXPECT_EQ(database.GetRow(first), nullptr);
    EXPECT_TRUE(HasGPUSceneRowFlag(
        database.GetCommittedMirror().primitives[first.slot].header.flags,
        GPUSceneRowFlags::Tombstone));

    const GPUScenePrimitiveRef replacement = AddSingleObject(database, 303);
    EXPECT_GT(replacement.slot, capacityBeforeClear);
    EXPECT_FALSE(database.IsLive(first));
}

TEST(GPUSceneValidation, PreparationAllocationFailuresLeaveCommittedMirrorUntouched)
{
    constexpr int32 maximumPreparationCheckpoints = 64;
    bool reachedSuccessfulPreparation = false;
    for (int32 checkpoint = 0; checkpoint <= maximumPreparationCheckpoints;
         ++checkpoint)
    {
        GPUSceneDatabase database;
        const GPUScenePrimitiveRef original = AddSingleObject(database, 101);
        const GPUSceneCommittedMirror before = database.GetCommittedMirror();
        const GPUSceneChangeSet changesBefore = database.GetLastChangeSet();
        const uint64 versionBefore = database.GetCommittedVersion();
        const uint32 objectCountBefore = database.GetObjectCount();
        const uint32 slotCapacityBefore = database.GetSlotCapacity();

        GPUSceneTransaction transaction;
        transaction.Add(MakeObject(202, 2.0F, 2));
        database.SetPrepareAllocationFailureCountdownForTesting(checkpoint);
        const GPUSceneCommitResult result = database.Commit(transaction);

        if (result.Succeeded())
        {
            reachedSuccessfulPreparation = true;
            EXPECT_TRUE(database.FindPrimitive(202).has_value());
            break;
        }

        EXPECT_EQ(result.status, GPUSceneCommitStatus::AllocationFailed)
            << "checkpoint=" << checkpoint;
        EXPECT_EQ(database.GetCommittedVersion(), versionBefore);
        EXPECT_EQ(database.GetObjectCount(), objectCountBefore);
        EXPECT_EQ(database.GetSlotCapacity(), slotCapacityBefore);
        EXPECT_EQ(database.GetCommittedMirror().primitives, before.primitives);
        EXPECT_EQ(database.GetCommittedMirror().bounds, before.bounds);
        EXPECT_EQ(database.GetCommittedMirror().transforms, before.transforms);
        EXPECT_EQ(database.GetCommittedMirror().materials, before.materials);
        EXPECT_EQ(database.GetCommittedMirror().geometries, before.geometries);
        EXPECT_EQ(database.GetCommittedMirror().draws, before.draws);
        EXPECT_EQ(database.GetLastChangeSet(), changesBefore);
        EXPECT_TRUE(database.IsLive(original));
        EXPECT_FALSE(database.FindPrimitive(202).has_value());
    }
    EXPECT_TRUE(reachedSuccessfulPreparation);
}

TEST(GPUSceneValidation, ZeroDrawObjectSupportsAddUpdateAndRemove)
{
    GPUSceneDatabase database;
    GPUSceneTransaction add;
    add.Add(MakeObject(101, 2.0F, 0));
    ASSERT_TRUE(database.Commit(add).Succeeded());
    const GPUScenePrimitiveRef primitive = database.FindPrimitive(101).value();
    ASSERT_NE(database.GetRow(primitive), nullptr);
    EXPECT_EQ(database.GetRow(primitive)->drawCount, 0U);
    EXPECT_FALSE(database.GetRow(primitive)->firstDraw.IsValid());

    GPUSceneTransaction update;
    update.Update(primitive, MakeObject(101, 5.0F, 0));
    ASSERT_TRUE(database.Commit(update).Succeeded());
    EXPECT_EQ(database.FindPrimitive(101).value(), primitive);
    EXPECT_EQ(database.GetRow(primitive)->drawCount, 0U);
    EXPECT_FALSE(database.GetRow(primitive)->firstDraw.IsValid());
    EXPECT_FLOAT_EQ(
        database.GetRow(database.GetRow(primitive)->bounds)->minimum.x,
        5.0F);

    GPUSceneTransaction remove;
    remove.Remove(primitive);
    ASSERT_TRUE(database.Commit(remove).Succeeded());
    EXPECT_FALSE(database.IsLive(primitive));
    EXPECT_FALSE(database.FindPrimitive(101).has_value());
}

TEST(GPUSceneValidation, ChangedDrawCountAllocatesNewBlockAndTombstonesOldRows)
{
    GPUSceneDatabase database;
    GPUSceneTransaction add;
    add.Add(MakeObject(101, 2.0F, 2));
    ASSERT_TRUE(database.Commit(add).Succeeded());
    const GPUScenePrimitiveRef primitive = database.FindPrimitive(101).value();
    const GPUScenePrimitiveRow before = *database.GetRow(primitive);
    ASSERT_EQ(before.drawCount, 2U);

    std::vector<GPUSceneDrawRef> oldDraws;
    std::vector<GPUSceneMaterialRef> oldMaterials;
    std::vector<GPUSceneGeometryRef> oldGeometries;
    for (uint32 index = 0; index < before.drawCount; ++index)
    {
        const GPUSceneDrawRef drawRef{
            before.firstDraw.slot + index,
            before.firstDraw.generation};
        const GPUSceneDrawMetadataRow* draw = database.GetRow(drawRef);
        ASSERT_NE(draw, nullptr);
        oldDraws.push_back(drawRef);
        oldMaterials.push_back(draw->material);
        oldGeometries.push_back(draw->geometry);
    }

    GPUSceneTransaction update;
    update.Update(primitive, MakeObject(101, 3.0F, 3));
    ASSERT_TRUE(database.Commit(update).Succeeded());
    const GPUScenePrimitiveRow* after = database.GetRow(primitive);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->drawCount, 3U);
    EXPECT_NE(after->firstDraw, before.firstDraw);

    for (uint32 index = 0; index < oldDraws.size(); ++index)
    {
        EXPECT_FALSE(database.IsLive(oldDraws[index]));
        EXPECT_FALSE(database.IsLive(oldMaterials[index]));
        EXPECT_FALSE(database.IsLive(oldGeometries[index]));
        EXPECT_TRUE(HasGPUSceneRowFlag(
            database.GetCommittedMirror().draws[oldDraws[index].slot].header.flags,
            GPUSceneRowFlags::Tombstone));
        EXPECT_TRUE(HasGPUSceneRowFlag(
            database.GetCommittedMirror().materials[oldMaterials[index].slot].header.flags,
            GPUSceneRowFlags::Tombstone));
        EXPECT_TRUE(HasGPUSceneRowFlag(
            database.GetCommittedMirror().geometries[oldGeometries[index].slot].header.flags,
            GPUSceneRowFlags::Tombstone));
    }
}

TEST(GPUSceneValidation, DrawCountCapacityFailureRollsBackAtomically)
{
    GPUSceneDatabase database(1, 1);
    const GPUScenePrimitiveRef primitive = AddSingleObject(database, 101);
    const GPUScenePrimitiveRow before = *database.GetRow(primitive);
    const GPUSceneDrawMetadataRow beforeDraw =
        *database.GetRow(before.firstDraw);
    const uint64 priorVersion = database.GetCommittedVersion();

    GPUSceneTransaction update;
    update.Update(primitive, MakeObject(101, 5.0F, 2));
    const GPUSceneCommitResult result = database.Commit(update);
    EXPECT_EQ(result.status, GPUSceneCommitStatus::CapacityExhausted);
    EXPECT_EQ(result.committedVersion, priorVersion);
    EXPECT_EQ(database.GetCommittedVersion(), priorVersion);
    ASSERT_NE(database.GetRow(primitive), nullptr);
    EXPECT_EQ(database.GetRow(primitive)->firstDraw, before.firstDraw);
    EXPECT_EQ(database.GetRow(primitive)->drawCount, 1U);
    EXPECT_TRUE(database.IsLive(before.firstDraw));
    EXPECT_TRUE(database.IsLive(beforeDraw.material));
    EXPECT_TRUE(database.IsLive(beforeDraw.geometry));
    EXPECT_FLOAT_EQ(
        database.GetRow(database.GetRow(primitive)->bounds)->minimum.x,
        1.0F);
}

TEST(GPUSceneValidation, ContradictoryOperationsRejectWithoutPublishingMutation)
{
    GPUSceneDatabase database;
    const GPUScenePrimitiveRef primitive = AddSingleObject(database, 101);
    const uint64 priorVersion = database.GetCommittedVersion();
    const GPUScenePrimitiveRow before = *database.GetRow(primitive);

    GPUSceneTransaction contradictory;
    contradictory.Update(primitive, MakeObject(101, 9.0F));
    contradictory.Remove(primitive);
    const GPUSceneCommitResult result = database.Commit(contradictory);
    EXPECT_EQ(result.status, GPUSceneCommitStatus::DuplicateObjectId);
    EXPECT_EQ(result.committedVersion, priorVersion);
    EXPECT_EQ(database.GetCommittedVersion(), priorVersion);
    EXPECT_EQ(database.FindPrimitive(101).value(), primitive);
    ASSERT_NE(database.GetRow(primitive), nullptr);
    EXPECT_EQ(database.GetRow(primitive)->firstDraw, before.firstDraw);
    EXPECT_FLOAT_EQ(
        database.GetRow(database.GetRow(primitive)->bounds)->minimum.x,
        1.0F);
}

TEST(GPUSceneValidation, MixedDisjointTransactionCommitsOnlyItsTouchedObjects)
{
    GPUSceneDatabase database;
    const GPUScenePrimitiveRef first = AddSingleObject(database, 101);
    const GPUScenePrimitiveRef second = AddSingleObject(database, 202);

    GPUSceneTransaction transaction;
    transaction.Update(first, MakeObject(101, 9.0F, 1));
    transaction.Remove(second);
    transaction.Add(MakeObject(303, 12.0F, 2));
    const GPUSceneCommitResult result = database.Commit(transaction);

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(database.GetCommittedVersion(), 3U);
    EXPECT_EQ(database.FindPrimitive(101).value(), first);
    EXPECT_FALSE(database.FindPrimitive(202).has_value());
    EXPECT_TRUE(database.FindPrimitive(303).has_value());
    EXPECT_FLOAT_EQ(
        database.GetRow(database.GetRow(first)->bounds)->minimum.x,
        9.0F);
    EXPECT_FALSE(database.IsLive(second));
    EXPECT_TRUE(HasGPUSceneRowFlag(
        database.GetCommittedMirror().primitives[second.slot].header.flags,
        GPUSceneRowFlags::Tombstone));
}

TEST(GPUSceneValidation, ChangeSetPublishesExactMergedRangesAndRemainsAtomic)
{
    GPUSceneDatabase database;
    GPUSceneTransaction initial;
    initial.Add(MakeObject(101, 1.0F, 2));
    initial.Add(MakeObject(202, 2.0F, 1));
    ASSERT_TRUE(database.Commit(initial).Succeeded());

    const GPUSceneChangeSet& initialChanges = database.GetLastChangeSet();
    EXPECT_EQ(initialChanges.baseVersion, 0U);
    EXPECT_EQ(initialChanges.committedVersion, 1U);
    EXPECT_FALSE(initialChanges.primitives.fullTableDirty);
    EXPECT_EQ(initialChanges.primitives.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 2}}));
    EXPECT_EQ(initialChanges.bounds.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 2}}));
    EXPECT_EQ(initialChanges.transforms.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 2}}));
    EXPECT_EQ(initialChanges.materials.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 3}}));
    EXPECT_EQ(initialChanges.geometries.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 3}}));
    EXPECT_EQ(initialChanges.draws.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 3}}));

    const GPUScenePrimitiveRef first = database.FindPrimitive(101).value();
    GPUSceneTransaction changedDrawCount;
    changedDrawCount.Update(first, MakeObject(101, 3.0F, 3));
    ASSERT_TRUE(database.Commit(changedDrawCount).Succeeded());
    const GPUSceneChangeSet& updatedChanges = database.GetLastChangeSet();
    EXPECT_EQ(updatedChanges.baseVersion, 1U);
    EXPECT_EQ(updatedChanges.committedVersion, 2U);
    EXPECT_EQ(updatedChanges.primitives.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 1}}));
    EXPECT_EQ(updatedChanges.materials.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 2}, {4, 3}}));
    EXPECT_EQ(updatedChanges.geometries.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 2}, {4, 3}}));
    EXPECT_EQ(updatedChanges.draws.dirtyRanges,
              std::vector<GPUSceneDirtyRowRange>({{1, 2}, {4, 3}}));

    const GPUSceneChangeSet beforeNoOp = updatedChanges;
    GPUSceneTransaction empty;
    EXPECT_TRUE(database.Commit(empty).Succeeded());
    EXPECT_EQ(database.GetLastChangeSet(), beforeNoOp);

    GPUSceneTransaction invalid;
    invalid.Add(MakeObject(0, 4.0F));
    EXPECT_EQ(database.Commit(invalid).status, GPUSceneCommitStatus::InvalidObjectId);
    EXPECT_EQ(database.GetLastChangeSet(), beforeNoOp);
}

TEST(GPUSceneValidation, CompletionWatermarkDefersReuseAndInvalidatesOldReferences)
{
    GPUSceneDatabase database(1, 1);
    const GPUScenePrimitiveRef first = AddSingleObject(database, 101);
    const GPUScenePrimitiveRow firstRow = *database.GetRow(first);
    const GPUSceneDrawMetadataRow firstDraw = *database.GetRow(firstRow.firstDraw);

    GPUSceneTransaction remove;
    remove.Remove(first);
    const GPUSceneCommitResult removed = database.Commit(remove);
    ASSERT_TRUE(removed.Succeeded());

    GPUSceneTransaction beforeCompletion;
    beforeCompletion.Add(MakeObject(202, 2.0F));
    EXPECT_EQ(database.Commit(beforeCompletion).status,
              GPUSceneCommitStatus::CapacityExhausted);
    ASSERT_TRUE(database.ReclaimRetiredThrough(removed.committedVersion - 1U));
    EXPECT_EQ(database.Commit(beforeCompletion).status,
              GPUSceneCommitStatus::CapacityExhausted);

    ASSERT_TRUE(database.ReclaimRetiredThrough(removed.committedVersion));
    GPUSceneTransaction replacementTransaction;
    replacementTransaction.Add(MakeObject(202, 2.0F));
    ASSERT_TRUE(database.Commit(replacementTransaction).Succeeded());
    const GPUScenePrimitiveRef replacement = database.FindPrimitive(202).value();
    const GPUScenePrimitiveRow replacementRow = *database.GetRow(replacement);
    const GPUSceneDrawMetadataRow replacementDraw =
        *database.GetRow(replacementRow.firstDraw);

    EXPECT_EQ(replacement.slot, first.slot);
    EXPECT_EQ(replacement.generation, first.generation + 1U);
    EXPECT_FALSE(database.IsLive(first));
    EXPECT_FALSE(database.IsLive(firstRow.bounds));
    EXPECT_FALSE(database.IsLive(firstRow.transform));
    EXPECT_FALSE(database.IsLive(firstRow.firstDraw));
    EXPECT_FALSE(database.IsLive(firstDraw.material));
    EXPECT_FALSE(database.IsLive(firstDraw.geometry));
    EXPECT_EQ(replacementRow.firstDraw.slot, firstRow.firstDraw.slot);
    EXPECT_EQ(replacementRow.firstDraw.generation, replacement.generation);
    EXPECT_EQ(replacementDraw.material.generation, replacement.generation);
    EXPECT_EQ(replacementDraw.geometry.generation, replacement.generation);
}

TEST(GPUSceneValidation, DrawBlocksReuseOnlyAsOneGenerationAndMatchingCount)
{
    GPUSceneDatabase database(1, 2);
    GPUSceneTransaction add;
    add.Add(MakeObject(101, 1.0F, 2));
    ASSERT_TRUE(database.Commit(add).Succeeded());
    const GPUScenePrimitiveRef first = database.FindPrimitive(101).value();
    const GPUScenePrimitiveRow firstRow = *database.GetRow(first);
    const GPUSceneDrawMetadataRow firstDraw = *database.GetRow(firstRow.firstDraw);

    GPUSceneTransaction remove;
    remove.Remove(first);
    const GPUSceneCommitResult removed = database.Commit(remove);
    ASSERT_TRUE(removed.Succeeded());
    ASSERT_TRUE(database.ReclaimRetiredThrough(removed.committedVersion));

    GPUSceneTransaction wrongCount;
    wrongCount.Add(MakeObject(202, 2.0F, 1));
    EXPECT_EQ(database.Commit(wrongCount).status,
              GPUSceneCommitStatus::CapacityExhausted);

    GPUSceneTransaction matchingCount;
    matchingCount.Add(MakeObject(202, 2.0F, 2));
    ASSERT_TRUE(database.Commit(matchingCount).Succeeded());
    const GPUScenePrimitiveRef replacement = database.FindPrimitive(202).value();
    const GPUScenePrimitiveRow replacementRow = *database.GetRow(replacement);
    const GPUSceneDrawMetadataRow replacementDraw =
        *database.GetRow(replacementRow.firstDraw);

    EXPECT_EQ(replacementRow.firstDraw.slot, firstRow.firstDraw.slot);
    EXPECT_EQ(replacementRow.firstDraw.generation, firstRow.firstDraw.generation + 1U);
    EXPECT_EQ(replacementDraw.material.slot, firstDraw.material.slot);
    EXPECT_EQ(replacementDraw.geometry.slot, firstDraw.geometry.slot);
    EXPECT_EQ(replacementDraw.material.generation, replacementRow.firstDraw.generation);
    EXPECT_EQ(replacementDraw.geometry.generation, replacementRow.firstDraw.generation);
}

TEST(GPUSceneValidation, MaximumGenerationRemainsPermanentlyRetiredAfterWatermark)
{
    GPUSceneDatabase database(std::numeric_limits<uint32>::max(), 1);
    const GPUScenePrimitiveRef first = AddSingleObject(database, 101);
    GPUSceneTransaction remove;
    remove.Remove(first);
    const GPUSceneCommitResult removed = database.Commit(remove);
    ASSERT_TRUE(removed.Succeeded());
    ASSERT_TRUE(database.ReclaimRetiredThrough(removed.committedVersion));
    EXPECT_EQ(database.GetSlotState(first), GPUSceneSlotState::PermanentlyRetired);

    GPUSceneTransaction replacement;
    replacement.Add(MakeObject(202, 2.0F));
    EXPECT_EQ(database.Commit(replacement).status,
              GPUSceneCommitStatus::CapacityExhausted);
    EXPECT_FALSE(database.FindPrimitive(202).has_value());
}

TEST(GPUSceneValidation, ClearPublishesFullDirtyAndUsesTheSameCompletionWatermark)
{
    GPUSceneDatabase database(1, 1);
    const GPUScenePrimitiveRef first = AddSingleObject(database, 101);
    const uint64 versionBeforeClear = database.GetCommittedVersion();
    database.Clear();

    const GPUSceneChangeSet& cleared = database.GetLastChangeSet();
    EXPECT_EQ(cleared.baseVersion, versionBeforeClear);
    EXPECT_EQ(cleared.committedVersion, versionBeforeClear + 1U);
    EXPECT_TRUE(cleared.primitives.fullTableDirty);
    EXPECT_TRUE(cleared.bounds.fullTableDirty);
    EXPECT_TRUE(cleared.transforms.fullTableDirty);
    EXPECT_TRUE(cleared.materials.fullTableDirty);
    EXPECT_TRUE(cleared.geometries.fullTableDirty);
    EXPECT_TRUE(cleared.draws.fullTableDirty);
    EXPECT_TRUE(cleared.primitives.dirtyRanges.empty());
    EXPECT_FALSE(database.IsLive(first));

    GPUSceneTransaction beforeCompletion;
    beforeCompletion.Add(MakeObject(202, 2.0F));
    EXPECT_EQ(database.Commit(beforeCompletion).status,
              GPUSceneCommitStatus::CapacityExhausted);
    ASSERT_TRUE(database.ReclaimRetiredThrough(cleared.committedVersion));
    GPUSceneTransaction replacement;
    replacement.Add(MakeObject(202, 2.0F));
    ASSERT_TRUE(database.Commit(replacement).Succeeded());
    EXPECT_EQ(database.FindPrimitive(202)->slot, first.slot);

    GPUSceneDatabase exhausted(
        1, std::numeric_limits<uint32>::max(),
        std::numeric_limits<uint64>::max());
    const GPUSceneChangeSet beforeExhaustedClear = exhausted.GetLastChangeSet();
    exhausted.Clear();
    EXPECT_EQ(exhausted.GetCommittedVersion(), std::numeric_limits<uint64>::max());
    EXPECT_EQ(exhausted.GetLastChangeSet(), beforeExhaustedClear);
}

TEST(GPUSceneValidation, ReclaimKeepsClearNoexceptRetirementCapacityForLiveDrawBlocks)
{
    GPUSceneDatabase database(1, 2);
    GPUSceneTransaction add;
    add.Add(MakeObject(101, 1.0F));
    add.Add(MakeObject(202, 2.0F));
    ASSERT_TRUE(database.Commit(add).Succeeded());
    const GPUScenePrimitiveRef first = database.FindPrimitive(101).value();
    const GPUScenePrimitiveRef second = database.FindPrimitive(202).value();

    GPUSceneTransaction remove;
    remove.Remove(first);
    const GPUSceneCommitResult removed = database.Commit(remove);
    ASSERT_TRUE(removed.Succeeded());
    ASSERT_TRUE(database.ReclaimRetiredThrough(removed.committedVersion));

    database.Clear();
    EXPECT_FALSE(database.IsLive(second));
    EXPECT_TRUE(database.GetLastChangeSet().draws.fullTableDirty);
    ASSERT_TRUE(database.ReclaimRetiredThrough(database.GetCommittedVersion()));

    GPUSceneTransaction replacement;
    replacement.Add(MakeObject(303, 3.0F));
    ASSERT_TRUE(database.Commit(replacement).Succeeded());
    EXPECT_EQ(database.FindPrimitive(303)->slot, first.slot);
}

TEST(GPUSceneValidation, PublicationDiffsByObjectIdAndIgnoresAcceptedObjectOrder)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle mesh = resources.AddReadyMesh({500});
    GPUSceneUpdate update;

    RenderScene first;
    first.AddObject(MakePublishedRenderObject(1, mesh, 1.0F));
    first.AddObject(MakePublishedRenderObject(2, mesh, 2.0F));
    const GPUScenePublicationStats initial =
        update.Publish(first, resources.registry);
    ASSERT_EQ(initial.failureReason, GPUScenePublicationFailureReason::None);
    EXPECT_EQ(initial.addCount, 2U);
    EXPECT_EQ(initial.publishedObjectCount, 2U);
    EXPECT_EQ(initial.publishedDrawCount, 2U);
    EXPECT_TRUE(initial.complete);
    EXPECT_FALSE(initial.executionEligible);

    RenderScene reordered;
    reordered.AddObject(MakePublishedRenderObject(2, mesh, 2.0F));
    reordered.AddObject(MakePublishedRenderObject(1, mesh, 1.0F));
    const GPUScenePublicationStats noOp =
        update.Publish(reordered, resources.registry);
    EXPECT_EQ(noOp.addCount, 0U);
    EXPECT_EQ(noOp.updateCount, 0U);
    EXPECT_EQ(noOp.removeCount, 0U);
    EXPECT_EQ(noOp.noOpCount, 2U);
    EXPECT_EQ(noOp.committedVersion, initial.committedVersion);

    RenderScene changed;
    changed.AddObject(MakePublishedRenderObject(1, mesh, 4.0F));
    const GPUScenePublicationStats mixed =
        update.Publish(changed, resources.registry);
    EXPECT_EQ(mixed.updateCount, 1U);
    EXPECT_EQ(mixed.removeCount, 1U);
    EXPECT_EQ(mixed.publishedObjectCount, 1U);
    EXPECT_EQ(mixed.excludedObjectCount, 0U);
    EXPECT_GT(mixed.committedVersion, initial.committedVersion);

    const uint64 committedVersion = mixed.committedVersion;
    EXPECT_TRUE(resources.registry.Release(mesh));
    const GPUScenePublicationStats stale =
        update.Revalidate(resources.registry);
    EXPECT_EQ(stale.failureReason,
              GPUScenePublicationFailureReason::ResourceUnavailable);
    EXPECT_FALSE(stale.complete);
    EXPECT_FALSE(stale.executionEligible);
    EXPECT_EQ(stale.committedVersion, committedVersion);

    update.Clear();
    EXPECT_EQ(update.GetStats().sourceSequence, 0U);
    EXPECT_GT(update.GetStats().committedVersion, committedVersion);
    EXPECT_EQ(update.GetStats().publishedObjectCount, 0U);
    EXPECT_EQ(update.GetStats().publishedDrawCount, 0U);
}

TEST(GPUSceneValidation, PublishedReceivesShadowPrimitiveFlagSurvivesCommitMirror)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle mesh = resources.AddReadyMesh({504});
    GPUSceneUpdate update;
    RenderScene scene;
    RenderObject object = MakePublishedRenderObject(1, mesh, 1.0F);
    object.flags = static_cast<uint32>(GPUScenePrimitiveFlags::ReceivesShadow);
    scene.AddObject(std::move(object));

    const GPUScenePublicationStats stats = update.Publish(scene, resources.registry);
    ASSERT_EQ(stats.failureReason, GPUScenePublicationFailureReason::None);
    ASSERT_TRUE(stats.complete);
    ASSERT_NE(stats.committedVersion, 0U);

    const GPUSceneCommittedMirror& mirror = update.GetCommittedMirrorForTesting();
    ASSERT_EQ(mirror.primitives.size(), 2U);
    const GPUScenePrimitiveRow& primitive = mirror.primitives[1];
    EXPECT_TRUE(HasGPUSceneRowFlag(primitive.header.flags, GPUSceneRowFlags::Live));
    EXPECT_TRUE(HasGPUScenePrimitiveFlag(
        primitive.primitiveFlags, GPUScenePrimitiveFlags::ReceivesShadow));
}

TEST(GPUSceneValidation, AcceptedDrawLookupRequiresOneLiveContiguousGenerationCheckedRow)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle mesh = resources.AddReadyMesh({505});
    GPUSceneUpdate update;
    RenderScene scene;
    scene.AddObject(MakePublishedRenderObject(17, mesh, 1.0F));
    ASSERT_TRUE(update.Publish(scene, resources.registry).complete);

    RenderDrawPacket packet =
        BuildLegacyMaterialDrawPacket(scene.GetObject(0).meshBatches[0]);
    packet.pass = RenderPassKind::Depth;
    RenderVisibilityCandidate candidate;
    candidate.candidateIndex = 3;
    candidate.sourcePacketIndex = 5;
    candidate.objectIndex = 0;
    candidate.pass = packet.pass;
    candidate.objectVisible = true;
    candidate.drawable = true;
    candidate.worldBounds = scene.GetObject(0).bounds;

    const std::optional<GPUSceneAcceptedDrawLookup> resolved =
        update.ResolveAcceptedDraw(scene, candidate, packet);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(resolved->IsValid());
    EXPECT_EQ(resolved->committedVersion, update.GetStats().committedVersion);
    EXPECT_LT(resolved->primitive.slot,
              update.GetCommittedMirrorForTesting().primitives.size());
    EXPECT_EQ(resolved->primitive.generation,
              update.GetCommittedMirrorForTesting().primitives[resolved->primitive.slot]
                  .header.generation);
    EXPECT_EQ(resolved->draw.generation,
              update.GetCommittedMirrorForTesting().draws[resolved->draw.slot]
                  .header.generation);

    RenderDrawPacket outOfRangePacket = packet;
    outOfRangePacket.submeshIndex = 9;
    EXPECT_FALSE(update.ResolveAcceptedDraw(scene, candidate, outOfRangePacket).has_value());
    RenderDrawPacket wrongPassPacket = packet;
    wrongPassPacket.pass = RenderPassKind::Transparent;
    RenderVisibilityCandidate wrongPassCandidate = candidate;
    wrongPassCandidate.pass = wrongPassPacket.pass;
    EXPECT_FALSE(update.ResolveAcceptedDraw(
        scene, wrongPassCandidate, wrongPassPacket).has_value());

    RenderScene updatedScene;
    updatedScene.AddObject(MakePublishedRenderObject(17, mesh, 3.0F));
    ASSERT_TRUE(update.Publish(updatedScene, resources.registry).complete);
    RenderDrawPacket updatedPacket =
        BuildLegacyMaterialDrawPacket(updatedScene.GetObject(0).meshBatches[0]);
    updatedPacket.pass = RenderPassKind::Depth;
    RenderVisibilityCandidate updatedCandidate = candidate;
    updatedCandidate.worldBounds = updatedScene.GetObject(0).bounds;
    const std::optional<GPUSceneAcceptedDrawLookup> updated =
        update.ResolveAcceptedDraw(updatedScene, updatedCandidate, updatedPacket);
    ASSERT_TRUE(updated.has_value());
    EXPECT_GT(updated->committedVersion, resolved->committedVersion);
    // The stable refs may remain live across an in-place update, but their old
    // committed-version snapshot must never be mixed with a newer lease.
    EXPECT_NE(updated->committedVersion, resolved->committedVersion);

    update.Clear();
    // Clearing tombstones its primitive and contiguous draw block;
    // stale accepted packets must never resolve a replacement row.
    EXPECT_FALSE(update.ResolveAcceptedDraw(scene, candidate, packet).has_value());

    const uint64 clearedVersion = update.GetStats().committedVersion;
    ASSERT_TRUE(update.ReclaimRetiredThrough(clearedVersion));
    RenderScene replacementScene;
    replacementScene.AddObject(MakePublishedRenderObject(17, mesh, 5.0F));
    ASSERT_TRUE(update.Publish(replacementScene, resources.registry).complete);
    RenderDrawPacket replacementPacket = BuildLegacyMaterialDrawPacket(
        replacementScene.GetObject(0).meshBatches[0]);
    replacementPacket.pass = RenderPassKind::Depth;
    RenderVisibilityCandidate replacementCandidate = candidate;
    replacementCandidate.worldBounds = replacementScene.GetObject(0).bounds;
    const std::optional<GPUSceneAcceptedDrawLookup> replacement =
        update.ResolveAcceptedDraw(
            replacementScene, replacementCandidate, replacementPacket);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_GT(replacement->committedVersion, clearedVersion);
    EXPECT_NE(replacement->primitive, resolved->primitive);
    EXPECT_NE(replacement->draw, resolved->draw);
}

TEST(GPUSceneValidation, PublicationFailureKeepsActualCommittedIdentityAndAttemptCounters)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle mesh = resources.AddReadyMesh({501});
    GPUSceneUpdate update;

    RenderScene scene;
    scene.AddObject(MakePublishedRenderObject(1, mesh, 1.0F));
    const GPUScenePublicationStats initial = update.Publish(scene, resources.registry);
    ASSERT_EQ(initial.failureReason, GPUScenePublicationFailureReason::None);

    RenderScene candidate;
    candidate.AddObject(MakePublishedRenderObject(1, mesh, 2.0F));
    candidate.AddObject(MakePublishedRenderObject(2, mesh, 3.0F));
    update.SetDatabasePrepareAllocationFailureCountdownForTesting(0);
    const GPUScenePublicationStats failed = update.Publish(candidate, resources.registry);

    EXPECT_EQ(failed.failureReason, GPUScenePublicationFailureReason::AllocationFailed);
    EXPECT_EQ(failed.sourceSequence, candidate.GetAcceptedHeader().sequence);
    EXPECT_EQ(failed.committedVersion, initial.committedVersion);
    EXPECT_EQ(failed.committedSourceSequence, initial.committedSourceSequence);
    EXPECT_EQ(failed.publishedObjectCount, 1U);
    EXPECT_EQ(failed.publishedDrawCount, 1U);
    EXPECT_EQ(failed.attemptedObjectCount, 2U);
    EXPECT_EQ(failed.candidateObjectCount, 2U);
    EXPECT_EQ(failed.updateCount, 1U);
    EXPECT_EQ(failed.addCount, 1U);
    EXPECT_FALSE(failed.complete);
    EXPECT_FALSE(failed.executionEligible);
}

TEST(GPUSceneValidation, ChangedDrawCountUpdatesOneObjectWhileAcceptedOrderRemainsNoOp)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle mesh = resources.AddReadyMesh({508});
    GPUSceneUpdate update;

    RenderScene initialScene;
    initialScene.AddObject(MakePublishedRenderObject(1, mesh, 1.0F));
    initialScene.AddObject(MakePublishedRenderObject(2, mesh, 2.0F));
    const GPUScenePublicationStats initial =
        update.Publish(initialScene, resources.registry);
    ASSERT_EQ(initial.failureReason, GPUScenePublicationFailureReason::None);

    RenderObject expanded = MakePublishedRenderObject(1, mesh, 1.0F);
    MeshBatch secondBatch = expanded.meshBatches.front();
    secondBatch.submeshIndex = 1;
    secondBatch.geometry.indexOffset = 3;
    expanded.meshBatches.push_back(secondBatch);
    RenderScene changedDrawCount;
    changedDrawCount.AddObject(std::move(expanded));
    changedDrawCount.AddObject(MakePublishedRenderObject(2, mesh, 2.0F));
    const GPUScenePublicationStats changed =
        update.Publish(changedDrawCount, resources.registry);
    EXPECT_EQ(changed.updateCount, 1U);
    EXPECT_EQ(changed.noOpCount, 1U);
    EXPECT_EQ(changed.publishedObjectCount, 2U);
    EXPECT_EQ(changed.publishedDrawCount, 3U);
    EXPECT_GT(changed.committedVersion, initial.committedVersion);

    RenderScene reordered;
    reordered.AddObject(MakePublishedRenderObject(2, mesh, 2.0F));
    RenderObject reorderedExpanded = MakePublishedRenderObject(1, mesh, 1.0F);
    reorderedExpanded.meshBatches.push_back(secondBatch);
    reordered.AddObject(std::move(reorderedExpanded));
    const GPUScenePublicationStats noOp = update.Publish(reordered, resources.registry);
    EXPECT_EQ(noOp.addCount, 0U);
    EXPECT_EQ(noOp.updateCount, 0U);
    EXPECT_EQ(noOp.removeCount, 0U);
    EXPECT_EQ(noOp.noOpCount, 2U);
    EXPECT_EQ(noOp.committedVersion, changed.committedVersion);
}

TEST(GPUSceneValidation, PassMasksUseOnlyDepthOpaqueShadowAndTransparent)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle mesh = resources.AddReadyMesh({502});
    GPUSceneUpdate update;
    RenderScene scene;

    const auto addObject = [&scene, mesh](
                               uint64 objectId,
                               RenderMaterialMode mode,
                               bool castsShadow)
    {
        RenderObject object = MakePublishedRenderObject(
            objectId, mesh, static_cast<float32>(objectId));
        object.meshBatches[0].materialMode = mode;
        object.meshBatches[0].flags = castsShadow
                                         ? RenderBatchFlags::CastsShadow
                                         : RenderBatchFlags::None;
        scene.AddObject(std::move(object));
    };
    addObject(1, RenderMaterialMode::Opaque, false);
    addObject(2, RenderMaterialMode::Opaque, true);
    addObject(3, RenderMaterialMode::Masked, false);
    addObject(4, RenderMaterialMode::Masked, true);
    addObject(5, RenderMaterialMode::Transparent, false);
    addObject(6, RenderMaterialMode::Transparent, true);

    ASSERT_EQ(update.Publish(scene, resources.registry).failureReason,
              GPUScenePublicationFailureReason::None);
    const GPUSceneCommittedMirror& mirror = update.GetCommittedMirrorForTesting();
    ASSERT_EQ(mirror.draws.size(), 7U);
    const uint32 depthOpaque =
        static_cast<uint32>(GPUScenePassMask::Depth) |
        static_cast<uint32>(GPUScenePassMask::Opaque);
    const uint32 transparentMask = static_cast<uint32>(GPUScenePassMask::Transparent);
    for (size_t index = 1; index < mirror.draws.size(); ++index)
    {
        const GPUSceneDrawMetadataRow& draw = mirror.draws[index];
        const uint64 objectId = UnpackGPUSceneUint64(draw.header.objectId);
        const bool castsShadow = objectId % 2U == 0U;
        const bool transparent = objectId == 5U || objectId == 6U;
        const uint32 expectedMask =
            (transparent ? transparentMask : depthOpaque) |
            (castsShadow ? static_cast<uint32>(GPUScenePassMask::Shadow) : 0U);
        const uint32 expectedVariant =
            transparent
                ? static_cast<uint32>(MaterialPipelineVariant::Transparent)
                : (objectId == 3U || objectId == 4U)
                      ? static_cast<uint32>(MaterialPipelineVariant::Masked)
                      : static_cast<uint32>(MaterialPipelineVariant::Opaque);
        EXPECT_EQ(draw.passMask, expectedMask);
        EXPECT_EQ(draw.materialVariant, expectedVariant);
    }
}

TEST(GPUSceneValidation, MissingAndMetadataInvalidMaterialsPublishDefaultRows)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle mesh = resources.AddReadyMesh({503});
    const RenderResourceHandle invalidMetadata =
        resources.AddReadyMaterialWithoutMetadata({504});
    GPUSceneUpdate update;
    RenderScene scene;

    RenderObject missing = MakePublishedRenderObject(1, mesh, 1.0F);
    RenderObject invalid = MakePublishedRenderObject(2, mesh, 2.0F);
    invalid.material = invalidMetadata;
    invalid.meshBatches[0].material = invalidMetadata;
    scene.AddObject(std::move(missing));
    scene.AddObject(std::move(invalid));

    const GPUScenePublicationStats stats = update.Publish(scene, resources.registry);
    ASSERT_EQ(stats.failureReason, GPUScenePublicationFailureReason::None);
    ASSERT_EQ(stats.publishedObjectCount, 2U);
    const GPUSceneCommittedMirror& mirror = update.GetCommittedMirrorForTesting();
    ASSERT_EQ(mirror.materials.size(), 3U);
    for (size_t index = 1; index < mirror.materials.size(); ++index)
    {
        const GPUSceneMaterialRow& material = mirror.materials[index];
        const uint64 objectId = UnpackGPUSceneUint64(material.header.objectId);
        EXPECT_TRUE(HasGPUSceneMaterialFlag(
            material.materialFlags, GPUSceneMaterialFlags::DefaultMaterial));
        EXPECT_TRUE(HasGPUSceneMaterialFlag(
            material.materialFlags,
            objectId == 1U ? GPUSceneMaterialFlags::MissingMaterial
                           : GPUSceneMaterialFlags::MetadataInvalid));
        EXPECT_EQ(material.resourceSlot, 0U);
        EXPECT_EQ(material.resourceGeneration, 0U);
        EXPECT_EQ(material.baseColor, (GPUSceneFloat4{0.8F, 0.8F, 0.8F, 1.0F}));
        EXPECT_FLOAT_EQ(material.metallic, 0.0F);
        EXPECT_FLOAT_EQ(material.roughness, 0.5F);
        EXPECT_FLOAT_EQ(material.emissiveIntensity, 0.0F);
        EXPECT_FLOAT_EQ(material.opacity, 1.0F);
    }
}

TEST(GPUSceneValidation, EvictedGenerationIsNotReboundUntilNewHandleIsAccepted)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle oldMesh = resources.AddReadyMesh({505});
    GPUSceneUpdate update;
    RenderScene oldScene;
    oldScene.AddObject(MakePublishedRenderObject(1, oldMesh, 1.0F));
    ASSERT_EQ(update.Publish(oldScene, resources.registry).failureReason,
              GPUScenePublicationFailureReason::None);

    const RenderResourceHandle newMesh =
        resources.ReleaseAndReuseReadyMesh(oldMesh, {506});
    ASSERT_EQ(newMesh.slot, oldMesh.slot);
    ASSERT_EQ(newMesh.generation, oldMesh.generation + 1U);
    const GPUScenePublicationStats stale = update.Revalidate(resources.registry);
    EXPECT_EQ(stale.failureReason,
              GPUScenePublicationFailureReason::ResourceUnavailable);
    EXPECT_EQ(stale.publishedObjectCount, 1U);
    const GPUSceneCommittedMirror& staleMirror =
        update.GetCommittedMirrorForTesting();
    EXPECT_EQ(staleMirror.geometries[1].resourceSlot, oldMesh.slot);
    EXPECT_EQ(staleMirror.geometries[1].resourceGeneration, oldMesh.generation);

    RenderScene replacementScene;
    replacementScene.AddObject(MakePublishedRenderObject(1, newMesh, 1.0F));
    const GPUScenePublicationStats replacement =
        update.Publish(replacementScene, resources.registry);
    ASSERT_EQ(replacement.failureReason, GPUScenePublicationFailureReason::None);
    EXPECT_EQ(replacement.publishedObjectCount, 1U);
    const GPUSceneCommittedMirror& replacementMirror =
        update.GetCommittedMirrorForTesting();
    ASSERT_GE(replacementMirror.geometries.size(), 2U);
    bool foundNewGeneration = false;
    for (size_t index = 1; index < replacementMirror.geometries.size(); ++index)
    {
        const GPUSceneGeometryRow& geometry = replacementMirror.geometries[index];
        if (HasGPUSceneRowFlag(geometry.header.flags, GPUSceneRowFlags::Live))
        {
            foundNewGeneration = geometry.resourceSlot == newMesh.slot &&
                                 geometry.resourceGeneration == newMesh.generation;
        }
    }
    EXPECT_TRUE(foundNewGeneration);
}

TEST(GPUSceneValidation,
     RevalidationCannotPromoteAnIncompletePublicationAfterHandleReuse)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle meshA = resources.AddReadyMesh({509});
    const RenderResourceHandle oldMeshB = resources.AddReadyMesh({510});
    GPUSceneUpdate update;

    RenderScene staleAcceptedScene;
    staleAcceptedScene.AddObject(MakePublishedRenderObject(1, meshA, 1.0F));
    staleAcceptedScene.AddObject(MakePublishedRenderObject(2, oldMeshB, 2.0F));
    const RenderResourceHandle newMeshB =
        resources.ReleaseAndReuseReadyMesh(oldMeshB, {511});
    ASSERT_EQ(newMeshB.slot, oldMeshB.slot);
    ASSERT_EQ(newMeshB.generation, oldMeshB.generation + 1U);

    const GPUScenePublicationStats partial =
        update.Publish(staleAcceptedScene, resources.registry);
    EXPECT_EQ(partial.failureReason,
              GPUScenePublicationFailureReason::ResourceUnavailable);
    EXPECT_EQ(partial.attemptedObjectCount, 2U);
    EXPECT_EQ(partial.candidateObjectCount, 1U);
    EXPECT_EQ(partial.excludedObjectCount, 1U);
    EXPECT_EQ(partial.publishedObjectCount, 1U);
    EXPECT_FALSE(partial.complete);

    const GPUScenePublicationStats revalidated =
        update.Revalidate(resources.registry);
    EXPECT_EQ(revalidated.failureReason,
              GPUScenePublicationFailureReason::ResourceUnavailable);
    EXPECT_EQ(revalidated.publishedObjectCount, 1U);
    EXPECT_FALSE(revalidated.complete);
    const GPUSceneCommittedMirror& incompleteMirror =
        update.GetCommittedMirrorForTesting();
    bool hasOldB = false;
    for (size_t index = 1; index < incompleteMirror.primitives.size(); ++index)
    {
        const GPUScenePrimitiveRow& primitive = incompleteMirror.primitives[index];
        hasOldB = hasOldB ||
                  (HasGPUSceneRowFlag(
                       primitive.header.flags, GPUSceneRowFlags::Live) &&
                   UnpackGPUSceneUint64(primitive.header.objectId) == 2U);
    }
    EXPECT_FALSE(hasOldB);

    RenderScene replacementAcceptedScene;
    replacementAcceptedScene.AddObject(MakePublishedRenderObject(1, meshA, 1.0F));
    replacementAcceptedScene.AddObject(MakePublishedRenderObject(2, newMeshB, 2.0F));
    const GPUScenePublicationStats complete =
        update.Publish(replacementAcceptedScene, resources.registry);
    EXPECT_EQ(complete.failureReason, GPUScenePublicationFailureReason::None);
    EXPECT_EQ(complete.addCount, 1U);
    EXPECT_EQ(complete.noOpCount, 1U);
    EXPECT_EQ(complete.publishedObjectCount, 2U);
    EXPECT_TRUE(complete.complete);
}

TEST(GPUSceneValidation, AffineRowsAreRowMajorAndInvalidTransformsAreExcluded)
{
    PublicationRegistryFixture resources;
    const RenderResourceHandle mesh = resources.AddReadyMesh({507});
    GPUSceneUpdate update;
    RenderScene scene;

    RenderObject valid = MakePublishedRenderObject(1, mesh, 1.0F);
    valid.worldMatrix[0] = {1.0F, 2.0F, 3.0F, 0.0F};
    valid.worldMatrix[1] = {4.0F, 5.0F, 6.0F, 0.0F};
    valid.worldMatrix[2] = {7.0F, 8.0F, 9.0F, 0.0F};
    valid.worldMatrix[3] = {10.0F, 11.0F, 12.0F, 1.0F};
    valid.previousWorldMatrix = valid.worldMatrix;
    valid.normalMatrix = Mat4Identity();

    RenderObject nonAffine = MakePublishedRenderObject(2, mesh, 2.0F);
    nonAffine.worldMatrix[0][3] = 0.5F;
    RenderObject nonFiniteNormal = MakePublishedRenderObject(3, mesh, 3.0F);
    nonFiniteNormal.normalMatrix[1][2] = std::numeric_limits<float32>::quiet_NaN();
    scene.AddObject(std::move(valid));
    scene.AddObject(std::move(nonAffine));
    scene.AddObject(std::move(nonFiniteNormal));

    const GPUScenePublicationStats stats = update.Publish(scene, resources.registry);
    EXPECT_EQ(stats.attemptedObjectCount, 3U);
    EXPECT_EQ(stats.candidateObjectCount, 1U);
    EXPECT_EQ(stats.excludedObjectCount, 2U);
    EXPECT_EQ(stats.failureReason, GPUScenePublicationFailureReason::InvalidObject);
    EXPECT_EQ(stats.publishedObjectCount, 1U);
    const GPUSceneCommittedMirror& mirror = update.GetCommittedMirrorForTesting();
    ASSERT_EQ(mirror.transforms.size(), 2U);
    const GPUSceneAffineMatrix3x4& packed = mirror.transforms[1].worldFromLocal;
    EXPECT_EQ(packed.rows[0], (GPUSceneFloat4{1.0F, 4.0F, 7.0F, 10.0F}));
    EXPECT_EQ(packed.rows[1], (GPUSceneFloat4{2.0F, 5.0F, 8.0F, 11.0F}));
    EXPECT_EQ(packed.rows[2], (GPUSceneFloat4{3.0F, 6.0F, 9.0F, 12.0F}));
}

TEST(GPUSceneValidation, DiagnosticsUseActualMirrorCapacityAndCompletionGatedLifecycle)
{
    const GPUSceneDiagnostics reset{};
    EXPECT_FALSE(reset.available);
    EXPECT_TRUE(reset.informationalOnly);
    EXPECT_EQ(reset.gpuAllocationBytes, 0U);
    EXPECT_EQ(reset.slots.liveSlotCount, 0U);

    GPUSceneDatabase database(1, 1);
    const GPUScenePrimitiveRef first = AddSingleObject(database, 801);

    const GPUSceneDiagnostics warm = database.GetDiagnostics();
    const GPUSceneTableDiagnostics& primitiveTable = warm.tables[
        static_cast<uint32>(GPUSceneDiagnosticsTable::Primitives)];
    EXPECT_EQ(primitiveTable.payloadRowCount,
              database.GetCommittedMirror().primitives.size());
    EXPECT_GE(primitiveTable.cpuRowCapacity, primitiveTable.payloadRowCount);
    EXPECT_EQ(primitiveTable.cpuPayloadBytes,
              static_cast<uint64>(database.GetCommittedMirror().primitives.size()) *
                  sizeof(GPUScenePrimitiveRow));
    EXPECT_EQ(warm.slots.liveSlotCount, 6U);
    EXPECT_EQ(warm.slots.retiredSlotCount, 0U);
    EXPECT_EQ(warm.slots.reclaimedSlotCount, 0U);

    GPUSceneTransaction remove;
    remove.Remove(first);
    ASSERT_TRUE(database.Commit(remove).Succeeded());
    const GPUSceneDiagnostics retired = database.GetDiagnostics();
    EXPECT_EQ(retired.slots.liveSlotCount, 0U);
    EXPECT_EQ(retired.slots.retiredSlotCount, 6U);
    EXPECT_EQ(retired.slots.conservativeReusePressureCount, 6U);
    EXPECT_EQ(retired.slots.retiredDrawBlockCount, 1U);

    ASSERT_TRUE(database.ReclaimRetiredThrough(database.GetCommittedVersion()));
    const GPUSceneDiagnostics reclaimed = database.GetDiagnostics();
    EXPECT_EQ(reclaimed.slots.freeSlotCount, 6U);
    EXPECT_EQ(reclaimed.slots.retiredSlotCount, 0U);
    EXPECT_EQ(reclaimed.slots.reclaimedSlotCount, 6U);
    EXPECT_EQ(reclaimed.slots.reclaimedDrawBlockCount, 1U);
    EXPECT_EQ(reclaimed.slots.conservativeReusePressureCount, 0U);
}
