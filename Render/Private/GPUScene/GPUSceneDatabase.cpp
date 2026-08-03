#include "GPUScene/GPUSceneDatabase.h"

#include <algorithm>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

namespace RVX
{
    namespace
    {
        template <typename Row>
        void WriteLiveHeader(Row& row, uint64 objectId, uint32 generation)
        {
            row.header.objectId = PackGPUSceneUint64(objectId);
            row.header.schemaVersion = RVX_GPU_SCENE_SCHEMA_VERSION;
            row.header.generation = generation;
            row.header.flags = static_cast<uint32>(GPUSceneRowFlags::Live);
            row.header.padding[0] = 0;
            row.header.padding[1] = 0;
            row.header.padding[2] = 0;
        }

        template <typename Row>
        void WriteTombstoneHeader(Row& row, uint64 objectId, uint32 generation)
        {
            row = {};
            row.header.objectId = PackGPUSceneUint64(objectId);
            row.header.schemaVersion = RVX_GPU_SCENE_SCHEMA_VERSION;
            row.header.generation = generation;
            row.header.flags = static_cast<uint32>(GPUSceneRowFlags::Tombstone);
            row.header.padding[0] = 0;
            row.header.padding[1] = 0;
            row.header.padding[2] = 0;
        }

        template <typename Ref, typename SlotRecord>
        bool IsLiveRef(const std::vector<SlotRecord>& slots, Ref reference)
        {
            return reference.IsValid() && reference.slot < slots.size() &&
                   slots[reference.slot].state == GPUSceneSlotState::Live &&
                   slots[reference.slot].generation == reference.generation;
        }

        template <typename Row, typename Ref, typename SlotRecord>
        void RetireRow(
            std::vector<Row>& rows,
            std::vector<SlotRecord>& slots,
            Ref reference,
            uint64 retireVersion)
        {
            SlotRecord& record = slots[reference.slot];
            WriteTombstoneHeader(rows[reference.slot], record.objectId, record.generation);
            record.state = record.generation == std::numeric_limits<uint32>::max()
                               ? GPUSceneSlotState::PermanentlyRetired
                               : GPUSceneSlotState::Retired;
            record.retireVersion = record.state == GPUSceneSlotState::Retired
                                       ? retireVersion
                                       : 0;
        }

        bool CanAppendSlots(size_t currentSize, size_t count, uint32 maxSlotCapacity)
        {
            if (currentSize == 0)
            {
                return false;
            }

            const size_t currentCapacity = currentSize - 1U;
            const size_t maximumCapacity = static_cast<size_t>(maxSlotCapacity);
            return currentCapacity <= maximumCapacity &&
                   count <= maximumCapacity - currentCapacity;
        }

        template <typename Row, typename SlotRecord>
        uint32 AppendSlot(
            std::vector<Row>& rows,
            std::vector<SlotRecord>& slots,
            uint64 objectId,
            uint32 generation) noexcept
        {
            const uint32 slot = static_cast<uint32>(slots.size());
            slots.push_back({objectId, 0, generation, GPUSceneSlotState::Live});
            rows.emplace_back();
            return slot;
        }

        template <typename Row, typename SlotRecord>
        void ActivateSlot(
            std::vector<Row>& rows,
            std::vector<SlotRecord>& slots,
            uint32 slot,
            uint64 objectId,
            uint32 generation) noexcept
        {
            SlotRecord& record = slots[slot];
            record.objectId = objectId;
            record.retireVersion = 0;
            record.generation = generation;
            record.state = GPUSceneSlotState::Live;
            rows[slot] = {};
        }
    } // namespace

    GPUSceneTransaction& GPUSceneTransaction::Add(const GPUSceneObjectData& object)
    {
        Operation operation;
        operation.type = OperationType::Add;
        operation.object = object;
        m_operations.push_back(std::move(operation));
        return *this;
    }

    GPUSceneTransaction& GPUSceneTransaction::Update(
        GPUScenePrimitiveRef primitive,
        const GPUSceneObjectData& object)
    {
        Operation operation;
        operation.type = OperationType::Update;
        operation.primitive = primitive;
        operation.object = object;
        m_operations.push_back(std::move(operation));
        return *this;
    }

    GPUSceneTransaction& GPUSceneTransaction::Remove(GPUScenePrimitiveRef primitive)
    {
        Operation operation;
        operation.type = OperationType::Remove;
        operation.primitive = primitive;
        m_operations.push_back(std::move(operation));
        return *this;
    }

    GPUSceneDatabase::GPUSceneDatabase(
        uint32 initialSlotGeneration,
        uint32 maxSlotCapacity,
        uint64 initialCommittedVersion)
        : m_initialSlotGeneration(
              initialSlotGeneration == 0 ? 1 : initialSlotGeneration),
          m_maxSlotCapacity(maxSlotCapacity)
    {
        // Zero is a reserved sentinel row in each independently allocated table.
        m_state.primitiveSlots.resize(1);
        m_state.boundsSlots.resize(1);
        m_state.transformSlots.resize(1);
        m_state.materialSlots.resize(1);
        m_state.geometrySlots.resize(1);
        m_state.drawSlots.resize(1);
        m_state.mirror.primitives.resize(1);
        m_state.mirror.bounds.resize(1);
        m_state.mirror.transforms.resize(1);
        m_state.mirror.materials.resize(1);
        m_state.mirror.geometries.resize(1);
        m_state.mirror.draws.resize(1);
        m_state.mirror.version = initialCommittedVersion;
        m_lastChangeSet.baseVersion = initialCommittedVersion;
        m_lastChangeSet.committedVersion = initialCommittedVersion;
    }

    GPUSceneCommitResult GPUSceneDatabase::Commit(const GPUSceneTransaction& transaction)
    {
        if (transaction.IsEmpty())
        {
            return {GPUSceneCommitStatus::Success, m_state.mirror.version};
        }

        if (m_state.mirror.version == std::numeric_limits<uint64>::max())
        {
            return {GPUSceneCommitStatus::VersionExhausted, m_state.mirror.version};
        }

        PreparedTransaction prepared;
        try
        {
            const GPUSceneCommitResult preflight =
                PrepareTransaction(transaction, prepared);
            if (!preflight.Succeeded())
            {
                return preflight;
            }
            const GPUSceneCommitResult reservation =
                ReserveAndPrepareNodes(prepared);
            if (!reservation.Succeeded())
            {
                return reservation;
            }
        }
        catch (const std::bad_alloc&)
        {
            return {GPUSceneCommitStatus::AllocationFailed, m_state.mirror.version};
        }

        const uint64 baseVersion = m_state.mirror.version;
        // Finalization mutates only touched rows. All dynamic allocations,
        // including map nodes, journal storage, and retired-block capacity,
        // completed above; this path is noexcept.
        FinalizePrepared(prepared);
        ++m_state.mirror.version;
        prepared.changeSet.baseVersion = baseVersion;
        prepared.changeSet.committedVersion = m_state.mirror.version;
        PublishPreparedChangeSet(prepared);
        return {GPUSceneCommitStatus::Success, m_state.mirror.version};
    }

    void GPUSceneDatabase::Clear() noexcept
    {
        if (m_state.mirror.version == std::numeric_limits<uint64>::max())
        {
            return;
        }

        const uint64 baseVersion = m_state.mirror.version;
        const uint64 retireVersion = baseVersion + 1U;
        for (size_t slot = 1; slot < m_state.primitiveSlots.size(); ++slot)
        {
            if (m_state.primitiveSlots[slot].state != GPUSceneSlotState::Live)
            {
                continue;
            }

            const GPUScenePrimitiveRef primitive{
                static_cast<uint32>(slot),
                m_state.primitiveSlots[slot].generation};
            const std::optional<DrawReferences> draws =
                GetDrawReferences(m_state, primitive);
            if (draws)
            {
                RetireDrawReferences(m_state, *draws, retireVersion);
            }

            const GPUScenePrimitiveRow& primitiveRow =
                m_state.mirror.primitives[slot];
            RetireRow(m_state.mirror.bounds, m_state.boundsSlots,
                      primitiveRow.bounds, retireVersion);
            RetireRow(m_state.mirror.transforms, m_state.transformSlots,
                      primitiveRow.transform, retireVersion);
            RetireRow(m_state.mirror.primitives, m_state.primitiveSlots,
                      primitive, retireVersion);
        }
        m_state.objectToPrimitive.clear();
        ++m_state.mirror.version;
        PublishFullChangeSetNoexcept(baseVersion, m_state.mirror.version);
    }

    bool GPUSceneDatabase::ReclaimRetiredThrough(uint64 safeVersion)
    {
        size_t primitiveCount = 0;
        size_t boundsCount = 0;
        size_t transformCount = 0;
        size_t drawBlockCount = 0;
        const auto countReclaimable = [safeVersion](const auto& slots, size_t& count)
        {
            for (size_t slot = 1; slot < slots.size(); ++slot)
            {
                const SlotRecord& record = slots[slot];
                if (record.state == GPUSceneSlotState::Retired &&
                    record.retireVersion <= safeVersion)
                {
                    ++count;
                }
            }
        };
        countReclaimable(m_state.primitiveSlots, primitiveCount);
        countReclaimable(m_state.boundsSlots, boundsCount);
        countReclaimable(m_state.transformSlots, transformCount);
        for (const DrawBlock& block : m_state.retiredDrawBlocks)
        {
            if (block.retireVersion <= safeVersion)
            {
                ++drawBlockCount;
            }
        }

        if (primitiveCount == 0 && boundsCount == 0 && transformCount == 0 &&
            drawBlockCount == 0)
        {
            return true;
        }

        try
        {
            std::vector<uint32> freePrimitives = m_state.freePrimitiveSlots;
            std::vector<uint32> freeBounds = m_state.freeBoundsSlots;
            std::vector<uint32> freeTransforms = m_state.freeTransformSlots;
            std::vector<DrawBlock> freeDrawBlocks = m_state.freeDrawBlocks;
            std::vector<DrawBlock> retainedDrawBlocks;
            freePrimitives.reserve(freePrimitives.size() + primitiveCount);
            freeBounds.reserve(freeBounds.size() + boundsCount);
            freeTransforms.reserve(freeTransforms.size() + transformCount);
            freeDrawBlocks.reserve(freeDrawBlocks.size() + drawBlockCount);
            // Keep Clear noexcept even after this swap: any remaining live
            // draw block may have to move back into retired storage at once.
            retainedDrawBlocks.reserve(m_state.drawSlots.size() - 1U);

            const auto appendReclaimable = [safeVersion](
                                             const auto& slots,
                                             auto& freeSlots)
            {
                for (uint32 slot = 1; slot < slots.size(); ++slot)
                {
                    const SlotRecord& record = slots[slot];
                    if (record.state == GPUSceneSlotState::Retired &&
                        record.retireVersion <= safeVersion)
                    {
                        freeSlots.push_back(slot);
                    }
                }
            };
            appendReclaimable(m_state.primitiveSlots, freePrimitives);
            appendReclaimable(m_state.boundsSlots, freeBounds);
            appendReclaimable(m_state.transformSlots, freeTransforms);
            for (const DrawBlock& block : m_state.retiredDrawBlocks)
            {
                if (block.retireVersion <= safeVersion)
                {
                    freeDrawBlocks.push_back(block);
                }
                else
                {
                    retainedDrawBlocks.push_back(block);
                }
            }

            const auto markFree = [safeVersion](auto& slots)
            {
                for (size_t slot = 1; slot < slots.size(); ++slot)
                {
                    SlotRecord& record = slots[slot];
                    if (record.state == GPUSceneSlotState::Retired &&
                        record.retireVersion <= safeVersion)
                    {
                        record.state = GPUSceneSlotState::Free;
                        record.retireVersion = 0;
                    }
                }
            };
            markFree(m_state.primitiveSlots);
            markFree(m_state.boundsSlots);
            markFree(m_state.transformSlots);
            for (const DrawBlock& block : m_state.retiredDrawBlocks)
            {
                if (block.retireVersion > safeVersion)
                {
                    continue;
                }
                for (uint32 offset = 0; offset < block.count; ++offset)
                {
                    m_state.materialSlots[block.firstMaterialSlot + offset].state =
                        GPUSceneSlotState::Free;
                    m_state.materialSlots[block.firstMaterialSlot + offset].retireVersion = 0;
                    m_state.geometrySlots[block.firstGeometrySlot + offset].state =
                        GPUSceneSlotState::Free;
                    m_state.geometrySlots[block.firstGeometrySlot + offset].retireVersion = 0;
                    m_state.drawSlots[block.firstDrawSlot + offset].state =
                        GPUSceneSlotState::Free;
                    m_state.drawSlots[block.firstDrawSlot + offset].retireVersion = 0;
                }
            }
            m_state.freePrimitiveSlots.swap(freePrimitives);
            m_state.freeBoundsSlots.swap(freeBounds);
            m_state.freeTransformSlots.swap(freeTransforms);
            m_state.freeDrawBlocks.swap(freeDrawBlocks);
            m_state.retiredDrawBlocks.swap(retainedDrawBlocks);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        return true;
    }

    void GPUSceneDatabase::SetPrepareAllocationFailureCountdownForTesting(
        int32 countdown) noexcept
    {
        m_prepareAllocationFailureCountdown = countdown;
    }

    void GPUSceneDatabase::FailPrepareAllocationCheckpoint()
    {
        if (m_prepareAllocationFailureCountdown == 0)
        {
            throw std::bad_alloc();
        }
        if (m_prepareAllocationFailureCountdown > 0)
        {
            --m_prepareAllocationFailureCountdown;
        }
    }

    GPUSceneCommitResult GPUSceneDatabase::PrepareTransaction(
        const GPUSceneTransaction& transaction,
        PreparedTransaction& outPrepared)
    {
        std::unordered_set<uint64> affectedObjectIds;
        FailPrepareAllocationCheckpoint();
        affectedObjectIds.reserve(transaction.m_operations.size());
        FailPrepareAllocationCheckpoint();
        outPrepared.operations.reserve(transaction.m_operations.size());

        const size_t operationCount = transaction.m_operations.size();
        if (operationCount > std::numeric_limits<size_t>::max() / 2U)
        {
            return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
        }
        const size_t maximumRanges = operationCount * 2U;
        const auto reserveRanges = [maximumRanges](GPUSceneTableChangeSet& table)
        {
            table.dirtyRanges.reserve(maximumRanges);
        };
        FailPrepareAllocationCheckpoint(); reserveRanges(outPrepared.changeSet.primitives);
        FailPrepareAllocationCheckpoint(); reserveRanges(outPrepared.changeSet.bounds);
        FailPrepareAllocationCheckpoint(); reserveRanges(outPrepared.changeSet.transforms);
        FailPrepareAllocationCheckpoint(); reserveRanges(outPrepared.changeSet.materials);
        FailPrepareAllocationCheckpoint(); reserveRanges(outPrepared.changeSet.geometries);
        FailPrepareAllocationCheckpoint(); reserveRanges(outPrepared.changeSet.draws);
        FailPrepareAllocationCheckpoint();
        outPrepared.reusedDrawBlockIndices.reserve(operationCount);

        const auto addCount = [](size_t& total, size_t value)
        {
            if (value > std::numeric_limits<size_t>::max() - total)
            {
                return false;
            }
            total += value;
            return true;
        };

        for (const GPUSceneTransaction::Operation& operation : transaction.m_operations)
        {
            uint64 objectId = operation.object.objectId;
            switch (operation.type)
            {
                case GPUSceneTransaction::OperationType::Add:
                {
                    if (objectId == 0)
                    {
                        return {GPUSceneCommitStatus::InvalidObjectId, m_state.mirror.version};
                    }
                    if (!affectedObjectIds.insert(objectId).second)
                    {
                        return {GPUSceneCommitStatus::DuplicateObjectId, m_state.mirror.version};
                    }
                    if (m_state.objectToPrimitive.contains(objectId))
                    {
                        return {GPUSceneCommitStatus::ObjectAlreadyExists, m_state.mirror.version};
                    }
                    if (operation.object.draws.size() >
                        std::numeric_limits<uint32>::max())
                    {
                        return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
                    }
                    PreparedOperation prepared;
                    prepared.operation = &operation;
                    prepared.objectId = objectId;
                    uint32 slot = 0;
                    uint32 generation = 0;
                    if (!AllocateSingleSlot(
                            m_state.primitiveSlots, m_state.freePrimitiveSlots,
                            outPrepared.reusedPrimitiveCount,
                            outPrepared.delta.primitives, slot, generation,
                            prepared.appendPrimitive))
                    {
                        return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
                    }
                    prepared.primitive = {slot, generation};
                    if (!AllocateSingleSlot(
                            m_state.boundsSlots, m_state.freeBoundsSlots,
                            outPrepared.reusedBoundsCount,
                            outPrepared.delta.bounds, slot, generation,
                            prepared.appendBounds))
                    {
                        return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
                    }
                    prepared.bounds = {slot, generation};
                    if (!AllocateSingleSlot(
                            m_state.transformSlots, m_state.freeTransformSlots,
                            outPrepared.reusedTransformCount,
                            outPrepared.delta.transforms, slot, generation,
                            prepared.appendTransform))
                    {
                        return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
                    }
                    prepared.transform = {slot, generation};
                    if (!AllocateDrawReferences(
                            m_state, outPrepared,
                            static_cast<uint32>(operation.object.draws.size()),
                            prepared.draws) ||
                        !addCount(outPrepared.delta.objects, 1))
                    {
                        return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
                    }
                    MarkDirtyRange(outPrepared.changeSet.primitives,
                                   prepared.primitive.slot, 1);
                    MarkDirtyRange(outPrepared.changeSet.bounds,
                                   prepared.bounds.slot, 1);
                    MarkDirtyRange(outPrepared.changeSet.transforms,
                                   prepared.transform.slot, 1);
                    MarkDirtyRange(outPrepared.changeSet.materials,
                                   prepared.draws.firstMaterial.slot,
                                   prepared.draws.count);
                    MarkDirtyRange(outPrepared.changeSet.geometries,
                                   prepared.draws.firstGeometry.slot,
                                   prepared.draws.count);
                    MarkDirtyRange(outPrepared.changeSet.draws,
                                   prepared.draws.firstDraw.slot,
                                   prepared.draws.count);
                    outPrepared.operations.push_back(prepared);
                    break;
                }

                case GPUSceneTransaction::OperationType::Update:
                {
                    if (objectId == 0)
                    {
                        return {GPUSceneCommitStatus::InvalidObjectId, m_state.mirror.version};
                    }
                    if (!affectedObjectIds.insert(objectId).second)
                    {
                        return {GPUSceneCommitStatus::DuplicateObjectId, m_state.mirror.version};
                    }
                    if (!IsLiveInState(m_state, operation.primitive))
                    {
                        return {GPUSceneCommitStatus::StalePrimitiveRef, m_state.mirror.version};
                    }
                    const auto found = m_state.objectToPrimitive.find(objectId);
                    if (found == m_state.objectToPrimitive.end())
                    {
                        return {GPUSceneCommitStatus::ObjectNotFound, m_state.mirror.version};
                    }
                    if (found->second != operation.primitive)
                    {
                        return {GPUSceneCommitStatus::StalePrimitiveRef, m_state.mirror.version};
                    }
                    const std::optional<DrawReferences> previous =
                        GetDrawReferences(m_state, operation.primitive);
                    if (!previous)
                    {
                        return {GPUSceneCommitStatus::InvalidDrawRange, m_state.mirror.version};
                    }
                    if (operation.object.draws.size() >
                        std::numeric_limits<uint32>::max())
                    {
                        return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
                    }
                    const uint32 drawCount =
                        static_cast<uint32>(operation.object.draws.size());
                    PreparedOperation prepared;
                    prepared.operation = &operation;
                    prepared.objectId = objectId;
                    prepared.primitive = operation.primitive;
                    const GPUScenePrimitiveRow& primitiveRow =
                        m_state.mirror.primitives[operation.primitive.slot];
                    prepared.bounds = primitiveRow.bounds;
                    prepared.transform = primitiveRow.transform;
                    prepared.previousDraws = *previous;
                    prepared.replaceDraws = drawCount != previous->count;
                    if (prepared.replaceDraws)
                    {
                        if (!AllocateDrawReferences(
                                m_state, outPrepared, drawCount,
                                prepared.draws))
                        {
                            return {GPUSceneCommitStatus::CapacityExhausted,
                                    m_state.mirror.version};
                        }
                        if (previous->count != 0)
                        {
                            ++outPrepared.retiredDrawBlockCount;
                        }
                        MarkDirtyRange(outPrepared.changeSet.materials,
                                       previous->firstMaterial.slot,
                                       previous->count);
                        MarkDirtyRange(outPrepared.changeSet.geometries,
                                       previous->firstGeometry.slot,
                                       previous->count);
                        MarkDirtyRange(outPrepared.changeSet.draws,
                                       previous->firstDraw.slot,
                                       previous->count);
                    }
                    else
                    {
                        prepared.draws = *previous;
                    }
                    MarkDirtyRange(outPrepared.changeSet.primitives,
                                   prepared.primitive.slot, 1);
                    MarkDirtyRange(outPrepared.changeSet.bounds,
                                   prepared.bounds.slot, 1);
                    MarkDirtyRange(outPrepared.changeSet.transforms,
                                   prepared.transform.slot, 1);
                    MarkDirtyRange(outPrepared.changeSet.materials,
                                   prepared.draws.firstMaterial.slot,
                                   prepared.draws.count);
                    MarkDirtyRange(outPrepared.changeSet.geometries,
                                   prepared.draws.firstGeometry.slot,
                                   prepared.draws.count);
                    MarkDirtyRange(outPrepared.changeSet.draws,
                                   prepared.draws.firstDraw.slot,
                                   prepared.draws.count);
                    outPrepared.operations.push_back(prepared);
                    break;
                }

                case GPUSceneTransaction::OperationType::Remove:
                {
                    if (!IsLiveInState(m_state, operation.primitive))
                    {
                        return {GPUSceneCommitStatus::StalePrimitiveRef, m_state.mirror.version};
                    }
                    objectId = m_state.primitiveSlots[operation.primitive.slot].objectId;
                    if (!affectedObjectIds.insert(objectId).second)
                    {
                        return {GPUSceneCommitStatus::DuplicateObjectId, m_state.mirror.version};
                    }
                    const std::optional<DrawReferences> previous =
                        GetDrawReferences(m_state, operation.primitive);
                    if (!previous)
                    {
                        return {GPUSceneCommitStatus::InvalidDrawRange, m_state.mirror.version};
                    }
                    PreparedOperation prepared;
                    prepared.operation = &operation;
                    prepared.objectId = objectId;
                    prepared.primitive = operation.primitive;
                    const GPUScenePrimitiveRow& primitiveRow =
                        m_state.mirror.primitives[operation.primitive.slot];
                    prepared.bounds = primitiveRow.bounds;
                    prepared.transform = primitiveRow.transform;
                    prepared.previousDraws = *previous;
                    if (previous->count != 0)
                    {
                        ++outPrepared.retiredDrawBlockCount;
                    }
                    MarkDirtyRange(outPrepared.changeSet.primitives,
                                   prepared.primitive.slot, 1);
                    MarkDirtyRange(outPrepared.changeSet.bounds,
                                   prepared.bounds.slot, 1);
                    MarkDirtyRange(outPrepared.changeSet.transforms,
                                   prepared.transform.slot, 1);
                    MarkDirtyRange(outPrepared.changeSet.materials,
                                   previous->firstMaterial.slot,
                                   previous->count);
                    MarkDirtyRange(outPrepared.changeSet.geometries,
                                   previous->firstGeometry.slot,
                                   previous->count);
                    MarkDirtyRange(outPrepared.changeSet.draws,
                                   previous->firstDraw.slot,
                                   previous->count);
                    outPrepared.operations.push_back(prepared);
                    break;
                }
            }
        }

        const auto canAppend = [this](size_t currentSize, size_t count)
        {
            return CanAppendSlots(currentSize, count, m_maxSlotCapacity);
        };
        if (!canAppend(m_state.primitiveSlots.size(), outPrepared.delta.primitives) ||
            !canAppend(m_state.boundsSlots.size(), outPrepared.delta.bounds) ||
            !canAppend(m_state.transformSlots.size(), outPrepared.delta.transforms) ||
            !canAppend(m_state.materialSlots.size(), outPrepared.delta.materials) ||
            !canAppend(m_state.geometrySlots.size(), outPrepared.delta.geometries) ||
            !canAppend(m_state.drawSlots.size(), outPrepared.delta.draws))
        {
            return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
        }
        SortAndMergeDirtyRanges(outPrepared.changeSet.primitives);
        SortAndMergeDirtyRanges(outPrepared.changeSet.bounds);
        SortAndMergeDirtyRanges(outPrepared.changeSet.transforms);
        SortAndMergeDirtyRanges(outPrepared.changeSet.materials);
        SortAndMergeDirtyRanges(outPrepared.changeSet.geometries);
        SortAndMergeDirtyRanges(outPrepared.changeSet.draws);
        std::sort(outPrepared.reusedDrawBlockIndices.begin(),
                  outPrepared.reusedDrawBlockIndices.end());
        return {GPUSceneCommitStatus::Success, m_state.mirror.version};
    }

    GPUSceneCommitResult GPUSceneDatabase::ReserveAndPrepareNodes(
        PreparedTransaction& prepared)
    {
        const auto reserveRows = [](auto& rows, auto& slots, size_t count)
        {
            rows.reserve(rows.size() + count);
            slots.reserve(slots.size() + count);
        };
        FailPrepareAllocationCheckpoint(); reserveRows(m_state.mirror.primitives, m_state.primitiveSlots, prepared.delta.primitives);
        FailPrepareAllocationCheckpoint(); reserveRows(m_state.mirror.bounds, m_state.boundsSlots, prepared.delta.bounds);
        FailPrepareAllocationCheckpoint(); reserveRows(m_state.mirror.transforms, m_state.transformSlots, prepared.delta.transforms);
        FailPrepareAllocationCheckpoint(); reserveRows(m_state.mirror.materials, m_state.materialSlots, prepared.delta.materials);
        FailPrepareAllocationCheckpoint(); reserveRows(m_state.mirror.geometries, m_state.geometrySlots, prepared.delta.geometries);
        FailPrepareAllocationCheckpoint(); reserveRows(m_state.mirror.draws, m_state.drawSlots, prepared.delta.draws);
        // Clear is noexcept and can retire every live draw block. Reserve one
        // retired-block entry per physical draw row, the conservative maximum.
        FailPrepareAllocationCheckpoint();
        m_state.retiredDrawBlocks.reserve(
            (m_state.drawSlots.size() - 1U) + prepared.delta.draws);
        FailPrepareAllocationCheckpoint();
        m_state.objectToPrimitive.reserve(
            m_state.objectToPrimitive.size() + prepared.delta.objects);
        FailPrepareAllocationCheckpoint();
        prepared.addedPrimitives.reserve(prepared.delta.objects);
        for (const PreparedOperation& operation : prepared.operations)
        {
            if (operation.operation->type != GPUSceneTransaction::OperationType::Add)
            {
                continue;
            }
            FailPrepareAllocationCheckpoint();
            prepared.addedPrimitives.emplace(
                operation.operation->object.objectId, operation.primitive);
        }
        return {GPUSceneCommitStatus::Success, m_state.mirror.version};
    }

    const GPUSceneCommittedMirror& GPUSceneDatabase::GetCommittedMirror() const
    {
        return m_state.mirror;
    }

    const GPUSceneChangeSet& GPUSceneDatabase::GetLastChangeSet() const noexcept
    {
        return m_lastChangeSet;
    }

    uint64 GPUSceneDatabase::GetCommittedVersion() const
    {
        return m_state.mirror.version;
    }

    uint32 GPUSceneDatabase::GetSlotCapacity() const
    {
        return static_cast<uint32>(m_state.primitiveSlots.size() - 1);
    }

    uint32 GPUSceneDatabase::GetObjectCount() const
    {
        return static_cast<uint32>(m_state.objectToPrimitive.size());
    }

    std::optional<GPUScenePrimitiveRef> GPUSceneDatabase::FindPrimitive(uint64 objectId) const
    {
        const auto found = m_state.objectToPrimitive.find(objectId);
        if (found == m_state.objectToPrimitive.end())
        {
            return std::nullopt;
        }
        return found->second;
    }

    GPUSceneSlotState GPUSceneDatabase::GetSlotState(GPUScenePrimitiveRef primitive) const
    {
        if (!primitive.IsValid() || primitive.slot >= m_state.primitiveSlots.size() ||
            m_state.primitiveSlots[primitive.slot].generation != primitive.generation)
        {
            return GPUSceneSlotState::Invalid;
        }
        return m_state.primitiveSlots[primitive.slot].state;
    }

    bool GPUSceneDatabase::IsLive(GPUScenePrimitiveRef primitive) const
    {
        return IsLiveInState(m_state, primitive);
    }

    bool GPUSceneDatabase::IsLive(GPUSceneBoundsRef bounds) const
    {
        return IsLiveRef(m_state.boundsSlots, bounds);
    }

    bool GPUSceneDatabase::IsLive(GPUSceneTransformRef transform) const
    {
        return IsLiveRef(m_state.transformSlots, transform);
    }

    bool GPUSceneDatabase::IsLive(GPUSceneMaterialRef material) const
    {
        return IsLiveRef(m_state.materialSlots, material);
    }

    bool GPUSceneDatabase::IsLive(GPUSceneGeometryRef geometry) const
    {
        return IsLiveRef(m_state.geometrySlots, geometry);
    }

    bool GPUSceneDatabase::IsLive(GPUSceneDrawRef draw) const
    {
        return IsLiveRef(m_state.drawSlots, draw);
    }

    const GPUScenePrimitiveRow* GPUSceneDatabase::GetRow(GPUScenePrimitiveRef primitive) const
    {
        return IsLive(primitive) ? &m_state.mirror.primitives[primitive.slot] : nullptr;
    }

    const GPUSceneBoundsRow* GPUSceneDatabase::GetRow(GPUSceneBoundsRef bounds) const
    {
        return IsLive(bounds) ? &m_state.mirror.bounds[bounds.slot] : nullptr;
    }

    const GPUSceneTransformRow* GPUSceneDatabase::GetRow(GPUSceneTransformRef transform) const
    {
        return IsLive(transform) ? &m_state.mirror.transforms[transform.slot] : nullptr;
    }

    const GPUSceneMaterialRow* GPUSceneDatabase::GetRow(GPUSceneMaterialRef material) const
    {
        return IsLive(material) ? &m_state.mirror.materials[material.slot] : nullptr;
    }

    const GPUSceneGeometryRow* GPUSceneDatabase::GetRow(GPUSceneGeometryRef geometry) const
    {
        return IsLive(geometry) ? &m_state.mirror.geometries[geometry.slot] : nullptr;
    }

    const GPUSceneDrawMetadataRow* GPUSceneDatabase::GetRow(GPUSceneDrawRef draw) const
    {
        return IsLive(draw) ? &m_state.mirror.draws[draw.slot] : nullptr;
    }

    void GPUSceneDatabase::FinalizePrepared(PreparedTransaction& prepared) noexcept
    {
        const uint64 retireVersion = m_state.mirror.version + 1U;
        for (PreparedOperation& preparedOperation : prepared.operations)
        {
            const GPUSceneTransaction::Operation& operation =
                *preparedOperation.operation;
            const GPUSceneObjectData& object = operation.object;
            switch (operation.type)
            {
                case GPUSceneTransaction::OperationType::Add:
                    if (preparedOperation.appendPrimitive)
                    {
                        AppendSlot(m_state.mirror.primitives, m_state.primitiveSlots,
                                   object.objectId, preparedOperation.primitive.generation);
                    }
                    else
                    {
                        ActivateSlot(m_state.mirror.primitives, m_state.primitiveSlots,
                                     preparedOperation.primitive.slot, object.objectId,
                                     preparedOperation.primitive.generation);
                    }
                    if (preparedOperation.appendBounds)
                    {
                        AppendSlot(m_state.mirror.bounds, m_state.boundsSlots,
                                   object.objectId, preparedOperation.bounds.generation);
                    }
                    else
                    {
                        ActivateSlot(m_state.mirror.bounds, m_state.boundsSlots,
                                     preparedOperation.bounds.slot, object.objectId,
                                     preparedOperation.bounds.generation);
                    }
                    if (preparedOperation.appendTransform)
                    {
                        AppendSlot(m_state.mirror.transforms, m_state.transformSlots,
                                   object.objectId, preparedOperation.transform.generation);
                    }
                    else
                    {
                        ActivateSlot(m_state.mirror.transforms, m_state.transformSlots,
                                     preparedOperation.transform.slot, object.objectId,
                                     preparedOperation.transform.generation);
                    }
                    ActivateDrawReferences(m_state, object.objectId,
                                           preparedOperation.draws);
                    WriteLiveObject(m_state, preparedOperation.primitive,
                                    preparedOperation.bounds,
                                    preparedOperation.transform,
                                    preparedOperation.draws, object);
                    m_state.objectToPrimitive.insert(
                        prepared.addedPrimitives.extract(object.objectId));
                    break;
                case GPUSceneTransaction::OperationType::Update:
                    if (preparedOperation.replaceDraws)
                    {
                        RetireDrawReferences(m_state, preparedOperation.previousDraws,
                                             retireVersion);
                        ActivateDrawReferences(m_state, object.objectId,
                                               preparedOperation.draws);
                    }
                    WriteLiveObject(m_state, preparedOperation.primitive,
                                    preparedOperation.bounds,
                                    preparedOperation.transform,
                                    preparedOperation.draws, object);
                    break;
                case GPUSceneTransaction::OperationType::Remove:
                    m_state.objectToPrimitive.erase(preparedOperation.objectId);
                    RetireDrawReferences(m_state, preparedOperation.previousDraws,
                                         retireVersion);
                    RetireRow(m_state.mirror.bounds, m_state.boundsSlots,
                              preparedOperation.bounds, retireVersion);
                    RetireRow(m_state.mirror.transforms, m_state.transformSlots,
                              preparedOperation.transform, retireVersion);
                    RetireRow(m_state.mirror.primitives, m_state.primitiveSlots,
                              preparedOperation.primitive, retireVersion);
                    break;
            }
        }
        m_state.freePrimitiveSlots.erase(
            m_state.freePrimitiveSlots.begin(),
            m_state.freePrimitiveSlots.begin() + prepared.reusedPrimitiveCount);
        m_state.freeBoundsSlots.erase(
            m_state.freeBoundsSlots.begin(),
            m_state.freeBoundsSlots.begin() + prepared.reusedBoundsCount);
        m_state.freeTransformSlots.erase(
            m_state.freeTransformSlots.begin(),
            m_state.freeTransformSlots.begin() + prepared.reusedTransformCount);
        for (auto it = prepared.reusedDrawBlockIndices.rbegin();
             it != prepared.reusedDrawBlockIndices.rend(); ++it)
        {
            m_state.freeDrawBlocks.erase(
                m_state.freeDrawBlocks.begin() + static_cast<size_t>(*it));
        }
    }

    bool GPUSceneDatabase::IsLiveInState(
        const State& state,
        GPUScenePrimitiveRef primitive) const
    {
        return IsLiveRef(state.primitiveSlots, primitive);
    }

    std::optional<GPUSceneDatabase::DrawReferences> GPUSceneDatabase::GetDrawReferences(
        const State& state,
        GPUScenePrimitiveRef primitive) const
    {
        const GPUScenePrimitiveRow& primitiveRow =
            state.mirror.primitives[primitive.slot];
        DrawReferences result;
        if (primitiveRow.drawCount == 0)
        {
            return primitiveRow.firstDraw.IsValid()
                       ? std::nullopt
                       : std::optional<DrawReferences>(std::move(result));
        }

        if (!primitiveRow.firstDraw.IsValid() ||
            primitiveRow.firstDraw.slot >= state.drawSlots.size() ||
            primitiveRow.drawCount >
                state.drawSlots.size() - primitiveRow.firstDraw.slot)
        {
            return std::nullopt;
        }

        const uint32 drawBlockGeneration = primitiveRow.firstDraw.generation;
        for (uint32 offset = 0; offset < primitiveRow.drawCount; ++offset)
        {
            const uint32 drawSlot = primitiveRow.firstDraw.slot + offset;
            const GPUSceneDrawMetadataRow& draw = state.mirror.draws[drawSlot];
            const GPUSceneDrawRef drawRef{drawSlot, drawBlockGeneration};
            if (draw.header.generation != drawBlockGeneration ||
                !IsLiveRef(state.drawSlots, drawRef) || draw.primitive != primitive ||
                draw.header.objectId != primitiveRow.header.objectId ||
                !IsLiveRef(state.materialSlots, draw.material) ||
                !IsLiveRef(state.geometrySlots, draw.geometry))
            {
                return std::nullopt;
            }

            const GPUSceneMaterialRow& material =
                state.mirror.materials[draw.material.slot];
            const GPUSceneGeometryRow& geometry =
                state.mirror.geometries[draw.geometry.slot];
            if (material.header.generation != draw.material.generation ||
                geometry.header.generation != draw.geometry.generation ||
                material.header.objectId != primitiveRow.header.objectId ||
                geometry.header.objectId != primitiveRow.header.objectId)
            {
                return std::nullopt;
            }

        }
        result.firstDraw = primitiveRow.firstDraw;
        const GPUSceneDrawMetadataRow& first =
            state.mirror.draws[primitiveRow.firstDraw.slot];
        result.firstMaterial = first.material;
        result.firstGeometry = first.geometry;
        result.count = primitiveRow.drawCount;
        return result;
    }

    bool GPUSceneDatabase::AllocateSingleSlot(
        const std::vector<SlotRecord>& slots,
        const std::vector<uint32>& freeSlots,
        size_t& inOutReuseCount,
        size_t& inOutAppendCount,
        uint32& outSlot,
        uint32& outGeneration,
        bool& outAppended) const noexcept
    {
        if (inOutReuseCount < freeSlots.size())
        {
            const uint32 slot = freeSlots[inOutReuseCount];
            if (slot == 0 || slot >= slots.size())
            {
                return false;
            }
            const SlotRecord& record = slots[slot];
            if (record.state != GPUSceneSlotState::Free ||
                record.generation == std::numeric_limits<uint32>::max())
            {
                return false;
            }
            ++inOutReuseCount;
            outSlot = slot;
            outGeneration = record.generation + 1U;
            outAppended = false;
            return true;
        }

        if (slots.size() > std::numeric_limits<uint32>::max() ||
            inOutAppendCount > std::numeric_limits<uint32>::max() - slots.size())
        {
            return false;
        }
        outSlot = static_cast<uint32>(slots.size() + inOutAppendCount);
        outGeneration = m_initialSlotGeneration;
        outAppended = true;
        ++inOutAppendCount;
        return true;
    }

    bool GPUSceneDatabase::AllocateDrawReferences(
        const State& state,
        PreparedTransaction& prepared,
        uint32 drawCount,
        DrawReferences& outReferences)
    {
        outReferences = {};
        if (drawCount == 0)
        {
            return true;
        }

        for (uint32 index = 0; index < state.freeDrawBlocks.size(); ++index)
        {
            if (std::find(prepared.reusedDrawBlockIndices.begin(),
                          prepared.reusedDrawBlockIndices.end(), index) !=
                prepared.reusedDrawBlockIndices.end())
            {
                continue;
            }

            const DrawBlock& block = state.freeDrawBlocks[index];
            if (block.count != drawCount ||
                block.generation == std::numeric_limits<uint32>::max() ||
                block.firstMaterialSlot == 0 || block.firstGeometrySlot == 0 ||
                block.firstDrawSlot == 0 ||
                block.count > state.materialSlots.size() - block.firstMaterialSlot ||
                block.count > state.geometrySlots.size() - block.firstGeometrySlot ||
                block.count > state.drawSlots.size() - block.firstDrawSlot)
            {
                continue;
            }

            bool blockIsFree = true;
            for (uint32 offset = 0; offset < block.count; ++offset)
            {
                const SlotRecord& material =
                    state.materialSlots[block.firstMaterialSlot + offset];
                const SlotRecord& geometry =
                    state.geometrySlots[block.firstGeometrySlot + offset];
                const SlotRecord& draw =
                    state.drawSlots[block.firstDrawSlot + offset];
                if (material.state != GPUSceneSlotState::Free ||
                    geometry.state != GPUSceneSlotState::Free ||
                    draw.state != GPUSceneSlotState::Free ||
                    material.generation != block.generation ||
                    geometry.generation != block.generation ||
                    draw.generation != block.generation)
                {
                    blockIsFree = false;
                    break;
                }
            }
            if (!blockIsFree)
            {
                continue;
            }

            prepared.reusedDrawBlockIndices.push_back(index);
            const uint32 generation = block.generation + 1U;
            outReferences.firstMaterial = {block.firstMaterialSlot, generation};
            outReferences.firstGeometry = {block.firstGeometrySlot, generation};
            outReferences.firstDraw = {block.firstDrawSlot, generation};
            outReferences.count = block.count;
            outReferences.appended = false;
            return true;
        }

        const auto canAppend = [drawCount](size_t existing, size_t appended)
        {
            return existing <= std::numeric_limits<uint32>::max() &&
                   appended <= std::numeric_limits<uint32>::max() - existing &&
                   drawCount <= std::numeric_limits<uint32>::max() - existing - appended;
        };
        if (!canAppend(state.materialSlots.size(), prepared.delta.materials) ||
            !canAppend(state.geometrySlots.size(), prepared.delta.geometries) ||
            !canAppend(state.drawSlots.size(), prepared.delta.draws))
        {
            return false;
        }

        outReferences.firstMaterial = {static_cast<uint32>(
            state.materialSlots.size() + prepared.delta.materials),
            m_initialSlotGeneration};
        outReferences.firstGeometry = {static_cast<uint32>(
            state.geometrySlots.size() + prepared.delta.geometries),
            m_initialSlotGeneration};
        outReferences.firstDraw = {static_cast<uint32>(
            state.drawSlots.size() + prepared.delta.draws),
            m_initialSlotGeneration};
        outReferences.count = drawCount;
        outReferences.appended = true;
        prepared.delta.materials += drawCount;
        prepared.delta.geometries += drawCount;
        prepared.delta.draws += drawCount;
        return true;
    }

    void GPUSceneDatabase::MarkDirtyRange(
        GPUSceneTableChangeSet& table,
        uint32 firstRow,
        uint32 rowCount) const
    {
        if (rowCount != 0)
        {
            table.dirtyRanges.push_back({firstRow, rowCount});
        }
    }

    void GPUSceneDatabase::SortAndMergeDirtyRanges(
        GPUSceneTableChangeSet& table) const noexcept
    {
        std::sort(table.dirtyRanges.begin(), table.dirtyRanges.end(),
                  [](const GPUSceneDirtyRowRange& lhs,
                     const GPUSceneDirtyRowRange& rhs)
                  {
                      return lhs.firstRow < rhs.firstRow;
                  });
        size_t writeIndex = 0;
        for (const GPUSceneDirtyRowRange range : table.dirtyRanges)
        {
            if (writeIndex == 0)
            {
                table.dirtyRanges[writeIndex++] = range;
                continue;
            }

            GPUSceneDirtyRowRange& previous = table.dirtyRanges[writeIndex - 1U];
            const uint64 previousEnd = static_cast<uint64>(previous.firstRow) +
                                       static_cast<uint64>(previous.rowCount);
            const uint64 rangeEnd = static_cast<uint64>(range.firstRow) +
                                    static_cast<uint64>(range.rowCount);
            if (static_cast<uint64>(range.firstRow) <= previousEnd)
            {
                const uint64 mergedEnd = std::max(previousEnd, rangeEnd);
                previous.rowCount = static_cast<uint32>(
                    mergedEnd - static_cast<uint64>(previous.firstRow));
            }
            else
            {
                table.dirtyRanges[writeIndex++] = range;
            }
        }
        table.dirtyRanges.resize(writeIndex);
    }

    void GPUSceneDatabase::PublishPreparedChangeSet(
        PreparedTransaction& prepared) noexcept
    {
        m_lastChangeSet.baseVersion = prepared.changeSet.baseVersion;
        m_lastChangeSet.committedVersion = prepared.changeSet.committedVersion;
        const auto publishTable = [](GPUSceneTableChangeSet& target,
                                     GPUSceneTableChangeSet& source)
        {
            target.fullTableDirty = source.fullTableDirty;
            target.dirtyRanges.swap(source.dirtyRanges);
        };
        publishTable(m_lastChangeSet.primitives, prepared.changeSet.primitives);
        publishTable(m_lastChangeSet.bounds, prepared.changeSet.bounds);
        publishTable(m_lastChangeSet.transforms, prepared.changeSet.transforms);
        publishTable(m_lastChangeSet.materials, prepared.changeSet.materials);
        publishTable(m_lastChangeSet.geometries, prepared.changeSet.geometries);
        publishTable(m_lastChangeSet.draws, prepared.changeSet.draws);
    }

    void GPUSceneDatabase::PublishFullChangeSetNoexcept(
        uint64 baseVersion,
        uint64 committedVersion) noexcept
    {
        m_lastChangeSet.baseVersion = baseVersion;
        m_lastChangeSet.committedVersion = committedVersion;
        const auto publishFull = [](GPUSceneTableChangeSet& table)
        {
            table.fullTableDirty = true;
            table.dirtyRanges.clear();
        };
        publishFull(m_lastChangeSet.primitives);
        publishFull(m_lastChangeSet.bounds);
        publishFull(m_lastChangeSet.transforms);
        publishFull(m_lastChangeSet.materials);
        publishFull(m_lastChangeSet.geometries);
        publishFull(m_lastChangeSet.draws);
    }

    void GPUSceneDatabase::WriteLiveObject(
        State& state,
        GPUScenePrimitiveRef primitive,
        GPUSceneBoundsRef bounds,
        GPUSceneTransformRef transform,
        const DrawReferences& draws,
        const GPUSceneObjectData& object) const noexcept
    {
        GPUScenePrimitiveRow primitiveRow;
        WriteLiveHeader(primitiveRow, object.objectId, primitive.generation);
        primitiveRow.bounds = bounds;
        primitiveRow.transform = transform;
        primitiveRow.firstDraw = draws.firstDraw;
        primitiveRow.drawCount = draws.count;
        primitiveRow.primitiveFlags = object.primitiveFlags;
        primitiveRow.layerMask = object.layerMask;
        primitiveRow.sortKey = PackGPUSceneUint64(object.sortKey);
        state.mirror.primitives[primitive.slot] = primitiveRow;

        GPUSceneBoundsRow boundsRow = object.bounds;
        GPUSceneTransformRow transformRow = object.transform;
        WriteLiveHeader(boundsRow, object.objectId, bounds.generation);
        WriteLiveHeader(transformRow, object.objectId, transform.generation);
        state.mirror.bounds[bounds.slot] = boundsRow;
        state.mirror.transforms[transform.slot] = transformRow;

        for (uint32 index = 0; index < primitiveRow.drawCount; ++index)
        {
            GPUSceneMaterialRow material = object.draws[index].material;
            GPUSceneGeometryRow geometry = object.draws[index].geometry;
            GPUSceneDrawMetadataRow draw = object.draws[index].draw;
            const GPUSceneMaterialRef materialRef{
                draws.firstMaterial.slot + index, draws.firstMaterial.generation};
            const GPUSceneGeometryRef geometryRef{
                draws.firstGeometry.slot + index, draws.firstGeometry.generation};
            const GPUSceneDrawRef drawRef{
                draws.firstDraw.slot + index, draws.firstDraw.generation};
            WriteLiveHeader(material, object.objectId, materialRef.generation);
            WriteLiveHeader(geometry, object.objectId, geometryRef.generation);
            WriteLiveHeader(draw, object.objectId, drawRef.generation);
            draw.primitive = primitive;
            draw.material = materialRef;
            draw.geometry = geometryRef;
            draw.sortKey = PackGPUSceneUint64(object.sortKey);
            state.mirror.materials[materialRef.slot] = material;
            state.mirror.geometries[geometryRef.slot] = geometry;
            state.mirror.draws[drawRef.slot] = draw;
        }
    }

    void GPUSceneDatabase::ActivateDrawReferences(
        State& state,
        uint64 objectId,
        const DrawReferences& draws) const noexcept
    {
        for (uint32 index = 0; index < draws.count; ++index)
        {
            if (draws.appended)
            {
                AppendSlot(state.mirror.materials, state.materialSlots, objectId,
                           draws.firstMaterial.generation);
                AppendSlot(state.mirror.geometries, state.geometrySlots, objectId,
                           draws.firstGeometry.generation);
                AppendSlot(state.mirror.draws, state.drawSlots, objectId,
                           draws.firstDraw.generation);
            }
            else
            {
                ActivateSlot(state.mirror.materials, state.materialSlots,
                             draws.firstMaterial.slot + index, objectId,
                             draws.firstMaterial.generation);
                ActivateSlot(state.mirror.geometries, state.geometrySlots,
                             draws.firstGeometry.slot + index, objectId,
                             draws.firstGeometry.generation);
                ActivateSlot(state.mirror.draws, state.drawSlots,
                             draws.firstDraw.slot + index, objectId,
                             draws.firstDraw.generation);
            }
        }
    }

    void GPUSceneDatabase::RetireDrawReferences(
        State& state,
        const DrawReferences& draws,
        uint64 retireVersion) const noexcept
    {
        if (draws.count == 0)
        {
            return;
        }
        for (uint32 index = 0; index < draws.count; ++index)
        {
            RetireRow(state.mirror.materials, state.materialSlots,
                      GPUSceneMaterialRef{draws.firstMaterial.slot + index,
                                          draws.firstMaterial.generation},
                      retireVersion);
            RetireRow(state.mirror.geometries, state.geometrySlots,
                      GPUSceneGeometryRef{draws.firstGeometry.slot + index,
                                          draws.firstGeometry.generation},
                      retireVersion);
            RetireRow(state.mirror.draws, state.drawSlots,
                      GPUSceneDrawRef{draws.firstDraw.slot + index,
                                      draws.firstDraw.generation},
                      retireVersion);
        }
        if (draws.firstDraw.generation != std::numeric_limits<uint32>::max())
        {
            state.retiredDrawBlocks.push_back(
                {retireVersion,
                 draws.firstMaterial.slot,
                 draws.firstGeometry.slot,
                 draws.firstDraw.slot,
                 draws.count,
                 draws.firstDraw.generation});
        }
    }
} // namespace RVX
