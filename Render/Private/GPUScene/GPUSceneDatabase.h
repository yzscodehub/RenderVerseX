#pragma once

/**
 * @file GPUSceneDatabase.h
 * @brief Render-private committed CPU mirror and transactional GPU-scene allocator.
 */

#include "Render/GPUScene/GPUSceneSchema.h"

#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace RVX
{
    /** @brief Value-only input for one material/geometry/draw batch. */
    struct GPUSceneDrawData
    {
        GPUSceneMaterialRow material;
        GPUSceneGeometryRow geometry;
        GPUSceneDrawMetadataRow draw;
    };

    /** @brief Complete value input for one logical scene object and its draw batches. */
    struct GPUSceneObjectData
    {
        uint64 objectId = 0;
        GPUSceneBoundsRow bounds;
        GPUSceneTransformRow transform;
        std::vector<GPUSceneDrawData> draws;
        uint32 primitiveFlags = 0;
        uint32 layerMask = 0;
        uint64 sortKey = 0;
    };

    /** @brief Outcome of committing one immutable sequence of object changes. */
    enum class GPUSceneCommitStatus : uint8
    {
        Success = 0,
        InvalidObjectId,
        DuplicateObjectId,
        ObjectAlreadyExists,
        ObjectNotFound,
        StalePrimitiveRef,
        InvalidDrawRange,
        CapacityExhausted,
        VersionExhausted,
        AllocationFailed,
    };

    struct GPUSceneCommitResult
    {
        GPUSceneCommitStatus status = GPUSceneCommitStatus::Success;
        uint64 committedVersion = 0;

        [[nodiscard]] constexpr bool Succeeded() const
        {
            return status == GPUSceneCommitStatus::Success;
        }
    };

    /** @brief One staged, all-or-nothing sequence of GPU-scene changes. */
    class GPUSceneTransaction
    {
    public:
        GPUSceneTransaction& Add(const GPUSceneObjectData& object);
        GPUSceneTransaction& Update(
            GPUScenePrimitiveRef primitive,
            const GPUSceneObjectData& object);
        GPUSceneTransaction& Remove(GPUScenePrimitiveRef primitive);

        [[nodiscard]] bool IsEmpty() const { return m_operations.empty(); }

    private:
        enum class OperationType : uint8
        {
            Add = 0,
            Update,
            Remove,
        };

        struct Operation
        {
            GPUSceneObjectData object;
            GPUScenePrimitiveRef primitive;
            OperationType type = OperationType::Add;
            uint8 padding[7] = {};
        };

        std::vector<Operation> m_operations;

        friend class GPUSceneDatabase;
    };

    /** @brief Read-only CPU data corresponding exactly to the last successful commit. */
    struct GPUSceneCommittedMirror
    {
        uint64 version = 0;
        std::vector<GPUScenePrimitiveRow> primitives;
        std::vector<GPUSceneBoundsRow> bounds;
        std::vector<GPUSceneTransformRow> transforms;
        std::vector<GPUSceneMaterialRow> materials;
        std::vector<GPUSceneGeometryRow> geometries;
        std::vector<GPUSceneDrawMetadataRow> draws;
    };

    /** @brief One contiguous, zero-based row range changed in a committed table. */
    struct GPUSceneDirtyRowRange
    {
        uint32 firstRow = 0;
        uint32 rowCount = 0;

        constexpr bool operator==(const GPUSceneDirtyRowRange&) const = default;
    };

    /** @brief Value-only dirty description for one GPU-scene table. */
    struct GPUSceneTableChangeSet
    {
        /** @brief The consumer must upload the complete current table extent. */
        bool fullTableDirty = false;
        std::vector<GPUSceneDirtyRowRange> dirtyRanges;

        bool operator==(const GPUSceneTableChangeSet&) const = default;
    };

    /**
     * @brief Atomic value-only delta between two committed CPU-mirror versions.
     *
     * A non-empty successful Commit publishes exact sorted/coalesced ranges.
     * Clear publishes full-table dirtiness so it never needs to allocate while
     * tombstoning live rows. This is an upload-planning input only; it does not
     * make GPUScene an execution input.
     */
    struct GPUSceneChangeSet
    {
        uint64 baseVersion = 0;
        uint64 committedVersion = 0;
        GPUSceneTableChangeSet primitives;
        GPUSceneTableChangeSet bounds;
        GPUSceneTableChangeSet transforms;
        GPUSceneTableChangeSet materials;
        GPUSceneTableChangeSet geometries;
        GPUSceneTableChangeSet draws;

        bool operator==(const GPUSceneChangeSet&) const = default;
    };

    /** @brief Allocator state for a single typed table slot. */
    enum class GPUSceneSlotState : uint8
    {
        Invalid = 0,
        Live,
        Retired,
        Free,
        PermanentlyRetired,
    };

    /**
     * @brief Owns the committed CPU mirror and validates atomic scene updates.
     *
     * Every table owns its own allocator; matching slot values across table
     * types have no semantic meaning. Reuse is possible only after the caller
     * supplies a version watermark already proven safe by real GPU completion.
     * The database does not derive completion from frame counts, clocks, or
     * CPU-side estimates.
     */
    class GPUSceneDatabase : public NonMovable
    {
    public:
        /**
         * @brief Create an empty mirror. @p initialSlotGeneration must be nonzero.
         *
         * The parameter permits deterministic validation of terminal
         * UINT32_MAX retirement without ever publishing generation zero. The
         * additional parameters are Render-private validation seams; production
         * callers use their defaults.
         */
        explicit GPUSceneDatabase(
            uint32 initialSlotGeneration = 1,
            uint32 maxSlotCapacity = std::numeric_limits<uint32>::max(),
            uint64 initialCommittedVersion = 0);

        /**
         * @brief Apply all staged changes or preserve the prior mirror unchanged.
         *
         * An empty transaction remains a no-op success at any version, including
         * UINT64_MAX. A non-empty transaction fails closed at UINT64_MAX.
         */
        [[nodiscard]] GPUSceneCommitResult Commit(const GPUSceneTransaction& transaction);

        /** @brief Tombstone live rows without reusing or resetting any identity. */
        void Clear() noexcept;

        /**
         * @brief Admit retirement records proven safe through @p safeVersion.
         *
         * The caller owns conversion from real multi-domain GPU completion
         * tokens to this committed-version watermark. No automatic reclamation
         * occurs inside the database. False means an allocation failure left
         * the allocator state unchanged.
         */
        [[nodiscard]] bool ReclaimRetiredThrough(uint64 safeVersion);

        /** @brief Render-private deterministic prepare-allocation failure seam. */
        void SetPrepareAllocationFailureCountdownForTesting(int32 countdown) noexcept;

        [[nodiscard]] const GPUSceneCommittedMirror& GetCommittedMirror() const;
        [[nodiscard]] const GPUSceneChangeSet& GetLastChangeSet() const noexcept;
        [[nodiscard]] uint64 GetCommittedVersion() const;
        /** @brief Number of addressable primitive-table slots, excluding slot zero. */
        [[nodiscard]] uint32 GetSlotCapacity() const;
        [[nodiscard]] uint32 GetObjectCount() const;
        [[nodiscard]] std::optional<GPUScenePrimitiveRef> FindPrimitive(uint64 objectId) const;

        [[nodiscard]] GPUSceneSlotState GetSlotState(GPUScenePrimitiveRef primitive) const;
        [[nodiscard]] bool IsLive(GPUScenePrimitiveRef primitive) const;
        [[nodiscard]] bool IsLive(GPUSceneBoundsRef bounds) const;
        [[nodiscard]] bool IsLive(GPUSceneTransformRef transform) const;
        [[nodiscard]] bool IsLive(GPUSceneMaterialRef material) const;
        [[nodiscard]] bool IsLive(GPUSceneGeometryRef geometry) const;
        [[nodiscard]] bool IsLive(GPUSceneDrawRef draw) const;

        [[nodiscard]] const GPUScenePrimitiveRow* GetRow(GPUScenePrimitiveRef primitive) const;
        [[nodiscard]] const GPUSceneBoundsRow* GetRow(GPUSceneBoundsRef bounds) const;
        [[nodiscard]] const GPUSceneTransformRow* GetRow(GPUSceneTransformRef transform) const;
        [[nodiscard]] const GPUSceneMaterialRow* GetRow(GPUSceneMaterialRef material) const;
        [[nodiscard]] const GPUSceneGeometryRow* GetRow(GPUSceneGeometryRef geometry) const;
        [[nodiscard]] const GPUSceneDrawMetadataRow* GetRow(GPUSceneDrawRef draw) const;

    private:
        struct SlotRecord
        {
            uint64 objectId = 0;
            uint64 retireVersion = 0;
            uint32 generation = 0;
            GPUSceneSlotState state = GPUSceneSlotState::Invalid;
        };

        struct DrawReferences
        {
            GPUSceneDrawRef firstDraw;
            GPUSceneMaterialRef firstMaterial;
            GPUSceneGeometryRef firstGeometry;
            uint32 count = 0;
            bool appended = false;
        };

        /** @brief One atomically retired material/geometry/draw block. */
        struct DrawBlock
        {
            uint64 retireVersion = 0;
            uint32 firstMaterialSlot = 0;
            uint32 firstGeometrySlot = 0;
            uint32 firstDrawSlot = 0;
            uint32 count = 0;
            uint32 generation = 0;
        };

        struct State
        {
            GPUSceneCommittedMirror mirror;
            std::vector<SlotRecord> primitiveSlots;
            std::vector<SlotRecord> boundsSlots;
            std::vector<SlotRecord> transformSlots;
            std::vector<SlotRecord> materialSlots;
            std::vector<SlotRecord> geometrySlots;
            std::vector<SlotRecord> drawSlots;
            std::unordered_map<uint64, GPUScenePrimitiveRef> objectToPrimitive;
            std::vector<uint32> freePrimitiveSlots;
            std::vector<uint32> freeBoundsSlots;
            std::vector<uint32> freeTransformSlots;
            std::vector<DrawBlock> retiredDrawBlocks;
            std::vector<DrawBlock> freeDrawBlocks;
        };

        /** @brief Actual append-only slots required after reclaimed reuse. */
        struct CapacityDelta
        {
            size_t primitives = 0;
            size_t bounds = 0;
            size_t transforms = 0;
            size_t materials = 0;
            size_t geometries = 0;
            size_t draws = 0;
            size_t objects = 0;
        };

        struct PreparedOperation
        {
            const GPUSceneTransaction::Operation* operation = nullptr;
            uint64 objectId = 0;
            GPUScenePrimitiveRef primitive;
            GPUSceneBoundsRef bounds;
            GPUSceneTransformRef transform;
            DrawReferences previousDraws;
            DrawReferences draws;
            bool appendPrimitive = false;
            bool appendBounds = false;
            bool appendTransform = false;
            bool replaceDraws = false;
        };

        struct PreparedTransaction
        {
            std::vector<PreparedOperation> operations;
            std::unordered_map<uint64, GPUScenePrimitiveRef> addedPrimitives;
            GPUSceneChangeSet changeSet;
            std::vector<uint32> reusedDrawBlockIndices;
            CapacityDelta delta;
            size_t reusedPrimitiveCount = 0;
            size_t reusedBoundsCount = 0;
            size_t reusedTransformCount = 0;
            size_t retiredDrawBlockCount = 0;
        };

        [[nodiscard]] GPUSceneCommitResult PrepareTransaction(
            const GPUSceneTransaction& transaction,
            PreparedTransaction& outPrepared);
        [[nodiscard]] GPUSceneCommitResult ReserveAndPrepareNodes(
            PreparedTransaction& prepared);
        void FinalizePrepared(PreparedTransaction& prepared) noexcept;
        void FailPrepareAllocationCheckpoint();
        [[nodiscard]] bool IsLiveInState(const State& state, GPUScenePrimitiveRef primitive) const;
        [[nodiscard]] std::optional<DrawReferences> GetDrawReferences(
            const State& state,
            GPUScenePrimitiveRef primitive) const;
        [[nodiscard]] bool AllocateDrawReferences(
            const State& state,
            PreparedTransaction& prepared,
            uint32 drawCount,
            DrawReferences& outReferences);
        [[nodiscard]] bool AllocateSingleSlot(
            const std::vector<SlotRecord>& slots,
            const std::vector<uint32>& freeSlots,
            size_t& inOutReuseCount,
            size_t& inOutAppendCount,
            uint32& outSlot,
            uint32& outGeneration,
            bool& outAppended) const noexcept;
        void MarkDirtyRange(
            GPUSceneTableChangeSet& table,
            uint32 firstRow,
            uint32 rowCount) const;
        void SortAndMergeDirtyRanges(GPUSceneTableChangeSet& table) const noexcept;
        void PublishPreparedChangeSet(PreparedTransaction& prepared) noexcept;
        void PublishFullChangeSetNoexcept(uint64 baseVersion, uint64 committedVersion) noexcept;

        void WriteLiveObject(
            State& state,
            GPUScenePrimitiveRef primitive,
            GPUSceneBoundsRef bounds,
            GPUSceneTransformRef transform,
            const DrawReferences& draws,
            const GPUSceneObjectData& object) const noexcept;
        void ActivateDrawReferences(
            State& state,
            uint64 objectId,
            const DrawReferences& draws) const noexcept;
        void RetireDrawReferences(
            State& state,
            const DrawReferences& draws,
            uint64 retireVersion) const noexcept;

        State m_state;
        GPUSceneChangeSet m_lastChangeSet;
        uint32 m_initialSlotGeneration = 1;
        uint32 m_maxSlotCapacity = std::numeric_limits<uint32>::max();
        int32 m_prepareAllocationFailureCountdown = -1;
    };
} // namespace RVX
