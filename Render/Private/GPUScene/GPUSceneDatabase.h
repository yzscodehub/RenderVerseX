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

    /** @brief Allocator state for a single typed table slot. */
    enum class GPUSceneSlotState : uint8
    {
        Invalid = 0,
        Live,
        Retired,
        PermanentlyRetired,
    };

    /**
     * @brief Owns the committed CPU mirror and validates atomic scene updates.
     *
     * Every table owns its own allocator; matching slot values across table
     * types have no semantic meaning. Removed slots are deliberately never
     * reused in Task 11A. No frame count or CPU-side estimate is treated as GPU
     * completion; future completion-token reclamation must be introduced later.
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

        /** @brief Render-private deterministic prepare-allocation failure seam. */
        void SetPrepareAllocationFailureCountdownForTesting(int32 countdown) noexcept;

        [[nodiscard]] const GPUSceneCommittedMirror& GetCommittedMirror() const;
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
            uint32 generation = 0;
            GPUSceneSlotState state = GPUSceneSlotState::Invalid;
        };

        struct DrawReferences
        {
            GPUSceneDrawRef firstDraw;
            GPUSceneMaterialRef firstMaterial;
            GPUSceneGeometryRef firstGeometry;
            uint32 count = 0;
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
        };

        /** @brief Append-only slots required by a preflighted transaction. */
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
            bool replaceDraws = false;
        };

        struct PreparedTransaction
        {
            std::vector<PreparedOperation> operations;
            std::unordered_map<uint64, GPUScenePrimitiveRef> addedPrimitives;
            CapacityDelta delta;
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
        [[nodiscard]] DrawReferences MakeFutureDrawReferences(
            const State& state,
            const CapacityDelta& priorDelta,
            uint32 drawCount) const noexcept;

        void WriteLiveObject(
            State& state,
            GPUScenePrimitiveRef primitive,
            GPUSceneBoundsRef bounds,
            GPUSceneTransformRef transform,
            const DrawReferences& draws,
            const GPUSceneObjectData& object) const noexcept;
        void AppendDrawReferences(
            State& state,
            uint64 objectId,
            const DrawReferences& draws) const noexcept;
        void RetireDrawReferences(State& state, const DrawReferences& draws) const noexcept;

        State m_state;
        uint32 m_initialSlotGeneration = 1;
        uint32 m_maxSlotCapacity = std::numeric_limits<uint32>::max();
        int32 m_prepareAllocationFailureCountdown = -1;
    };
} // namespace RVX
