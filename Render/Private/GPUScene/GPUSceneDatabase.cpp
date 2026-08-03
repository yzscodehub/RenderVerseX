#include "GPUScene/GPUSceneDatabase.h"

#include <limits>
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
            uint32 generation)
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

        // TODO(Task 11B): replace this whole-State copy with an incremental,
        // frame-integrable transaction journal before this database enters a
        // render-frame path. Task 11A favors straightforward strong atomicity.
        State candidate = m_state;
        std::unordered_set<uint64> affectedObjectIds;

        for (const GPUSceneTransaction::Operation& operation : transaction.m_operations)
        {
            GPUSceneCommitResult result;
            switch (operation.type)
            {
                case GPUSceneTransaction::OperationType::Add:
                    if (operation.object.objectId == 0)
                    {
                        return {GPUSceneCommitStatus::InvalidObjectId, m_state.mirror.version};
                    }
                    if (!affectedObjectIds.insert(operation.object.objectId).second)
                    {
                        return {GPUSceneCommitStatus::DuplicateObjectId, m_state.mirror.version};
                    }
                    result = AddObject(candidate, operation.object);
                    break;

                case GPUSceneTransaction::OperationType::Update:
                    if (operation.object.objectId == 0)
                    {
                        return {GPUSceneCommitStatus::InvalidObjectId, m_state.mirror.version};
                    }
                    if (!affectedObjectIds.insert(operation.object.objectId).second)
                    {
                        return {GPUSceneCommitStatus::DuplicateObjectId, m_state.mirror.version};
                    }
                    result = UpdateObject(candidate, operation.primitive, operation.object);
                    break;

                case GPUSceneTransaction::OperationType::Remove:
                {
                    if (!IsLiveInState(candidate, operation.primitive))
                    {
                        return {GPUSceneCommitStatus::StalePrimitiveRef, m_state.mirror.version};
                    }

                    const uint64 objectId =
                        candidate.primitiveSlots[operation.primitive.slot].objectId;
                    if (!affectedObjectIds.insert(objectId).second)
                    {
                        return {GPUSceneCommitStatus::DuplicateObjectId, m_state.mirror.version};
                    }
                    result = RemoveObject(candidate, operation.primitive);
                    break;
                }
            }

            if (!result.Succeeded())
            {
                result.committedVersion = m_state.mirror.version;
                return result;
            }
        }

        ++candidate.mirror.version;
        using std::swap;
        swap(m_state, candidate);
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

    GPUSceneCommitResult GPUSceneDatabase::AddObject(
        State& state,
        const GPUSceneObjectData& object) const
    {
        if (state.objectToPrimitive.contains(object.objectId))
        {
            return {GPUSceneCommitStatus::ObjectAlreadyExists, state.mirror.version};
        }

        const size_t drawCount = object.draws.size();
        if (drawCount > std::numeric_limits<uint32>::max() ||
            !CanAppendSlots(state.primitiveSlots.size(), 1, m_maxSlotCapacity) ||
            !CanAppendSlots(state.boundsSlots.size(), 1, m_maxSlotCapacity) ||
            !CanAppendSlots(state.transformSlots.size(), 1, m_maxSlotCapacity) ||
            !CanAppendSlots(state.materialSlots.size(), drawCount, m_maxSlotCapacity) ||
            !CanAppendSlots(state.geometrySlots.size(), drawCount, m_maxSlotCapacity) ||
            !CanAppendSlots(state.drawSlots.size(), drawCount, m_maxSlotCapacity))
        {
            return {GPUSceneCommitStatus::CapacityExhausted, state.mirror.version};
        }

        const uint32 primitiveSlot = AppendSlot(
            state.mirror.primitives,
            state.primitiveSlots,
            object.objectId,
            m_initialSlotGeneration);
        const uint32 boundsSlot = AppendSlot(
            state.mirror.bounds,
            state.boundsSlots,
            object.objectId,
            m_initialSlotGeneration);
        const uint32 transformSlot = AppendSlot(
            state.mirror.transforms,
            state.transformSlots,
            object.objectId,
            m_initialSlotGeneration);
        const GPUScenePrimitiveRef primitive{primitiveSlot, m_initialSlotGeneration};
        const GPUSceneBoundsRef bounds{boundsSlot, m_initialSlotGeneration};
        const GPUSceneTransformRef transform{transformSlot, m_initialSlotGeneration};
        const DrawReferences draws = AllocateDrawReferences(
            state,
            object.objectId,
            static_cast<uint32>(drawCount));
        WriteLiveObject(state, primitive, bounds, transform, draws, object);
        state.objectToPrimitive.emplace(object.objectId, primitive);
        return {GPUSceneCommitStatus::Success, state.mirror.version};
    }

    GPUSceneCommitResult GPUSceneDatabase::UpdateObject(
        State& state,
        GPUScenePrimitiveRef primitive,
        const GPUSceneObjectData& object) const
    {
        if (!IsLiveInState(state, primitive))
        {
            return {GPUSceneCommitStatus::StalePrimitiveRef, state.mirror.version};
        }

        const auto found = state.objectToPrimitive.find(object.objectId);
        if (found == state.objectToPrimitive.end())
        {
            return {GPUSceneCommitStatus::ObjectNotFound, state.mirror.version};
        }
        if (found->second != primitive)
        {
            return {GPUSceneCommitStatus::StalePrimitiveRef, state.mirror.version};
        }

        const GPUScenePrimitiveRow previous = state.mirror.primitives[primitive.slot];
        const std::optional<DrawReferences> existingDraws =
            GetDrawReferences(state, primitive);
        if (!existingDraws)
        {
            return {GPUSceneCommitStatus::InvalidDrawRange, state.mirror.version};
        }
        DrawReferences draws = *existingDraws;
        const size_t drawCount = object.draws.size();
        if (drawCount > std::numeric_limits<uint32>::max())
        {
            return {GPUSceneCommitStatus::CapacityExhausted, state.mirror.version};
        }

        if (drawCount != draws.draws.size())
        {
            if (!CanAppendSlots(
                    state.materialSlots.size(), drawCount, m_maxSlotCapacity) ||
                !CanAppendSlots(
                    state.geometrySlots.size(), drawCount, m_maxSlotCapacity) ||
                !CanAppendSlots(
                    state.drawSlots.size(), drawCount, m_maxSlotCapacity))
            {
                return {GPUSceneCommitStatus::CapacityExhausted, state.mirror.version};
            }

            RetireDrawReferences(state, draws);
            draws = AllocateDrawReferences(
                state,
                object.objectId,
                static_cast<uint32>(drawCount));
        }

        WriteLiveObject(state, primitive, previous.bounds, previous.transform, draws, object);
        return {GPUSceneCommitStatus::Success, state.mirror.version};
    }

    GPUSceneCommitResult GPUSceneDatabase::RemoveObject(
        State& state,
        GPUScenePrimitiveRef primitive) const
    {
        if (!IsLiveInState(state, primitive))
        {
            return {GPUSceneCommitStatus::StalePrimitiveRef, state.mirror.version};
        }

        const GPUScenePrimitiveRow previous = state.mirror.primitives[primitive.slot];
        const std::optional<DrawReferences> draws = GetDrawReferences(state, primitive);
        if (!draws)
        {
            return {GPUSceneCommitStatus::InvalidDrawRange, state.mirror.version};
        }
        state.objectToPrimitive.erase(state.primitiveSlots[primitive.slot].objectId);
        RetireDrawReferences(state, *draws);
        RetireRow(state.mirror.bounds, state.boundsSlots, previous.bounds);
        RetireRow(state.mirror.transforms, state.transformSlots, previous.transform);
        RetireRow(state.mirror.primitives, state.primitiveSlots, primitive);
        return {GPUSceneCommitStatus::Success, state.mirror.version};
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

        result.draws.reserve(primitiveRow.drawCount);
        result.materials.reserve(primitiveRow.drawCount);
        result.geometries.reserve(primitiveRow.drawCount);
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

            result.draws.push_back(drawRef);
            result.materials.push_back(draw.material);
            result.geometries.push_back(draw.geometry);
        }
        return result;
    }

    GPUSceneDatabase::DrawReferences GPUSceneDatabase::AllocateDrawReferences(
        State& state,
        uint64 objectId,
        uint32 drawCount) const
    {
        DrawReferences result;
        // Task 11A never reuses retired draw blocks. Completion-aware reuse must
        // advance and assign this one generation to the entire future block.
        const uint32 drawBlockGeneration = m_initialSlotGeneration;
        result.draws.reserve(drawCount);
        result.materials.reserve(drawCount);
        result.geometries.reserve(drawCount);
        for (uint32 index = 0; index < drawCount; ++index)
        {
            const uint32 materialSlot = AppendSlot(
                state.mirror.materials,
                state.materialSlots,
                objectId,
                m_initialSlotGeneration);
            const uint32 geometrySlot = AppendSlot(
                state.mirror.geometries,
                state.geometrySlots,
                objectId,
                m_initialSlotGeneration);
            const uint32 drawSlot = AppendSlot(
                state.mirror.draws,
                state.drawSlots,
                objectId,
                drawBlockGeneration);
            result.materials.push_back({materialSlot, m_initialSlotGeneration});
            result.geometries.push_back({geometrySlot, m_initialSlotGeneration});
            result.draws.push_back({drawSlot, drawBlockGeneration});
        }
        return result;
    }

    void GPUSceneDatabase::WriteLiveObject(
        State& state,
        GPUScenePrimitiveRef primitive,
        GPUSceneBoundsRef bounds,
        GPUSceneTransformRef transform,
        const DrawReferences& draws,
        const GPUSceneObjectData& object) const
    {
        GPUScenePrimitiveRow primitiveRow;
        WriteLiveHeader(primitiveRow, object.objectId, primitive.generation);
        primitiveRow.bounds = bounds;
        primitiveRow.transform = transform;
        primitiveRow.firstDraw = draws.draws.empty() ? GPUSceneDrawRef{} : draws.draws.front();
        primitiveRow.drawCount = static_cast<uint32>(draws.draws.size());
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
            WriteLiveHeader(material, object.objectId, draws.materials[index].generation);
            WriteLiveHeader(geometry, object.objectId, draws.geometries[index].generation);
            WriteLiveHeader(draw, object.objectId, draws.draws[index].generation);
            draw.primitive = primitive;
            draw.material = draws.materials[index];
            draw.geometry = draws.geometries[index];
            draw.sortKey = PackGPUSceneUint64(object.sortKey);
            state.mirror.materials[draws.materials[index].slot] = material;
            state.mirror.geometries[draws.geometries[index].slot] = geometry;
            state.mirror.draws[draws.draws[index].slot] = draw;
        }
    }

    void GPUSceneDatabase::RetireDrawReferences(
        State& state,
        const DrawReferences& draws) const
    {
        for (uint32 index = 0; index < draws.draws.size(); ++index)
        {
            RetireRow(state.mirror.materials, state.materialSlots, draws.materials[index]);
            RetireRow(state.mirror.geometries, state.geometrySlots, draws.geometries[index]);
            RetireRow(state.mirror.draws, state.drawSlots, draws.draws[index]);
        }
    }
} // namespace RVX
