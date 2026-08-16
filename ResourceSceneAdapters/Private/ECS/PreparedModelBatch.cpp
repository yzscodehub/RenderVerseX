#include "ResourceSceneAdapters/ECS/PreparedModelBatch.h"

#include "Geometry/Asset/Node.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/ModelResource.h"
#include "Scene/ECS/RetirementFragments.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace RVX::ResourceSceneAdapters
{
namespace
{
    using EntityHandle = ::RVX::ECS::EntityHandle;
    using EntityReceipt = ::RVX::ECS::EntityReceipt;

    [[nodiscard]] uint64 GetStructuralRevision(const ::RVX::ECS::Registry& registry)
    {
        const uint64 nextSequence = registry.GetStructuralJournal().GetNextSequence();
        return nextSequence == 0 ? 0 : nextSequence - 1u;
    }

    [[nodiscard]] RenderMaterialMode ToRenderMaterialMode(Resource::MaterialAlphaMode mode)
    {
        switch (mode)
        {
            case Resource::MaterialAlphaMode::Mask:
                return RenderMaterialMode::Masked;
            case Resource::MaterialAlphaMode::Blend:
                return RenderMaterialMode::Transparent;
            case Resource::MaterialAlphaMode::Opaque:
            default:
                return RenderMaterialMode::Opaque;
        }
    }

    [[nodiscard]] SceneECS::LocalTransform MakeLocalTransform(const Transform& source)
    {
        return {
            .translation = source.GetPosition(),
            .rotation = source.GetRotation(),
            .scale = source.GetScale(),
            .revision = 1,
        };
    }

    [[nodiscard]] SceneECS::Bounds MakeBounds(const Resource::MeshResource& mesh)
    {
        const AABB& source = mesh.GetBounds();
        if (!source.IsValid())
        {
            return {};
        }
        return {
            .center = source.GetCenter(),
            .extents = source.GetExtent(),
        };
    }

    struct SourceBuildState
    {
        const Resource::ModelResource& model;
        PreparedModelBatch& batch;
        PreparedModelBuildError error = PreparedModelBuildError::None;
        PreparedModelNodeId nextNodeId = 1;
        std::unordered_set<const Node*> visited;
    };

    [[nodiscard]] bool BuildMaterialSlots(const Resource::ModelResource& model,
                                          const std::vector<int>& materialIndices,
                                          size_t materialOffset,
                                          uint32 expectedSlotCount,
                                          SceneECS::MaterialSlots& destination,
                                          PreparedModelBuildError& error)
    {
        if (expectedSlotCount == 0 || expectedSlotCount > SceneECS::MaterialSlots::MaxSlotCount ||
            materialOffset > materialIndices.size() ||
            materialIndices.size() - materialOffset < expectedSlotCount)
        {
            error = expectedSlotCount > SceneECS::MaterialSlots::MaxSlotCount ?
                        PreparedModelBuildError::TooManyMaterialSlots :
                        PreparedModelBuildError::InvalidMaterialPrerequisite;
            return false;
        }

        destination.count = expectedSlotCount;
        for (uint32 slot = 0; slot < expectedSlotCount; ++slot)
        {
            const int materialIndex = materialIndices[materialOffset + slot];
            if (materialIndex < 0)
            {
                // A P3 batch cannot replace a missing source slot with an
                // implicit fallback without shifting every following binding.
                error = PreparedModelBuildError::InvalidMaterialPrerequisite;
                return false;
            }
            const auto material = model.GetMaterial(static_cast<size_t>(materialIndex));
            if (!material.IsValid() || !material.IsLoaded() || material.GetId() == Resource::InvalidResourceId)
            {
                error = PreparedModelBuildError::InvalidMaterialPrerequisite;
                return false;
            }
            destination.values[slot] = {
                .materialAssetId = {.value = material.GetId()},
                .materialMode = ToRenderMaterialMode(material->GetAlphaMode()),
            };
        }
        return true;
    }

    [[nodiscard]] bool AddMeshPayload(SourceBuildState& state,
                                      PreparedModelNode& destination,
                                      int meshIndex,
                                      const std::vector<int>& materialIndices,
                                      size_t materialOffset,
                                      bool requiresExactRemainder,
                                      const SceneECS::Visibility& visibility)
    {
        if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= state.model.GetMeshCount())
        {
            state.error = PreparedModelBuildError::UnresolvedMeshResource;
            return false;
        }

        const auto mesh = state.model.GetMesh(static_cast<size_t>(meshIndex));
        if (!mesh.IsValid() || !mesh.IsLoaded() || mesh.GetId() == Resource::InvalidResourceId)
        {
            state.error = PreparedModelBuildError::InvalidMeshPrerequisite;
            return false;
        }

        const size_t submeshCount = mesh->GetAssetMeshSubmeshCount();
        if (submeshCount > std::numeric_limits<uint32>::max())
        {
            state.error = PreparedModelBuildError::UnsupportedSourceMeshLayout;
            return false;
        }
        if (submeshCount == 0 || submeshCount > SceneECS::MaterialSlots::MaxSlotCount)
        {
            state.error = submeshCount > SceneECS::MaterialSlots::MaxSlotCount ?
                              PreparedModelBuildError::TooManyMaterialSlots :
                              PreparedModelBuildError::UnsupportedSourceMeshLayout;
            return false;
        }
        const uint32 expectedSlotCount = static_cast<uint32>(submeshCount);
        if (requiresExactRemainder && materialIndices.size() != expectedSlotCount)
        {
            state.error = PreparedModelBuildError::UnsupportedSourceMeshLayout;
            return false;
        }

        destination.hasMesh = true;
        destination.mesh = {
            .meshAssetId = {.value = mesh.GetId()},
            .submeshCount = static_cast<uint32>(submeshCount),
        };
        destination.bounds = MakeBounds(*mesh);
        destination.visibility = visibility;
        return BuildMaterialSlots(state.model,
                                  materialIndices,
                                  materialOffset,
                                  expectedSlotCount,
                                  destination.materialSlots,
                                  state.error);
    }

    [[nodiscard]] std::string MakeDerivedPrimitiveNodeName(
        std::string_view sourceName,
        PreparedModelSourceNodeIdentity sourceNodeIdentity,
        uint32 primitiveOrdinal)
    {
        // Keep sourceName intact for exact source-name queries. The explicit
        // source identity and ordinal make this diagnostic/runtime name stable
        // and unambiguous even when a model contains duplicate node names.
        return std::string(sourceName) + "::rvx-primitive@" +
               std::to_string(sourceNodeIdentity.value) + "[" +
               std::to_string(primitiveOrdinal) + "]";
    }

    [[nodiscard]] bool AddSourceNode(SourceBuildState& state,
                                     const Node& source,
                                     PreparedModelNodeId parentNodeId)
    {
        if (!state.visited.insert(&source).second)
        {
            state.error = PreparedModelBuildError::SourceHierarchyCycle;
            return false;
        }

        PreparedModelNode node;
        node.temporaryNodeId = state.nextNodeId++;
        node.parentTemporaryNodeId = parentNodeId;
        node.sourceName = source.GetName();
        node.sourceNodeIdentity = {.value = source.GetId()};
        node.nodeName = node.sourceName;
        node.localTransform = MakeLocalTransform(source.GetLocalTransform());
        node.active.value = source.IsActive();
        if (source.HasSkin())
        {
            const auto animation = state.model.GetAnimationResource();
            const Animation::Skeleton::ConstPtr skeleton =
                animation.IsValid() ? animation->GetSkeleton() : nullptr;
            if (!animation.IsValid() || !animation.IsLoaded() ||
                animation.GetId() == Resource::InvalidResourceId || !skeleton ||
                skeleton->bones.empty() ||
                skeleton->bones.size() > std::numeric_limits<uint32>::max())
            {
                state.error = PreparedModelBuildError::InvalidOptionalPayloadPrerequisite;
                return false;
            }
            node.optionalPayload = {
                .flags = PreparedModelOptionalPayload::SkeletonBinding,
                .sourceSkinIndex = source.GetSkinIndex(),
                .animationAssetId = {.value = animation.GetId()},
                .boneCount = static_cast<uint32>(skeleton->bones.size()),
            };
        }

        const PreparedModelNodeId sourceNodeId = node.temporaryNodeId;
        state.batch.nodes.push_back(node);
        PreparedModelNode& sourceDestination = state.batch.nodes.back();
        // Primitive children can grow batch.nodes and invalidate
        // sourceDestination. Copy the source identity before that growth so
        // derived identities never depend on a vector reference lifetime.
        const std::string sourceName = sourceDestination.sourceName;
        const PreparedModelSourceNodeIdentity sourceNodeIdentity =
            sourceDestination.sourceNodeIdentity;
        const bool sourceHasSkeletonBinding =
            (sourceDestination.optionalPayload.flags &
             PreparedModelOptionalPayload::SkeletonBinding) != 0;
        const std::vector<int>& materialIndices = source.GetMaterialIndices();
        const SceneECS::Visibility visibility{};

        std::vector<int> meshIndices;
        if (source.HasExplicitMeshIndices())
        {
            meshIndices = source.GetMeshIndices();
        }
        else if (source.GetMeshIndex() >= 0)
        {
            meshIndices.push_back(source.GetMeshIndex());
        }
        if (meshIndices.size() == 1)
        {
            if (!AddMeshPayload(state,
                                sourceDestination,
                                meshIndices.front(),
                                materialIndices,
                                0,
                                true,
                                visibility))
            {
                return false;
            }
            if (sourceHasSkeletonBinding)
            {
                sourceDestination.skinnedPoseOwnerTemporaryNodeId = sourceNodeId;
            }
        }
        else if (meshIndices.size() > 1)
        {
            if (materialIndices.size() != meshIndices.size())
            {
                state.error = PreparedModelBuildError::UnsupportedSourceMeshLayout;
                return false;
            }
            for (size_t index = 0; index < meshIndices.size(); ++index)
            {
                PreparedModelNode primitive;
                primitive.temporaryNodeId = state.nextNodeId++;
                primitive.parentTemporaryNodeId = sourceNodeId;
                primitive.sourceName = sourceName;
                primitive.sourceNodeIdentity = sourceNodeIdentity;
                primitive.derivedPrimitiveOrdinal = static_cast<uint32>(index);
                primitive.nodeName = MakeDerivedPrimitiveNodeName(
                    primitive.sourceName,
                    primitive.sourceNodeIdentity,
                    primitive.derivedPrimitiveOrdinal);
                primitive.active.value = source.IsActive();
                if (sourceHasSkeletonBinding)
                {
                    primitive.skinnedPoseOwnerTemporaryNodeId = sourceNodeId;
                }
                if (!AddMeshPayload(state,
                                    primitive,
                                    meshIndices[index],
                                    materialIndices,
                                    index,
                                    false,
                                    visibility))
                {
                    return false;
                }
                state.batch.nodes.push_back(primitive);
            }
        }

        for (const Node::Ptr& child : source.GetChildren())
        {
            if (child == nullptr || !AddSourceNode(state, *child, sourceNodeId))
            {
                if (state.error == PreparedModelBuildError::None)
                {
                    state.error = PreparedModelBuildError::SourceHierarchyCycle;
                }
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] PreparedModelAdoptionReceipt MakeRejectedReceipt(
        const ::RVX::ECS::Registry& registry,
        ::RVX::ECS::SceneRuntimeId sceneRuntimeId,
        PreparedModelAdoptionError error)
    {
        const uint64 revision = GetStructuralRevision(registry);
        return {
            .error = error,
            .transactionStatus = ::RVX::ECS::CommandStatus::Rejected,
            .transactionError = ::RVX::ECS::CommandError::OperationRejected,
            .sceneRuntimeId = sceneRuntimeId,
            .sceneStructuralRevisionBefore = revision,
            .sceneStructuralRevisionAfter = revision,
        };
    }

    [[nodiscard]] PreparedModelAdoptionError ValidateBatch(
        const PreparedModelBatch& batch,
        const PreparedModelAdoptionOptions& options,
        uint32 currentEntityCount,
        std::vector<const PreparedModelNode*>& orderedNodes)
    {
        if (batch.nodes.empty())
        {
            return PreparedModelAdoptionError::EmptyBatch;
        }
        if (options.requireAssetPrerequisites && !batch.sourceModelAssetId.IsValid())
        {
            return PreparedModelAdoptionError::InvalidSourceModelAssetPrerequisite;
        }
        if (batch.nodes.size() > options.maxAdditionalEntities ||
            batch.nodes.size() > std::numeric_limits<uint32>::max() ||
            currentEntityCount > std::numeric_limits<uint32>::max() -
                                     static_cast<uint32>(batch.nodes.size()))
        {
            return PreparedModelAdoptionError::EntityCapacityExceeded;
        }

        std::unordered_map<PreparedModelNodeId, const PreparedModelNode*> nodesById;
        nodesById.reserve(batch.nodes.size());
        orderedNodes.reserve(batch.nodes.size());
        uint32 rootCount = 0;
        for (const PreparedModelNode& node : batch.nodes)
        {
            if (node.temporaryNodeId == RVX_INVALID_PREPARED_MODEL_NODE_ID)
            {
                return PreparedModelAdoptionError::InvalidTemporaryNodeId;
            }
            if (!node.sourceNodeIdentity.IsValid())
            {
                return PreparedModelAdoptionError::InvalidSourceNodeIdentity;
            }
            if (!nodesById.emplace(node.temporaryNodeId, &node).second)
            {
                return PreparedModelAdoptionError::DuplicateTemporaryNodeId;
            }
            if (node.parentTemporaryNodeId == RVX_INVALID_PREPARED_MODEL_NODE_ID)
            {
                ++rootCount;
            }
            if (node.hasMesh)
            {
                if (options.requireAssetPrerequisites && !node.mesh.meshAssetId.IsValid())
                {
                    return PreparedModelAdoptionError::InvalidMeshAssetPrerequisite;
                }
                if (node.materialSlots.count > SceneECS::MaterialSlots::MaxSlotCount ||
                    node.mesh.submeshCount == 0 ||
                    node.materialSlots.count != node.mesh.submeshCount)
                {
                    return PreparedModelAdoptionError::InvalidMaterialSlotCount;
                }
                if (options.requireAssetPrerequisites)
                {
                    for (uint32 slot = 0; slot < node.materialSlots.count; ++slot)
                    {
                        if (!node.materialSlots.values[slot].materialAssetId.IsValid())
                        {
                            return PreparedModelAdoptionError::InvalidMaterialAssetPrerequisite;
                        }
                    }
                }
            }
            if ((node.optionalPayload.flags & ~PreparedModelOptionalPayload::KnownFlags) != 0 ||
                (node.optionalPayload.flags == 0 &&
                 (node.optionalPayload.sourceSkinIndex != -1 ||
                  node.optionalPayload.animationAssetId.IsValid() ||
                  node.optionalPayload.boneCount != 0)) ||
                ((node.optionalPayload.flags & PreparedModelOptionalPayload::SkeletonBinding) != 0 &&
                 (node.optionalPayload.sourceSkinIndex < 0 ||
                  node.optionalPayload.boneCount == 0 ||
                  (options.requireAssetPrerequisites &&
                   !node.optionalPayload.animationAssetId.IsValid()))))
            {
                return PreparedModelAdoptionError::InvalidOptionalPayloadPrerequisite;
            }
        }
        if (rootCount != 1)
        {
            return PreparedModelAdoptionError::MultipleRoots;
        }

        for (const PreparedModelNode& node : batch.nodes)
        {
            if (node.parentTemporaryNodeId == RVX_INVALID_PREPARED_MODEL_NODE_ID)
            {
                continue;
            }
            if (node.parentTemporaryNodeId == node.temporaryNodeId ||
                !nodesById.contains(node.parentTemporaryNodeId))
            {
                return node.parentTemporaryNodeId == node.temporaryNodeId ?
                           PreparedModelAdoptionError::HierarchyCycle :
                           PreparedModelAdoptionError::MissingParent;
            }

            std::unordered_set<PreparedModelNodeId> ancestors;
            PreparedModelNodeId current = node.parentTemporaryNodeId;
            while (current != RVX_INVALID_PREPARED_MODEL_NODE_ID)
            {
                if (!ancestors.insert(current).second || current == node.temporaryNodeId)
                {
                    return PreparedModelAdoptionError::HierarchyCycle;
                }
                const PreparedModelNode* ancestor = nodesById.at(current);
                current = ancestor->parentTemporaryNodeId;
            }
        }

        // A skinned primitive may only point to the exact source node that
        // owns the skeleton payload.  This preserves a single evaluator/pose
        // owner for every primitive without retaining source pointers.
        for (const PreparedModelNode& node : batch.nodes)
        {
            if (node.skinnedPoseOwnerTemporaryNodeId ==
                RVX_INVALID_PREPARED_MODEL_NODE_ID)
            {
                if (node.hasMesh &&
                    (node.optionalPayload.flags &
                     PreparedModelOptionalPayload::SkeletonBinding) != 0)
                {
                    return PreparedModelAdoptionError::InvalidSkinnedPoseOwner;
                }
                continue;
            }

            const auto ownerIt = nodesById.find(node.skinnedPoseOwnerTemporaryNodeId);
            if (!node.hasMesh || ownerIt == nodesById.end())
            {
                return PreparedModelAdoptionError::InvalidSkinnedPoseOwner;
            }

            const PreparedModelNode& poseOwner = *ownerIt->second;
            if ((poseOwner.optionalPayload.flags &
                 PreparedModelOptionalPayload::SkeletonBinding) == 0 ||
                poseOwner.sourceNodeIdentity != node.sourceNodeIdentity)
            {
                return PreparedModelAdoptionError::InvalidSkinnedPoseOwner;
            }

            if (node.derivedPrimitiveOrdinal ==
                RVX_INVALID_PREPARED_MODEL_DERIVED_PRIMITIVE_ORDINAL)
            {
                if (node.temporaryNodeId != poseOwner.temporaryNodeId)
                {
                    return PreparedModelAdoptionError::InvalidSkinnedPoseOwner;
                }
            }
            else if (node.parentTemporaryNodeId != poseOwner.temporaryNodeId)
            {
                return PreparedModelAdoptionError::InvalidSkinnedPoseOwner;
            }
        }

        for (const PreparedModelNode& node : batch.nodes)
        {
            orderedNodes.push_back(&node);
        }
        std::sort(orderedNodes.begin(), orderedNodes.end(), [](const PreparedModelNode* lhs,
                                                                const PreparedModelNode* rhs)
        {
            return lhs->temporaryNodeId < rhs->temporaryNodeId;
        });
        return PreparedModelAdoptionError::None;
    }
} // namespace

PreparedModelBatchBuildReceipt PreparedModelBatchBuilder::Build(const Resource::ModelResource& model)
{
    PreparedModelBatchBuildReceipt result;
    if (!model.IsLoaded())
    {
        result.error = PreparedModelBuildError::ModelNotPublished;
        return result;
    }
    const Node::Ptr root = model.GetRootNode();
    if (root == nullptr)
    {
        result.error = PreparedModelBuildError::MissingRootNode;
        return result;
    }

    result.batch.sourceModelAssetId = {.value = model.GetId()};
    SourceBuildState state{
        .model = model,
        .batch = result.batch,
    };
    if (!AddSourceNode(state, *root, RVX_INVALID_PREPARED_MODEL_NODE_ID))
    {
        result.batch.nodes.clear();
        result.error = state.error;
    }
    return result;
}

EntityHandle PreparedModelAdoptionReceipt::FindEntityByTemporaryNodeId(
    PreparedModelNodeId id) const
{
    const auto found = std::lower_bound(
        members.begin(), members.end(), id, [](const PreparedModelEntityMapping& mapping,
                                               PreparedModelNodeId targetId)
        {
            return mapping.temporaryNodeId < targetId;
        });
    return found != members.end() && found->temporaryNodeId == id ?
               found->entity :
               EntityHandle::Invalid();
}

std::vector<EntityHandle> PreparedModelAdoptionReceipt::FindEntitiesByExactSourceName(
    std::string_view sourceName) const
{
    std::vector<EntityHandle> result;
    for (const PreparedModelEntityMapping& member : members)
    {
        if (member.sourceName == sourceName)
        {
            result.push_back(member.entity);
        }
    }
    return result;
}

std::optional<EntityHandle> PreparedModelAdoptionReceipt::FindUniqueEntityByExactSourceName(
    std::string_view sourceName) const
{
    const std::vector<EntityHandle> matches = FindEntitiesByExactSourceName(sourceName);
    return matches.size() == 1 ? std::optional<EntityHandle>(matches.front()) : std::nullopt;
}

PreparedModelAdoptionReceipt AdoptPreparedModelBatch(
    SceneECS::SceneEcsRuntime& runtime,
    const PreparedModelBatch& batch,
    ::RVX::ECS::SceneRuntimeId expectedSceneRuntimeId,
    const PreparedModelAdoptionOptions& options)
{
    ::RVX::ECS::Registry& registry =
        runtime.GetMutableRegistryForPreparedModelAdoption();
    const ::RVX::ECS::SceneRuntimeId sceneRuntimeId = runtime.GetSceneRuntimeId();
    if (!expectedSceneRuntimeId.IsValid())
    {
        return MakeRejectedReceipt(registry, sceneRuntimeId,
                                   PreparedModelAdoptionError::InvalidExpectedSceneRuntime);
    }
    if (expectedSceneRuntimeId != runtime.GetSceneRuntimeId())
    {
        return MakeRejectedReceipt(registry, sceneRuntimeId,
                                   PreparedModelAdoptionError::SceneRuntimeMismatch);
    }

    std::vector<const PreparedModelNode*> orderedNodes;
    const PreparedModelAdoptionError validation = ValidateBatch(
        batch, options, registry.GetEntityCount(), orderedNodes);
    if (validation != PreparedModelAdoptionError::None)
    {
        return MakeRejectedReceipt(registry, sceneRuntimeId, validation);
    }

    PreparedModelAdoptionReceipt result;
    result.sceneRuntimeId = runtime.GetSceneRuntimeId();
    result.sceneStructuralRevisionBefore = GetStructuralRevision(registry);

    ::RVX::ECS::EntityTransaction transaction = registry.BeginTransaction();
    struct PendingNode
    {
        const PreparedModelNode* source = nullptr;
        EntityReceipt entity;
    };
    std::vector<PendingNode> pending;
    pending.reserve(orderedNodes.size());
    std::unordered_map<PreparedModelNodeId, size_t> pendingIndexById;
    pendingIndexById.reserve(orderedNodes.size());

    bool recorded = true;
    for (const PreparedModelNode* node : orderedNodes)
    {
        EntityReceipt entity = transaction.Create();
        recorded = recorded && entity.IsQueued();
        pendingIndexById.emplace(node->temporaryNodeId, pending.size());
        pending.push_back({.source = node, .entity = entity});
    }
    const auto rootPending = std::find_if(
        pending.begin(), pending.end(), [](const PendingNode& node)
        {
            return node.source->parentTemporaryNodeId == RVX_INVALID_PREPARED_MODEL_NODE_ID;
        });
    if (rootPending == pending.end())
    {
        return MakeRejectedReceipt(registry, sceneRuntimeId,
                                   PreparedModelAdoptionError::RecordingRejected);
    }

    for (const PendingNode& node : pending)
    {
        const PreparedModelNode& source = *node.source;
        recorded = recorded && transaction.Add<SceneECS::LocalTransform>(node.entity, source.localTransform).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::SimulationWorldTransform>(node.entity).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::PreviousSimulationWorldTransform>(node.entity).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::RenderWorldTransform>(node.entity).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::Bounds>(node.entity, source.bounds).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::Active>(node.entity, source.active).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::Layer>(node.entity, source.layer).IsQueued();
        recorded = recorded && transaction.Add<SceneECS::EntityLifecycleState>(node.entity).IsQueued();
        // Model-instance retirement is handled as one exact asset-owned set by
        // EcsSceneAssetLoadCoordinator.  Publish this marker in the same
        // transaction as every member so the generic Render coordinator can
        // never capture only a subset of this instance.
        recorded = recorded &&
                   transaction.Add<::RVX::ECS::Tag<SceneECS::SpecializedRenderRetirement>>(
                       node.entity).IsQueued();
        if (source.hasMesh)
        {
            recorded = recorded && transaction.Add<SceneECS::Mesh>(node.entity, source.mesh).IsQueued();
            recorded = recorded && transaction.Add<SceneECS::MaterialSlots>(node.entity, source.materialSlots).IsQueued();
            recorded = recorded && transaction.Add<SceneECS::Visibility>(node.entity, source.visibility).IsQueued();
        }
        if ((source.optionalPayload.flags & PreparedModelOptionalPayload::SkeletonBinding) != 0)
        {
            recorded = recorded && transaction.Add<SceneECS::AnimationSkeletonBinding>(
                                         node.entity,
                                         {
                                             .animationAssetValue =
                                                 source.optionalPayload.animationAssetId.value,
                                             .sourceModelAssetValue =
                                                 batch.sourceModelAssetId.value,
                                             .sourceSkinIndex = source.optionalPayload.sourceSkinIndex,
                                             .boneCount = source.optionalPayload.boneCount,
                                         }).IsQueued();
            recorded = recorded &&
                       transaction.Add<SceneECS::Animator>(node.entity).IsQueued();
            recorded = recorded &&
                       transaction.Add<SceneECS::AnimationPoseState>(node.entity).IsQueued();
        }
        if (source.skinnedPoseOwnerTemporaryNodeId !=
            RVX_INVALID_PREPARED_MODEL_NODE_ID)
        {
            const PendingNode& poseOwner = pending[pendingIndexById.at(
                source.skinnedPoseOwnerTemporaryNodeId)];
            recorded = recorded && transaction.AddLinked<
                SceneECS::SkinnedMeshBinding,
                &SceneECS::SkinnedMeshBinding::poseEntity>(
                node.entity,
                poseOwner.entity,
                {
                    .sourceModelAssetValue = batch.sourceModelAssetId.value,
                    .sourceSkinIndex = poseOwner.source->optionalPayload.sourceSkinIndex,
                }).IsQueued();
        }
        if (!source.active.value)
        {
            recorded = recorded && transaction.SetEnabled(node.entity, false).IsQueued();
        }
    }

    for (const PendingNode& child : pending)
    {
        if (child.source->parentTemporaryNodeId == RVX_INVALID_PREPARED_MODEL_NODE_ID)
        {
            continue;
        }
        const PendingNode& parent = pending[pendingIndexById.at(child.source->parentTemporaryNodeId)];
        recorded = recorded && transaction.AddLinked<SceneECS::ParentRelation,
                                                       &SceneECS::ParentRelation::parent>(
                                   child.entity, parent.entity).IsQueued();
    }

    const uint32 memberCount = static_cast<uint32>(pending.size());
    recorded = recorded && transaction.Add<AssetInstanceFragment>(
                                 rootPending->entity,
                                 {
                                     .sourceAssetId = batch.sourceModelAssetId,
                                     .rootTemporaryNodeId = rootPending->source->temporaryNodeId,
                                     .memberCount = memberCount,
                                 }).IsQueued();
    recorded = recorded && transaction.Add<ModelInstanceFragment>(
                                 rootPending->entity,
                                 {
                                     .sourceModelAssetId = batch.sourceModelAssetId,
                                     .rootTemporaryNodeId = rootPending->source->temporaryNodeId,
                                     .memberCount = memberCount,
                                 }).IsQueued();
    for (const PendingNode& node : pending)
    {
        recorded = recorded && transaction.AddLinked<ModelInstanceMemberFragment,
                                                       &ModelInstanceMemberFragment::instanceRoot>(
                                   node.entity,
                                   rootPending->entity,
                                   {.temporaryNodeId = node.source->temporaryNodeId}).IsQueued();
    }

    if (recorded && options.fault == PreparedModelAdoptionFault::RejectAfterRecording)
    {
        // A duplicate fragment add deterministically rejects staging, proving no
        // earlier entity or hierarchy command can leak from the transaction.
        recorded = transaction.Add<SceneECS::LocalTransform>(pending.front().entity).IsQueued();
    }

    if (!recorded)
    {
        return MakeRejectedReceipt(registry, sceneRuntimeId,
                                   PreparedModelAdoptionError::RecordingRejected);
    }

    const ::RVX::ECS::TransactionReceipt transactionResult = transaction.Commit();
    result.transactionStatus = transactionResult.GetStatus();
    result.transactionError = transactionResult.GetError();
    result.sceneStructuralRevisionAfter = GetStructuralRevision(registry);
    if (!transactionResult.IsApplied())
    {
        result.error = PreparedModelAdoptionError::TransactionRejected;
        result.sceneStructuralRevisionAfter = result.sceneStructuralRevisionBefore;
        return result;
    }

    result.members.reserve(pending.size());
    for (const PendingNode& node : pending)
    {
        if (!node.entity.IsResolved())
        {
            result.error = PreparedModelAdoptionError::TransactionRejected;
            result.transactionStatus = ::RVX::ECS::CommandStatus::Rejected;
            result.transactionError = ::RVX::ECS::CommandError::TransactionAborted;
            result.members.clear();
            result.rootEntity = EntityHandle::Invalid();
            return result;
        }
        const EntityHandle entity = node.entity.GetEntity();
        result.members.push_back({
            .temporaryNodeId = node.source->temporaryNodeId,
            .entity = entity,
            .sourceName = node.source->sourceName,
            .sourceNodeIdentity = node.source->sourceNodeIdentity,
            .nodeName = node.source->nodeName,
            .derivedPrimitiveOrdinal = node.source->derivedPrimitiveOrdinal,
        });
        if (node.source->parentTemporaryNodeId == RVX_INVALID_PREPARED_MODEL_NODE_ID)
        {
            result.rootEntity = entity;
        }
    }
    result.error = PreparedModelAdoptionError::None;
    return result;
}
} // namespace RVX::ResourceSceneAdapters
