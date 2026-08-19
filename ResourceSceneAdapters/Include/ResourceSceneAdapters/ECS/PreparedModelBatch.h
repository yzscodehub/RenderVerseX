#pragma once

/**
 * @file PreparedModelBatch.h
 * @brief Value-only preparation and atomic ECS adoption for published models.
 */

#include "ECS/Commands.h"
#include "Scene/ECS/AnimationFragments.h"
#include "Scene/ECS/Fragments.h"
#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <compare>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RVX::Resource
{
    class ModelResource;
}

namespace RVX::ResourceSceneAdapters
{
    /** @brief Batch-local source node identity; zero is never a valid node. */
    using PreparedModelNodeId = uint64;
    inline constexpr PreparedModelNodeId RVX_INVALID_PREPARED_MODEL_NODE_ID = 0;

    /**
     * @brief Stable identity copied from Geometry::Node for one published ModelResource.
     *
     * This is deliberately a value copied at preparation time. It is neither a
     * pointer nor a process-local lookup key after the batch leaves Resource.
     */
    struct PreparedModelSourceNodeIdentity
    {
        uint64 value = 0;

        [[nodiscard]] bool IsValid() const noexcept { return value != 0; }

        auto operator<=>(const PreparedModelSourceNodeIdentity&) const = default;
    };

    inline constexpr uint32 RVX_INVALID_PREPARED_MODEL_DERIVED_PRIMITIVE_ORDINAL =
        std::numeric_limits<uint32>::max();

    /** @brief Optional source data atomically translated into subsystem-owned ECS fragments. */
    struct PreparedModelOptionalPayload
    {
        static constexpr uint32 SkeletonBinding = 1u << 0u;
        static constexpr uint32 KnownFlags = SkeletonBinding;

        uint32 flags = 0;
        int32 sourceSkinIndex = -1;
        AssetId animationAssetId{};
        uint32 boneCount = 0;
    };

    /** @brief Root-owned generic asset-instance identity for future residency and retirement processors. */
    struct AssetInstanceFragment
    {
        AssetId sourceAssetId{};
        PreparedModelNodeId rootTemporaryNodeId = RVX_INVALID_PREPARED_MODEL_NODE_ID;
        uint32 memberCount = 0;
    };

    /** @brief Root-owned model specialization retaining the exact source asset and batch topology summary. */
    struct ModelInstanceFragment
    {
        AssetId sourceModelAssetId{};
        PreparedModelNodeId rootTemporaryNodeId = RVX_INVALID_PREPARED_MODEL_NODE_ID;
        uint32 memberCount = 0;
    };

    /** @brief Per-member value link back to the instance root, including the deterministic source node ID. */
    struct ModelInstanceMemberFragment
    {
        ::RVX::ECS::EntityHandle instanceRoot = ::RVX::ECS::EntityHandle::Invalid();
        PreparedModelNodeId temporaryNodeId = RVX_INVALID_PREPARED_MODEL_NODE_ID;
    };

    static_assert(::RVX::ECS::Fragment<AssetInstanceFragment>);
    static_assert(::RVX::ECS::Fragment<ModelInstanceFragment>);
    static_assert(::RVX::ECS::Fragment<ModelInstanceMemberFragment>);

    /**
     * @brief Complete value description of one entity to create during model adoption.
     *
     * Resource objects are deliberately reduced to AssetId values.  This batch
     * owns no resource leases and does not expose the source Node or ModelResource.
     */
    struct PreparedModelNode
    {
        PreparedModelNodeId temporaryNodeId = RVX_INVALID_PREPARED_MODEL_NODE_ID;
        PreparedModelNodeId parentTemporaryNodeId = RVX_INVALID_PREPARED_MODEL_NODE_ID;
        /** Exact UTF-8 Geometry::Node name. Derived primitive entities retain the source name. */
        std::string sourceName;
        /** Exact Geometry::Node identity for this source hierarchy node. */
        PreparedModelSourceNodeIdentity sourceNodeIdentity;
        /**
         * @brief Runtime-facing deterministic name.
         *
         * For source nodes this is sourceName. Derived mesh-primitive children
         * append their source identity and ordinal, and are therefore never
         * ambiguous with their source node even when source names are duplicated.
         */
        std::string nodeName;
        /** Invalid for a source Node; otherwise the zero-based source primitive ordinal. */
        uint32 derivedPrimitiveOrdinal = RVX_INVALID_PREPARED_MODEL_DERIVED_PRIMITIVE_ORDINAL;
        /**
         * @brief Batch-local Skeleton/Pose source for this renderable mesh.
         *
         * Derived primitives retain the exact source node identity, but do not
         * duplicate its Animation fragments or side-table evaluator.  This
         * value lets adoption resolve all such primitives to that one source
         * entity within its creation transaction.
         */
        PreparedModelNodeId skinnedPoseOwnerTemporaryNodeId =
            RVX_INVALID_PREPARED_MODEL_NODE_ID;
        SceneECS::LocalTransform localTransform;
        SceneECS::Bounds bounds;
        SceneECS::Active active;
        SceneECS::Layer layer;
        bool hasMesh = false;
        SceneECS::Mesh mesh;
        SceneECS::MaterialSlots materialSlots;
        SceneECS::Visibility visibility;
        PreparedModelOptionalPayload optionalPayload;
    };

    /** @brief Immutable-by-convention transfer object between model preparation and Scene ownership. */
    struct PreparedModelBatch
    {
        AssetId sourceModelAssetId{};
        std::vector<PreparedModelNode> nodes;
    };

    enum class PreparedModelBuildError : uint8
    {
        None = 0,
        ModelNotPublished,
        MissingRootNode,
        SourceHierarchyCycle,
        UnresolvedMeshResource,
        InvalidMeshPrerequisite,
        InvalidMaterialPrerequisite,
        InvalidOptionalPayloadPrerequisite,
        TooManyMaterialSlots,
        UnsupportedSourceMeshLayout,
    };

    /** @brief Result of a read-only ModelResource to PreparedModelBatch conversion. */
    struct PreparedModelBatchBuildReceipt
    {
        PreparedModelBuildError error = PreparedModelBuildError::None;
        PreparedModelBatch batch;

        [[nodiscard]] bool IsPrepared() const { return error == PreparedModelBuildError::None; }
        explicit operator bool() const { return IsPrepared(); }
    };

    /** @brief Stateless owner-thread adapter from a published ModelResource to a value batch. */
    class PreparedModelBatchBuilder final
    {
    public:
        /**
         * @brief Copy published source data into a value-only batch without mutating the model or a Registry.
         *
         * The caller must hold the Resource publication/owner-thread guarantee while this method runs.
         */
        [[nodiscard]] static PreparedModelBatchBuildReceipt Build(const Resource::ModelResource& model);
    };

    enum class PreparedModelAdoptionError : uint8
    {
        None = 0,
        InvalidExpectedSceneRuntime,
        SceneRuntimeMismatch,
        EmptyBatch,
        EntityCapacityExceeded,
        InvalidTemporaryNodeId,
        InvalidSourceNodeIdentity,
        DuplicateTemporaryNodeId,
        MissingParent,
        MultipleRoots,
        HierarchyCycle,
        InvalidMeshAssetPrerequisite,
        InvalidMaterialSlotCount,
        InvalidMaterialAssetPrerequisite,
        InvalidSourceModelAssetPrerequisite,
        InvalidOptionalPayloadPrerequisite,
        InvalidSkinnedPoseOwner,
        RecordingRejected,
        TransactionRejected,
    };

    /** @brief Explicit test-only transactional failure injection for rollback validation. */
    enum class PreparedModelAdoptionFault : uint8
    {
        None = 0,
        RejectAfterRecording = 1,
    };

    /** @brief Limits and validation policy for a single owner-thread adoption attempt. */
    struct PreparedModelAdoptionOptions
    {
        uint32 maxAdditionalEntities = std::numeric_limits<uint32>::max();
        bool requireAssetPrerequisites = true;
        PreparedModelAdoptionFault fault = PreparedModelAdoptionFault::None;
    };

    /** @brief Stable mapping from a batch-local node ID to its newly published entity. */
    struct PreparedModelEntityMapping
    {
        PreparedModelNodeId temporaryNodeId = RVX_INVALID_PREPARED_MODEL_NODE_ID;
        ::RVX::ECS::EntityHandle entity = ::RVX::ECS::EntityHandle::Invalid();
        /** Exact UTF-8 source name; duplicate and empty names are intentional. */
        std::string sourceName;
        /** Value copied from the published Geometry::Node, never a pointer. */
        PreparedModelSourceNodeIdentity sourceNodeIdentity;
        /** Deterministic runtime name for diagnostics and derived primitive distinction. */
        std::string nodeName;
        uint32 derivedPrimitiveOrdinal = RVX_INVALID_PREPARED_MODEL_DERIVED_PRIMITIVE_ORDINAL;
    };

    /**
     * @brief Complete evidence of one atomic adoption attempt.
     *
     * On failure, rootEntity and members remain invalid/empty and the two
     * structural revision values are identical.
     */
    struct PreparedModelAdoptionReceipt
    {
        PreparedModelAdoptionError error = PreparedModelAdoptionError::None;
        ::RVX::ECS::CommandStatus transactionStatus = ::RVX::ECS::CommandStatus::Rejected;
        ::RVX::ECS::CommandError transactionError = ::RVX::ECS::CommandError::None;
        ::RVX::ECS::SceneRuntimeId sceneRuntimeId;
        uint64 sceneStructuralRevisionBefore = 0;
        uint64 sceneStructuralRevisionAfter = 0;
        ::RVX::ECS::EntityHandle rootEntity = ::RVX::ECS::EntityHandle::Invalid();
        std::vector<PreparedModelEntityMapping> members;

        [[nodiscard]] bool IsApplied() const
        {
            return error == PreparedModelAdoptionError::None &&
                   transactionStatus == ::RVX::ECS::CommandStatus::Applied;
        }

        [[nodiscard]] ::RVX::ECS::EntityHandle FindEntityByTemporaryNodeId(
            PreparedModelNodeId id) const;
        [[nodiscard]] std::vector<::RVX::ECS::EntityHandle> FindEntitiesByExactSourceName(
            std::string_view sourceName) const;
        [[nodiscard]] std::optional<::RVX::ECS::EntityHandle> FindUniqueEntityByExactSourceName(
            std::string_view sourceName) const;

        /** @brief Compatibility spelling for existing callers; the lookup remains temporary-ID exact. */
        [[nodiscard]] ::RVX::ECS::EntityHandle FindEntity(PreparedModelNodeId id) const
        {
            return FindEntityByTemporaryNodeId(id);
        }
    };

    /**
     * @brief Validate and atomically publish an entire prepared hierarchy into one Scene ECS runtime.
     *
     * expectedSceneRuntimeId is deliberately mandatory: batches do not carry a
     * Scene identity while they are prepared, and callers must prove the intended
     * owner before any transaction is recorded.
     */
    [[nodiscard]] PreparedModelAdoptionReceipt AdoptPreparedModelBatch(
        SceneECS::SceneEcsRuntime& runtime,
        const PreparedModelBatch& batch,
        ::RVX::ECS::SceneRuntimeId expectedSceneRuntimeId,
        const PreparedModelAdoptionOptions& options = {});
} // namespace RVX::ResourceSceneAdapters
