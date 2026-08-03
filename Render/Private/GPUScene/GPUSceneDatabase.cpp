#include "GPUScene/GPUSceneDatabase.h"

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
            Ref reference)
        {
            SlotRecord& record = slots[reference.slot];
            WriteTombstoneHeader(rows[reference.slot], record.objectId, record.generation);
            record.state = record.generation == std::numeric_limits<uint32>::max()
                               ? GPUSceneSlotState::PermanentlyRetired
                               : GPUSceneSlotState::Retired;
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
            slots.push_back({objectId, generation, GPUSceneSlotState::Live});
            rows.emplace_back();
            return slot;
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

        // Finalization mutates only touched rows. All dynamic allocations,
        // including map nodes for Adds, completed above; this path is noexcept.
        FinalizePrepared(prepared);
        ++m_state.mirror.version;
        return {GPUSceneCommitStatus::Success, m_state.mirror.version};
    }

    void GPUSceneDatabase::Clear() noexcept
    {
        const auto tombstone = [](auto& rows, auto& slots)
        {
            for (size_t slot = 1; slot < slots.size(); ++slot)
            {
                if (slots[slot].state != GPUSceneSlotState::Live)
                {
                    continue;
                }
                WriteTombstoneHeader(rows[slot], slots[slot].objectId,
                                     slots[slot].generation);
                slots[slot].state = slots[slot].generation ==
                                             std::numeric_limits<uint32>::max()
                                         ? GPUSceneSlotState::PermanentlyRetired
                                         : GPUSceneSlotState::Retired;
            }
        };
        tombstone(m_state.mirror.primitives, m_state.primitiveSlots);
        tombstone(m_state.mirror.bounds, m_state.boundsSlots);
        tombstone(m_state.mirror.transforms, m_state.transformSlots);
        tombstone(m_state.mirror.materials, m_state.materialSlots);
        tombstone(m_state.mirror.geometries, m_state.geometrySlots);
        tombstone(m_state.mirror.draws, m_state.drawSlots);
        m_state.objectToPrimitive.clear();
        if (m_state.mirror.version != std::numeric_limits<uint64>::max())
        {
            ++m_state.mirror.version;
        }
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
                    const size_t drawCount = operation.object.draws.size();
                    if (!addCount(outPrepared.delta.primitives, 1) ||
                        !addCount(outPrepared.delta.bounds, 1) ||
                        !addCount(outPrepared.delta.transforms, 1) ||
                        !addCount(outPrepared.delta.materials, drawCount) ||
                        !addCount(outPrepared.delta.geometries, drawCount) ||
                        !addCount(outPrepared.delta.draws, drawCount) ||
                        !addCount(outPrepared.delta.objects, 1))
                    {
                        return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
                    }
                    PreparedOperation prepared;
                    prepared.operation = &operation;
                    prepared.objectId = objectId;
                    prepared.primitive = {static_cast<uint32>(m_state.primitiveSlots.size() +
                                                               outPrepared.delta.primitives - 1),
                                          m_initialSlotGeneration};
                    prepared.bounds = {static_cast<uint32>(m_state.boundsSlots.size() +
                                                            outPrepared.delta.bounds - 1),
                                       m_initialSlotGeneration};
                    prepared.transform = {static_cast<uint32>(m_state.transformSlots.size() +
                                                               outPrepared.delta.transforms - 1),
                                          m_initialSlotGeneration};
                    CapacityDelta prior = outPrepared.delta;
                    prior.materials -= drawCount;
                    prior.geometries -= drawCount;
                    prior.draws -= drawCount;
                    prepared.draws = MakeFutureDrawReferences(
                        m_state, prior, static_cast<uint32>(drawCount));
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
                    const size_t drawCount = operation.object.draws.size();
                    if (drawCount != previous->count &&
                        (!addCount(outPrepared.delta.materials, drawCount) ||
                         !addCount(outPrepared.delta.geometries, drawCount) ||
                         !addCount(outPrepared.delta.draws, drawCount)))
                    {
                        return {GPUSceneCommitStatus::CapacityExhausted, m_state.mirror.version};
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
                    prepared.replaceDraws = drawCount != previous->count;
                    if (prepared.replaceDraws)
                    {
                        CapacityDelta prior = outPrepared.delta;
                        prior.materials -= drawCount;
                        prior.geometries -= drawCount;
                        prior.draws -= drawCount;
                        prepared.draws = MakeFutureDrawReferences(
                            m_state, prior, static_cast<uint32>(drawCount));
                    }
                    else
                    {
                        prepared.draws = *previous;
                    }
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
        for (PreparedOperation& preparedOperation : prepared.operations)
        {
            const GPUSceneTransaction::Operation& operation =
                *preparedOperation.operation;
            const GPUSceneObjectData& object = operation.object;
            switch (operation.type)
            {
                case GPUSceneTransaction::OperationType::Add:
                    AppendSlot(m_state.mirror.primitives, m_state.primitiveSlots,
                               object.objectId, m_initialSlotGeneration);
                    AppendSlot(m_state.mirror.bounds, m_state.boundsSlots,
                               object.objectId, m_initialSlotGeneration);
                    AppendSlot(m_state.mirror.transforms, m_state.transformSlots,
                               object.objectId, m_initialSlotGeneration);
                    AppendDrawReferences(m_state, object.objectId,
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
                        RetireDrawReferences(m_state, preparedOperation.previousDraws);
                        AppendDrawReferences(m_state, object.objectId,
                                             preparedOperation.draws);
                    }
                    WriteLiveObject(m_state, preparedOperation.primitive,
                                    preparedOperation.bounds,
                                    preparedOperation.transform,
                                    preparedOperation.draws, object);
                    break;
                case GPUSceneTransaction::OperationType::Remove:
                    m_state.objectToPrimitive.erase(preparedOperation.objectId);
                    RetireDrawReferences(m_state, preparedOperation.previousDraws);
                    RetireRow(m_state.mirror.bounds, m_state.boundsSlots,
                              preparedOperation.bounds);
                    RetireRow(m_state.mirror.transforms, m_state.transformSlots,
                              preparedOperation.transform);
                    RetireRow(m_state.mirror.primitives, m_state.primitiveSlots,
                              preparedOperation.primitive);
                    break;
            }
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

    GPUSceneDatabase::DrawReferences GPUSceneDatabase::MakeFutureDrawReferences(
        const State& state,
        const CapacityDelta& priorDelta,
        uint32 drawCount) const noexcept
    {
        DrawReferences result;
        if (drawCount == 0)
            return result;
        result.firstMaterial = {static_cast<uint32>(
            state.materialSlots.size() + priorDelta.materials), m_initialSlotGeneration};
        result.firstGeometry = {static_cast<uint32>(
            state.geometrySlots.size() + priorDelta.geometries), m_initialSlotGeneration};
        result.firstDraw = {static_cast<uint32>(
            state.drawSlots.size() + priorDelta.draws), m_initialSlotGeneration};
        result.count = drawCount;
        return result;
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

    void GPUSceneDatabase::AppendDrawReferences(
        State& state,
        uint64 objectId,
        const DrawReferences& draws) const noexcept
    {
        for (uint32 index = 0; index < draws.count; ++index)
        {
            AppendSlot(state.mirror.materials, state.materialSlots, objectId,
                       draws.firstMaterial.generation);
            AppendSlot(state.mirror.geometries, state.geometrySlots, objectId,
                       draws.firstGeometry.generation);
            AppendSlot(state.mirror.draws, state.drawSlots, objectId,
                       draws.firstDraw.generation);
        }
    }

    void GPUSceneDatabase::RetireDrawReferences(
        State& state,
        const DrawReferences& draws) const noexcept
    {
        for (uint32 index = 0; index < draws.count; ++index)
        {
            RetireRow(state.mirror.materials, state.materialSlots,
                      GPUSceneMaterialRef{draws.firstMaterial.slot + index,
                                          draws.firstMaterial.generation});
            RetireRow(state.mirror.geometries, state.geometrySlots,
                      GPUSceneGeometryRef{draws.firstGeometry.slot + index,
                                          draws.firstGeometry.generation});
            RetireRow(state.mirror.draws, state.drawSlots,
                      GPUSceneDrawRef{draws.firstDraw.slot + index,
                                      draws.firstDraw.generation});
        }
    }
} // namespace RVX
