#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderDrawPacket.h"
#include "Render/Renderer/RenderScene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <vector>

using namespace RVX;

namespace
{
    MeshBatchBuildInput MakeValidInput()
    {
        MeshBatchBuildInput input;
        input.objectId = 77;
        input.mesh = RenderResourceHandle{11, 3};
        input.primitiveData = 4;
        input.indexType = MeshUploadIndexType::UInt16;
        input.boundsMin = {-1.0f, -2.0f, -3.0f};
        input.boundsMax = {1.0f, 2.0f, 3.0f};
        input.flags = RenderBatchFlags::Skinned | RenderBatchFlags::CastsShadow;
        input.submeshes = {
            {0,
             {0, 3, 0, MeshUploadPrimitiveTopology::Triangles},
             RenderResourceHandle{21, 2},
             RenderMaterialMode::Opaque},
            {1,
             {3, 6, -2, MeshUploadPrimitiveTopology::TriangleStrip},
             RenderResourceHandle{22, 5},
             RenderMaterialMode::Transparent}};
        return input;
    }

    MeshBatch MakeBatch(RenderMaterialMode mode, uint32 submeshIndex)
    {
        MeshBatch batch;
        batch.objectId = 7;
        batch.mesh = RenderResourceHandle{3, 9};
        batch.material = RenderResourceHandle{4 + submeshIndex, 1};
        batch.submeshIndex = submeshIndex;
        batch.primitiveData = 2;
        batch.indexType = MeshUploadIndexType::UInt32;
        batch.geometry = {submeshIndex * 3, 3, 0,
                          MeshUploadPrimitiveTopology::Triangles};
        batch.materialMode = mode;
        if (mode == RenderMaterialMode::Masked)
            batch.flags |= RenderBatchFlags::Masked;
        if (mode == RenderMaterialMode::Transparent)
            batch.flags |= RenderBatchFlags::Transparent;
        return batch;
    }
} // namespace

TEST(RenderDrawPacketValidation, BuildsExactBatchesAndIndexedArguments)
{
    const MeshBatchBuildResult result = BuildMeshBatches(MakeValidInput());
    ASSERT_TRUE(result.IsSuccess());
    ASSERT_EQ(result.batches.size(), 2U);
    EXPECT_EQ(result.batches[0].mesh, (RenderResourceHandle{11, 3}));
    EXPECT_EQ(result.batches[1].submeshIndex, 1U);
    EXPECT_EQ(result.batches[1].geometry.indexOffset, 3U);
    EXPECT_EQ(result.batches[1].geometry.indexCount, 6U);

    const RenderDrawPacket packet =
        BuildLegacyMaterialDrawPacket(result.batches[1]);
    EXPECT_EQ(packet.pass, RenderPassKind::Transparent);
    EXPECT_EQ(packet.pipelineKey.materialVariant,
              MaterialPipelineVariant::Transparent);
    EXPECT_EQ(packet.geometryKey.mesh, (RenderResourceHandle{11, 3}));
    EXPECT_EQ(packet.geometryKey.submeshIndex, 1U);
    EXPECT_EQ(packet.geometryKey.indexType, MeshUploadIndexType::UInt16);
    EXPECT_EQ(packet.arguments.indexCount, 6U);
    EXPECT_EQ(packet.arguments.instanceCount, 1U);
    EXPECT_EQ(packet.arguments.firstIndex, 3U);
    EXPECT_EQ(packet.arguments.vertexOffset, -2);
    EXPECT_EQ(packet.arguments.firstInstance, 0U);
}

TEST(RenderDrawPacketValidation, ValidatesSubmeshBoundsAndMissingMaterials)
{
    MeshBatchBuildInput missingSubmesh = MakeValidInput();
    missingSubmesh.submeshes.clear();
    EXPECT_EQ(BuildMeshBatches(missingSubmesh).code,
              MeshBatchBuildCode::MissingSubmesh);

    MeshBatchBuildInput missingMaterial = MakeValidInput();
    missingMaterial.submeshes[0].material = {};
    const MeshBatchBuildResult missingMaterialResult =
        BuildMeshBatches(missingMaterial);
    ASSERT_TRUE(missingMaterialResult.IsSuccess());
    EXPECT_TRUE(HasRenderBatchFlag(missingMaterialResult.batches[0].flags,
                                   RenderBatchFlags::MissingMaterial));

    MeshBatchBuildInput nanBounds = MakeValidInput();
    nanBounds.boundsMin.x = std::numeric_limits<float32>::quiet_NaN();
    EXPECT_EQ(BuildMeshBatches(nanBounds).code,
              MeshBatchBuildCode::MalformedBounds);

    MeshBatchBuildInput unorderedBounds = MakeValidInput();
    unorderedBounds.boundsMin.y = 3.0f;
    EXPECT_EQ(BuildMeshBatches(unorderedBounds).code,
              MeshBatchBuildCode::MalformedBounds);

    MeshBatchBuildInput zeroExtent = MakeValidInput();
    zeroExtent.boundsMin = {2.0f, 2.0f, 2.0f};
    zeroExtent.boundsMax = zeroExtent.boundsMin;
    EXPECT_TRUE(BuildMeshBatches(zeroExtent).IsSuccess());

    MeshBatchBuildInput overflowingRange = MakeValidInput();
    overflowingRange.submeshes[0].geometry.indexOffset =
        std::numeric_limits<uint32>::max() - 1U;
    overflowingRange.submeshes[0].geometry.indexCount = 3;
    EXPECT_EQ(BuildMeshBatches(overflowingRange).code,
              MeshBatchBuildCode::InvalidSubmesh);
}

TEST(RenderDrawPacketValidation, PreservesMaterialClassificationAndSkinnedFlags)
{
    MeshBatchBuildInput input = MakeValidInput();
    input.submeshes[0].materialMode = RenderMaterialMode::Masked;
    const MeshBatchBuildResult result = BuildMeshBatches(input);
    ASSERT_TRUE(result.IsSuccess());
    EXPECT_TRUE(HasRenderBatchFlag(result.batches[0].flags,
                                   RenderBatchFlags::Skinned));
    EXPECT_TRUE(HasRenderBatchFlag(result.batches[0].flags,
                                   RenderBatchFlags::Masked));
    EXPECT_TRUE(HasRenderBatchFlag(result.batches[1].flags,
                                   RenderBatchFlags::Transparent));
    EXPECT_EQ(BuildLegacyMaterialDrawPacket(result.batches[0]).pass,
              RenderPassKind::Opaque);
    EXPECT_EQ(BuildLegacyMaterialDrawPacket(result.batches[1]).pass,
              RenderPassKind::Transparent);

    MeshBatchBuildInput nonCanonicalFlags = MakeValidInput();
    nonCanonicalFlags.flags |= RenderBatchFlags::Masked |
                               RenderBatchFlags::Transparent |
                               RenderBatchFlags::MissingMaterial;
    nonCanonicalFlags.submeshes.resize(1);
    nonCanonicalFlags.submeshes[0].materialMode = RenderMaterialMode::Opaque;
    const MeshBatchBuildResult normalized = BuildMeshBatches(nonCanonicalFlags);
    ASSERT_TRUE(normalized.IsSuccess());
    EXPECT_FALSE(HasRenderBatchFlag(normalized.batches[0].flags,
                                    RenderBatchFlags::Masked));
    EXPECT_FALSE(HasRenderBatchFlag(normalized.batches[0].flags,
                                    RenderBatchFlags::Transparent));
    EXPECT_FALSE(HasRenderBatchFlag(normalized.batches[0].flags,
                                    RenderBatchFlags::MissingMaterial));
}

TEST(RenderDrawPacketValidation, KeysUseCompleteExactIdentityAndStableHashing)
{
    PipelineKey pipelineA;
    pipelineA.materialVariant = MaterialPipelineVariant::Masked;
    pipelineA.topology = MeshUploadPrimitiveTopology::TriangleStrip;
    pipelineA.skinned = true;
    PipelineKey pipelineB = pipelineA;
    EXPECT_EQ(pipelineA, pipelineB);
    EXPECT_EQ(GetStableHash(pipelineA), GetStableHash(pipelineB));
    pipelineB.skinned = false;
    EXPECT_NE(pipelineA, pipelineB);

    GeometryBindingKey geometryA{
        RenderResourceHandle{8, 2}, 1, MeshUploadIndexType::UInt16};
    GeometryBindingKey geometryB = geometryA;
    EXPECT_EQ(geometryA, geometryB);
    EXPECT_EQ(GetStableHash(geometryA), GetStableHash(geometryB));
    geometryB.mesh.generation = 3;
    EXPECT_NE(geometryA, geometryB);

    MaterialBindingKey materialA{{}, RenderMaterialMode::Opaque};
    MaterialBindingKey materialB{{}, RenderMaterialMode::Opaque};
    EXPECT_EQ(materialA, materialB);
    EXPECT_EQ(GetStableHash(materialA), GetStableHash(materialB));
    materialB.material = RenderResourceHandle{9, 1};
    EXPECT_NE(materialA, materialB);

    const MeshBatch batch = MakeBatch(RenderMaterialMode::Opaque, 0);
    const RenderDrawPacket first = BuildLegacyMaterialDrawPacket(batch);
    const RenderDrawPacket repeated = BuildLegacyMaterialDrawPacket(batch);
    EXPECT_EQ(first.pipelineKey, repeated.pipelineKey);
    EXPECT_EQ(first.geometryKey, repeated.geometryKey);
    EXPECT_EQ(first.materialKey, repeated.materialKey);
    EXPECT_EQ(first.arguments, repeated.arguments);
    EXPECT_EQ(first, repeated);
}

TEST(RenderDrawPacketValidation, ProjectsAuthoritativeAndLegacyObjectsToLists)
{
    RenderScene scene;
    RenderObject authoritative;
    authoritative.entityId = 1;
    authoritative.meshBatchesAuthoritative = true;
    authoritative.meshBatches = {
        MakeBatch(RenderMaterialMode::Opaque, 0),
        MakeBatch(RenderMaterialMode::Masked, 1),
        MakeBatch(RenderMaterialMode::Transparent, 2)};
    scene.AddObject(authoritative);

    RenderObject authoritativeEmpty;
    authoritativeEmpty.entityId = 2;
    authoritativeEmpty.meshBatchesAuthoritative = true;
    scene.AddObject(authoritativeEmpty);

    RenderObject legacy;
    legacy.entityId = 3;
    legacy.mesh = RenderResourceHandle{31, 1};
    legacy.material = RenderResourceHandle{32, 1};
    legacy.materialModes = {RenderMaterialMode::Opaque};
    scene.AddObject(legacy);

    std::vector<RenderDrawItem> opaque;
    std::vector<RenderDrawItem> masked;
    std::vector<RenderDrawItem> transparent;
    BuildMaterialDrawLists(scene, {0, 1, 2}, Vec3{0.0f}, opaque, masked,
                           transparent);
    ASSERT_EQ(opaque.size(), 2U);
    ASSERT_EQ(masked.size(), 1U);
    ASSERT_EQ(transparent.size(), 1U);
    EXPECT_EQ(opaque[0].packet.pass, RenderPassKind::Opaque);
    EXPECT_EQ(masked[0].packet.pipelineKey.materialVariant,
              MaterialPipelineVariant::Masked);
    EXPECT_EQ(transparent[0].packet.pass, RenderPassKind::Transparent);
    const auto legacyItem = std::find_if(
        opaque.begin(), opaque.end(),
        [](const RenderDrawItem& item) { return item.objectIndex == 2U; });
    ASSERT_NE(legacyItem, opaque.end());
    EXPECT_EQ(legacyItem->submeshIndex, 0U);
}

TEST(RenderDrawPacketValidation,
     TransparentDrawOrderUsesFullStableIdentityForEqualDepthAndRejectsNonFiniteDepth)
{
    const auto makeTransparentObject = [](uint64 objectId,
                                          uint32 meshSlot,
                                          uint32 materialSlot,
                                          uint32 submeshIndex)
    {
        RenderObject object;
        object.entityId = objectId;
        object.meshBatchesAuthoritative = true;
        MeshBatch batch = MakeBatch(RenderMaterialMode::Transparent, submeshIndex);
        batch.objectId = objectId;
        batch.mesh = RenderResourceHandle{meshSlot, 2};
        batch.material = RenderResourceHandle{materialSlot, 3};
        object.meshBatches = {batch};
        return object;
    };

    RenderScene scene;
    // Deliberately insert in a different order than the identity tie-break.
    scene.AddObject(makeTransparentObject(30, 13, 23, 2));
    scene.AddObject(makeTransparentObject(10, 11, 21, 1));
    scene.AddObject(makeTransparentObject(20, 12, 22, 0));

    std::vector<RenderDrawItem> opaque;
    std::vector<RenderDrawItem> masked;
    std::vector<RenderDrawItem> transparent;
    TransparentDrawListDiagnostics firstDiagnostics;
    BuildMaterialDrawLists(scene, {2, 0, 1}, Vec3{0.0f}, opaque, masked,
                           transparent, &firstDiagnostics);

    ASSERT_EQ(3U, transparent.size());
    EXPECT_TRUE(firstDiagnostics.orderValid);
    EXPECT_EQ(0U, firstDiagnostics.rejectedNonFiniteDepthCount);
    EXPECT_EQ(10U, transparent[0].packet.objectId);
    EXPECT_EQ(20U, transparent[1].packet.objectId);
    EXPECT_EQ(30U, transparent[2].packet.objectId);
    EXPECT_TRUE(IsTransparentDrawListStrictlyOrdered(transparent));

    std::vector<RenderDrawItem> repeatedOpaque;
    std::vector<RenderDrawItem> repeatedMasked;
    std::vector<RenderDrawItem> repeatedTransparent;
    TransparentDrawListDiagnostics repeatedDiagnostics;
    BuildMaterialDrawLists(scene, {0, 1, 2}, Vec3{0.0f}, repeatedOpaque,
                           repeatedMasked, repeatedTransparent,
                           &repeatedDiagnostics);
    ASSERT_EQ(transparent.size(), repeatedTransparent.size());
    for (size_t index = 0; index < transparent.size(); ++index)
    {
        EXPECT_EQ(transparent[index].packet.objectId,
                  repeatedTransparent[index].packet.objectId);
        EXPECT_EQ(transparent[index].submeshIndex,
                  repeatedTransparent[index].submeshIndex);
        EXPECT_EQ(transparent[index].material,
                  repeatedTransparent[index].material);
        EXPECT_EQ(transparent[index].mesh,
                  repeatedTransparent[index].mesh);
    }
    EXPECT_EQ(firstDiagnostics.orderHash, repeatedDiagnostics.orderHash);

    TransparentDrawListDiagnostics invalidDepthDiagnostics;
    BuildMaterialDrawLists(scene, {0, 1, 2},
                           Vec3{std::numeric_limits<float32>::quiet_NaN()},
                           opaque, masked, transparent,
                           &invalidDepthDiagnostics);
    EXPECT_TRUE(transparent.empty());
    EXPECT_EQ(3U, invalidDepthDiagnostics.rejectedNonFiniteDepthCount);
    EXPECT_TRUE(invalidDepthDiagnostics.orderValid);
}
