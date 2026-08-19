#include "ResourceSceneAdapters/ECS/PreparedModelBatch.h"

#include "Geometry/Asset/Mesh.h"
#include "Geometry/Asset/Node.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/ModelResource.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "Scene/ECS/RetirementFragments.h"

namespace
{
    using namespace RVX;
    using namespace RVX::ResourceSceneAdapters;

    class TestModelResource final : public Resource::ModelResource
    {
    public:
        void MarkLoaded() { SetState(ResourceState::Loaded); }
    };

    class TestMeshResource final : public Resource::MeshResource
    {
    public:
        void MarkLoaded() { SetState(ResourceState::Loaded); }
    };

    class TestMaterialResource final : public Resource::MaterialResource
    {
    public:
        void MarkLoaded() { SetState(ResourceState::Loaded); }
    };

    Resource::ResourceHandle<Resource::MeshResource> MakeLoadedMesh(Resource::ResourceId id)
    {
        auto* mesh = new TestMeshResource();
        mesh->SetId(id);
        mesh->SetMesh(MeshFactory::CreateTriangle());
        mesh->MarkLoaded();
        return Resource::ResourceHandle<Resource::MeshResource>(mesh);
    }

    Resource::ResourceHandle<Resource::MaterialResource> MakeLoadedMaterial(Resource::ResourceId id)
    {
        auto* material = new TestMaterialResource();
        material->SetId(id);
        material->MarkLoaded();
        return Resource::ResourceHandle<Resource::MaterialResource>(material);
    }

    PreparedModelNode MakeNode(PreparedModelNodeId id,
                               PreparedModelNodeId parent = RVX_INVALID_PREPARED_MODEL_NODE_ID)
    {
        PreparedModelNode node;
        node.temporaryNodeId = id;
        node.parentTemporaryNodeId = parent;
        node.sourceNodeIdentity = {.value = static_cast<uint64>(id)};
        node.localTransform.translation = {static_cast<float>(id), 0.0f, 0.0f};
        return node;
    }

    PreparedModelBatch MakeThreeNodeBatch()
    {
        PreparedModelBatch batch;
        batch.sourceModelAssetId = {.value = 9001};
        batch.nodes = {
            MakeNode(30, 10),
            MakeNode(10),
            MakeNode(20, 10),
        };
        return batch;
    }

    PreparedModelBatch MakeExactMaterialBatch()
    {
        PreparedModelBatch batch;
        batch.sourceModelAssetId = {.value = 9002};
        PreparedModelNode root = MakeNode(10);
        root.hasMesh = true;
        root.mesh = {
            .meshAssetId = {.value = 101},
            .submeshCount = 2,
        };
        root.materialSlots.count = 2;
        root.materialSlots.values[0].materialAssetId = {.value = 201};
        root.materialSlots.values[1].materialAssetId = {.value = 202};
        batch.nodes.push_back(root);
        return batch;
    }

    void SetSourceIdentity(PreparedModelNode& node,
                           std::string sourceName,
                           uint64 sourceNodeIdentity)
    {
        node.sourceName = std::move(sourceName);
        node.nodeName = node.sourceName;
        node.sourceNodeIdentity = {.value = sourceNodeIdentity};
    }
} // namespace

TEST(EcsModelBatchValidation, PreservesExactSourceNamesAndReturnsDuplicateAndEmptyMatches)
{
    SceneECS::SceneEcsRuntime runtime;
    PreparedModelBatch batch;
    batch.sourceModelAssetId = {.value = 9000};
    PreparedModelNode root = MakeNode(10);
    SetSourceIdentity(root, "Hull", 1001);
    PreparedModelNode firstDuplicate = MakeNode(20, 10);
    SetSourceIdentity(firstDuplicate, "Panel", 1002);
    PreparedModelNode secondDuplicate = MakeNode(30, 10);
    SetSourceIdentity(secondDuplicate, "Panel", 1003);
    PreparedModelNode emptyName = MakeNode(40, 10);
    SetSourceIdentity(emptyName, "", 1004);
    batch.nodes = {secondDuplicate, root, emptyName, firstDuplicate};

    const PreparedModelAdoptionReceipt adopted = AdoptPreparedModelBatch(
        runtime, batch, runtime.GetSceneRuntimeId());
    ASSERT_TRUE(adopted.IsApplied());
    ASSERT_EQ(adopted.members.size(), 4u);
    EXPECT_EQ(adopted.members[0].temporaryNodeId, 10u);
    EXPECT_EQ(adopted.members[1].temporaryNodeId, 20u);
    EXPECT_EQ(adopted.members[2].temporaryNodeId, 30u);
    EXPECT_EQ(adopted.members[3].temporaryNodeId, 40u);
    EXPECT_EQ(adopted.members[2].sourceName, "Panel");
    EXPECT_EQ(adopted.members[2].sourceNodeIdentity.value, 1003u);

    const auto panelEntities = adopted.FindEntitiesByExactSourceName("Panel");
    ASSERT_EQ(panelEntities.size(), 2u);
    EXPECT_EQ(panelEntities[0], adopted.FindEntityByTemporaryNodeId(20));
    EXPECT_EQ(panelEntities[1], adopted.FindEntityByTemporaryNodeId(30));
    EXPECT_FALSE(adopted.FindUniqueEntityByExactSourceName("Panel").has_value());
    ASSERT_TRUE(adopted.FindUniqueEntityByExactSourceName("Hull").has_value());
    EXPECT_EQ(*adopted.FindUniqueEntityByExactSourceName("Hull"), adopted.rootEntity);
    const auto emptyEntities = adopted.FindEntitiesByExactSourceName("");
    ASSERT_EQ(emptyEntities.size(), 1u);
    EXPECT_EQ(emptyEntities.front(), adopted.FindEntityByTemporaryNodeId(40));
}

TEST(EcsModelBatchValidation, BuilderCopiesSourceIdentityAndSeparatesDerivedPrimitives)
{
    TestModelResource model;
    model.SetId(9010);
    model.AddMesh(MakeLoadedMesh(101));
    model.AddMesh(MakeLoadedMesh(102));
    model.AddMaterial(MakeLoadedMaterial(201));
    model.AddMaterial(MakeLoadedMaterial(202));

    auto root = std::make_shared<Node>("Source");
    const uint32 rootId = root->GetId();
    root->SetActive(false);
    root->SetMeshIndices({0, 1});
    root->SetMaterialIndices({0, 1});
    EXPECT_TRUE(root->HasMeshData());
    auto firstDuplicate = std::make_shared<Node>("Duplicate");
    auto secondDuplicate = std::make_shared<Node>("Duplicate");
    auto emptyName = std::make_shared<Node>("");
    const uint32 firstDuplicateId = firstDuplicate->GetId();
    const uint32 secondDuplicateId = secondDuplicate->GetId();
    const uint32 emptyNameId = emptyName->GetId();
    root->AddChild(firstDuplicate);
    root->AddChild(secondDuplicate);
    root->AddChild(emptyName);
    model.SetRootNode(root);
    model.MarkLoaded();

    const PreparedModelBatchBuildReceipt first = PreparedModelBatchBuilder::Build(model);
    const PreparedModelBatchBuildReceipt second = PreparedModelBatchBuilder::Build(model);
    ASSERT_TRUE(first.IsPrepared());
    ASSERT_TRUE(second.IsPrepared());
    ASSERT_EQ(first.batch.nodes.size(), 6u);
    ASSERT_EQ(second.batch.nodes.size(), first.batch.nodes.size());

    const PreparedModelNode& source = first.batch.nodes[0];
    const PreparedModelNode& primitive0 = first.batch.nodes[1];
    const PreparedModelNode& primitive1 = first.batch.nodes[2];
    EXPECT_EQ(source.sourceName, "Source");
    EXPECT_EQ(source.nodeName, "Source");
    EXPECT_EQ(source.sourceNodeIdentity.value, rootId);
    EXPECT_EQ(source.derivedPrimitiveOrdinal,
              RVX_INVALID_PREPARED_MODEL_DERIVED_PRIMITIVE_ORDINAL);
    EXPECT_FALSE(source.active.value);
    EXPECT_EQ(primitive0.sourceName, "Source");
    EXPECT_EQ(primitive0.sourceNodeIdentity.value, rootId);
    EXPECT_EQ(primitive0.derivedPrimitiveOrdinal, 0u);
    EXPECT_EQ(primitive1.derivedPrimitiveOrdinal, 1u);
    EXPECT_FALSE(primitive0.active.value);
    EXPECT_FALSE(primitive1.active.value);
    EXPECT_TRUE(primitive0.visibility.visible);
    EXPECT_TRUE(primitive0.visibility.castsShadow);
    EXPECT_TRUE(primitive0.visibility.receivesShadow);
    EXPECT_NE(primitive0.nodeName, source.nodeName);
    EXPECT_NE(primitive0.nodeName, primitive1.nodeName);
    EXPECT_EQ(primitive0.nodeName, "Source::rvx-primitive@" + std::to_string(rootId) + "[0]");
    EXPECT_EQ(primitive1.nodeName, "Source::rvx-primitive@" + std::to_string(rootId) + "[1]");

    EXPECT_EQ(first.batch.nodes[3].sourceName, "Duplicate");
    EXPECT_EQ(first.batch.nodes[3].sourceNodeIdentity.value, firstDuplicateId);
    EXPECT_EQ(first.batch.nodes[4].sourceName, "Duplicate");
    EXPECT_EQ(first.batch.nodes[4].sourceNodeIdentity.value, secondDuplicateId);
    EXPECT_EQ(first.batch.nodes[5].sourceName, "");
    EXPECT_EQ(first.batch.nodes[5].sourceNodeIdentity.value, emptyNameId);
    for (size_t index = 0; index < first.batch.nodes.size(); ++index)
    {
        EXPECT_EQ(first.batch.nodes[index].temporaryNodeId, second.batch.nodes[index].temporaryNodeId);
        EXPECT_EQ(first.batch.nodes[index].sourceName, second.batch.nodes[index].sourceName);
        EXPECT_EQ(first.batch.nodes[index].sourceNodeIdentity,
                  second.batch.nodes[index].sourceNodeIdentity);
        EXPECT_EQ(first.batch.nodes[index].nodeName, second.batch.nodes[index].nodeName);
        EXPECT_EQ(first.batch.nodes[index].derivedPrimitiveOrdinal,
                  second.batch.nodes[index].derivedPrimitiveOrdinal);
    }
}

TEST(EcsModelBatchValidation, AdoptsMultiNodeHierarchyWithDeterministicMappings)
{
    SceneECS::SceneEcsRuntime runtime;
    const PreparedModelBatch batch = MakeThreeNodeBatch();

    const PreparedModelAdoptionReceipt result = AdoptPreparedModelBatch(
        runtime, batch, runtime.GetSceneRuntimeId());

    ASSERT_TRUE(result.IsApplied());
    ASSERT_EQ(result.members.size(), 3u);
    EXPECT_LT(result.sceneStructuralRevisionBefore, result.sceneStructuralRevisionAfter);
    EXPECT_EQ(result.members[0].temporaryNodeId, 10u);
    EXPECT_EQ(result.members[1].temporaryNodeId, 20u);
    EXPECT_EQ(result.members[2].temporaryNodeId, 30u);
    EXPECT_EQ(result.rootEntity, result.FindEntity(10));
    EXPECT_EQ(runtime.GetTransformHierarchy().GetParent(result.FindEntity(20)), result.rootEntity);
    EXPECT_EQ(runtime.GetTransformHierarchy().GetParent(result.FindEntity(30)), result.rootEntity);

    const auto children = runtime.GetTransformHierarchy().GetChildren(result.rootEntity);
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0], result.FindEntity(20));
    EXPECT_EQ(children[1], result.FindEntity(30));
    runtime.ResolveSimulationTransforms();
    const auto* childWorld =
        runtime.GetRegistry().TryGet<SceneECS::SimulationWorldTransform>(result.FindEntity(20));
    ASSERT_NE(childWorld, nullptr);
    EXPECT_NEAR(childWorld->matrix[3].x, 30.0f, 0.0001f);

    const auto* assetInstance =
        runtime.GetRegistry().TryGet<AssetInstanceFragment>(result.rootEntity);
    const auto* modelInstance =
        runtime.GetRegistry().TryGet<ModelInstanceFragment>(result.rootEntity);
    ASSERT_NE(assetInstance, nullptr);
    ASSERT_NE(modelInstance, nullptr);
    EXPECT_EQ(assetInstance->sourceAssetId, batch.sourceModelAssetId);
    EXPECT_EQ(assetInstance->rootTemporaryNodeId, 10u);
    EXPECT_EQ(assetInstance->memberCount, 3u);
    EXPECT_EQ(modelInstance->sourceModelAssetId, batch.sourceModelAssetId);
    for (const PreparedModelEntityMapping& member : result.members)
    {
        const auto* instanceMember =
            runtime.GetRegistry().TryGet<ModelInstanceMemberFragment>(member.entity);
        ASSERT_NE(instanceMember, nullptr);
        EXPECT_EQ(instanceMember->instanceRoot, result.rootEntity);
        EXPECT_EQ(instanceMember->temporaryNodeId, member.temporaryNodeId);
        EXPECT_TRUE(runtime.GetRegistry().HasTag<SceneECS::SpecializedRenderRetirement>(
            member.entity));
    }
}

TEST(EcsModelBatchValidation, PreservesExactMaterialSlotPositionsAndRejectsMissingSlots)
{
    SceneECS::SceneEcsRuntime runtime;
    const PreparedModelBatch exact = MakeExactMaterialBatch();
    const PreparedModelAdoptionReceipt result = AdoptPreparedModelBatch(
        runtime, exact, runtime.GetSceneRuntimeId());
    ASSERT_TRUE(result.IsApplied());
    const auto* materialSlots =
        runtime.GetRegistry().TryGet<SceneECS::MaterialSlots>(result.rootEntity);
    ASSERT_NE(materialSlots, nullptr);
    ASSERT_EQ(materialSlots->count, 2u);
    EXPECT_EQ(materialSlots->values[0].materialAssetId.value, 201u);
    EXPECT_EQ(materialSlots->values[1].materialAssetId.value, 202u);

    PreparedModelBatch missing = MakeExactMaterialBatch();
    missing.nodes.front().materialSlots.count = 1;
    const PreparedModelAdoptionReceipt missingResult = AdoptPreparedModelBatch(
        runtime, missing, runtime.GetSceneRuntimeId());
    EXPECT_EQ(missingResult.error, PreparedModelAdoptionError::InvalidMaterialSlotCount);

    PreparedModelBatch invalidPosition = MakeExactMaterialBatch();
    invalidPosition.nodes.front().materialSlots.values[1].materialAssetId = {};
    const PreparedModelAdoptionReceipt invalidPositionResult = AdoptPreparedModelBatch(
        runtime, invalidPosition, runtime.GetSceneRuntimeId());
    EXPECT_EQ(invalidPositionResult.error,
              PreparedModelAdoptionError::InvalidMaterialAssetPrerequisite);
}

TEST(EcsModelBatchValidation, PublishesSkeletonBindingAndRejectsMissingInstancePrerequisites)
{
    SceneECS::SceneEcsRuntime runtime;
    PreparedModelBatch skinned = MakeThreeNodeBatch();
    PreparedModelNode& root = skinned.nodes[1];
    root.hasMesh = true;
    root.mesh = {
        .meshAssetId = {.value = 111},
        .submeshCount = 1,
    };
    root.materialSlots.count = 1;
    root.materialSlots.values[0].materialAssetId = {.value = 211};
    root.optionalPayload = {
        .flags = PreparedModelOptionalPayload::SkeletonBinding,
        .sourceSkinIndex = 4,
        .animationAssetId = {.value = 301},
        .boneCount = 24,
    };
    root.skinnedPoseOwnerTemporaryNodeId = root.temporaryNodeId;
    const PreparedModelAdoptionReceipt result = AdoptPreparedModelBatch(
        runtime, skinned, runtime.GetSceneRuntimeId());
    ASSERT_TRUE(result.IsApplied());
    const auto* skeleton = runtime.GetRegistry().TryGet<SceneECS::AnimationSkeletonBinding>(
        result.rootEntity);
    ASSERT_NE(skeleton, nullptr);
    EXPECT_EQ(skeleton->sourceSkinIndex, 4);
    EXPECT_EQ(skeleton->animationAssetValue, 301u);
    EXPECT_EQ(skeleton->sourceModelAssetValue, 9001u);
    EXPECT_EQ(skeleton->boneCount, 24u);
    EXPECT_NE(runtime.GetRegistry().TryGet<SceneECS::Animator>(result.rootEntity), nullptr);
    EXPECT_NE(runtime.GetRegistry().TryGet<SceneECS::AnimationPoseState>(result.rootEntity),
              nullptr);
    const auto* skinning = runtime.GetRegistry().TryGet<SceneECS::SkinnedMeshBinding>(
        result.rootEntity);
    ASSERT_NE(skinning, nullptr);
    EXPECT_EQ(skinning->poseEntity, result.rootEntity);
    EXPECT_EQ(skinning->sourceModelAssetValue, 9001u);
    EXPECT_EQ(skinning->sourceSkinIndex, 4);

    PreparedModelBatch missingSourceAsset = MakeThreeNodeBatch();
    missingSourceAsset.sourceModelAssetId = {};
    EXPECT_EQ(AdoptPreparedModelBatch(runtime,
                                      missingSourceAsset,
                                      runtime.GetSceneRuntimeId()).error,
              PreparedModelAdoptionError::InvalidSourceModelAssetPrerequisite);

    PreparedModelBatch missingSkeletonAsset = MakeThreeNodeBatch();
    missingSkeletonAsset.nodes[1].optionalPayload = {
        .flags = PreparedModelOptionalPayload::SkeletonBinding,
        .sourceSkinIndex = 0,
        .boneCount = 24,
    };
    EXPECT_EQ(AdoptPreparedModelBatch(runtime,
                                      missingSkeletonAsset,
                                      runtime.GetSceneRuntimeId()).error,
              PreparedModelAdoptionError::InvalidOptionalPayloadPrerequisite);
}

TEST(EcsModelBatchValidation,
     AdoptsDerivedSkinnedPrimitivesWithOneExactPoseOwnerAndRejectsBrokenLinks)
{
    SceneECS::SceneEcsRuntime runtime;
    PreparedModelBatch batch;
    batch.sourceModelAssetId = {.value = 9011};

    PreparedModelNode poseOwner = MakeNode(10);
    poseOwner.optionalPayload = {
        .flags = PreparedModelOptionalPayload::SkeletonBinding,
        .sourceSkinIndex = 6,
        .animationAssetId = {.value = 302},
        .boneCount = 24,
    };

    const auto makePrimitive = [](PreparedModelNodeId id, uint32 ordinal)
    {
        PreparedModelNode primitive = MakeNode(id, 10);
        primitive.sourceNodeIdentity = {.value = 10};
        primitive.derivedPrimitiveOrdinal = ordinal;
        primitive.skinnedPoseOwnerTemporaryNodeId = 10;
        primitive.hasMesh = true;
        primitive.mesh = {
            .meshAssetId = {.value = 112 + ordinal},
            .submeshCount = 1,
        };
        primitive.materialSlots.count = 1;
        primitive.materialSlots.values[0].materialAssetId = {.value = 212 + ordinal};
        return primitive;
    };
    batch.nodes = {poseOwner, makePrimitive(11, 0), makePrimitive(12, 1)};

    const PreparedModelAdoptionReceipt adopted = AdoptPreparedModelBatch(
        runtime, batch, runtime.GetSceneRuntimeId());
    ASSERT_TRUE(adopted.IsApplied());
    const ECS::EntityHandle poseEntity = adopted.FindEntity(10);
    ASSERT_TRUE(poseEntity.IsValid());
    EXPECT_NE(runtime.GetRegistry().TryGet<SceneECS::AnimationSkeletonBinding>(poseEntity),
              nullptr);
    EXPECT_NE(runtime.GetRegistry().TryGet<SceneECS::Animator>(poseEntity), nullptr);
    EXPECT_NE(runtime.GetRegistry().TryGet<SceneECS::AnimationPoseState>(poseEntity), nullptr);

    for (const PreparedModelNodeId primitiveId : {11u, 12u})
    {
        const ECS::EntityHandle primitiveEntity = adopted.FindEntity(primitiveId);
        const auto* binding = runtime.GetRegistry().TryGet<SceneECS::SkinnedMeshBinding>(
            primitiveEntity);
        ASSERT_NE(binding, nullptr);
        EXPECT_EQ(binding->poseEntity, poseEntity);
        EXPECT_EQ(binding->sourceModelAssetValue, 9011u);
        EXPECT_EQ(binding->sourceSkinIndex, 6);
        EXPECT_EQ(runtime.GetRegistry().TryGet<SceneECS::Animator>(primitiveEntity), nullptr);
        EXPECT_EQ(runtime.GetRegistry().TryGet<SceneECS::AnimationPoseState>(primitiveEntity),
                  nullptr);
    }

    PreparedModelBatch broken = batch;
    broken.nodes[1].skinnedPoseOwnerTemporaryNodeId = 99;
    const uint32 baselineCount = runtime.GetRegistry().GetEntityCount();
    const uint64 baselineRevision =
        runtime.GetRegistry().GetStructuralJournal().GetNextSequence() - 1u;
    const PreparedModelAdoptionReceipt rejected = AdoptPreparedModelBatch(
        runtime, broken, runtime.GetSceneRuntimeId());
    EXPECT_EQ(rejected.error, PreparedModelAdoptionError::InvalidSkinnedPoseOwner);
    EXPECT_EQ(rejected.sceneStructuralRevisionBefore, baselineRevision);
    EXPECT_EQ(rejected.sceneStructuralRevisionAfter, baselineRevision);
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), baselineCount);
}

TEST(EcsModelBatchValidation, RejectsDuplicateCyclesAndMissingParentsBeforePublication)
{
    SceneECS::SceneEcsRuntime runtime;
    const uint32 baseline = runtime.GetRegistry().GetEntityCount();

    PreparedModelBatch duplicate;
    duplicate.sourceModelAssetId = {.value = 9101};
    duplicate.nodes = {MakeNode(1), MakeNode(1)};
    EXPECT_EQ(AdoptPreparedModelBatch(runtime, duplicate, runtime.GetSceneRuntimeId()).error,
              PreparedModelAdoptionError::DuplicateTemporaryNodeId);

    PreparedModelBatch missingParent;
    missingParent.sourceModelAssetId = {.value = 9102};
    missingParent.nodes = {MakeNode(1), MakeNode(2, 99)};
    EXPECT_EQ(AdoptPreparedModelBatch(runtime, missingParent, runtime.GetSceneRuntimeId()).error,
              PreparedModelAdoptionError::MissingParent);

    PreparedModelBatch cycle;
    cycle.sourceModelAssetId = {.value = 9103};
    cycle.nodes = {MakeNode(1, 2), MakeNode(2, 1), MakeNode(3)};
    EXPECT_EQ(AdoptPreparedModelBatch(runtime, cycle, runtime.GetSceneRuntimeId()).error,
              PreparedModelAdoptionError::HierarchyCycle);
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), baseline);
}

TEST(EcsModelBatchValidation, RejectsInvalidSourceNodeIdentityBeforePublication)
{
    SceneECS::SceneEcsRuntime runtime;
    PreparedModelBatch batch = MakeThreeNodeBatch();
    batch.nodes[1].sourceNodeIdentity = {};

    const uint32 baselineEntityCount = runtime.GetRegistry().GetEntityCount();
    const uint64 baselineRevision =
        runtime.GetRegistry().GetStructuralJournal().GetNextSequence() - 1u;

    const PreparedModelAdoptionReceipt result = AdoptPreparedModelBatch(
        runtime, batch, runtime.GetSceneRuntimeId());

    EXPECT_EQ(result.error, PreparedModelAdoptionError::InvalidSourceNodeIdentity);
    EXPECT_TRUE(result.members.empty());
    EXPECT_FALSE(result.rootEntity.IsValid());
    EXPECT_EQ(result.sceneStructuralRevisionBefore, baselineRevision);
    EXPECT_EQ(result.sceneStructuralRevisionAfter, baselineRevision);
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), baselineEntityCount);
}

TEST(EcsModelBatchValidation, RejectsForeignRuntimeAndRollsBackForcedTransactionFailure)
{
    SceneECS::SceneEcsRuntime runtime;
    SceneECS::SceneEcsRuntime foreignRuntime;
    const PreparedModelBatch batch = MakeThreeNodeBatch();
    const uint32 baseline = runtime.GetRegistry().GetEntityCount();
    const uint64 revision = runtime.GetRegistry().GetStructuralJournal().GetNextSequence() - 1u;

    const PreparedModelAdoptionReceipt foreign = AdoptPreparedModelBatch(
        runtime, batch, foreignRuntime.GetSceneRuntimeId());
    EXPECT_EQ(foreign.error, PreparedModelAdoptionError::SceneRuntimeMismatch);
    EXPECT_TRUE(foreign.members.empty());

    PreparedModelAdoptionOptions options;
    options.fault = PreparedModelAdoptionFault::RejectAfterRecording;
    const PreparedModelAdoptionReceipt failed = AdoptPreparedModelBatch(
        runtime, batch, runtime.GetSceneRuntimeId(), options);
    EXPECT_EQ(failed.error, PreparedModelAdoptionError::TransactionRejected);
    EXPECT_EQ(failed.transactionStatus, ECS::CommandStatus::Rejected);
    EXPECT_TRUE(failed.members.empty());
    EXPECT_FALSE(failed.rootEntity.IsValid());
    EXPECT_EQ(failed.sceneStructuralRevisionBefore, revision);
    EXPECT_EQ(failed.sceneStructuralRevisionAfter, revision);
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), baseline);
}
