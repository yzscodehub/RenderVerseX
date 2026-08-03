#include "GPUScene/GPUSceneDatabase.h"

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
