#pragma once

#include "Core/Types.h"
#include "ECS/Entity.h"

#include <typeindex>
#include <vector>

namespace RVX::ECS
{
    /**
     * @brief Process-local fragment registration order.
     *
     * This ID orders structural removals deterministically for one running
     * build. It is deliberately not a serialized or cross-build type ID.
     */
    using FragmentTypeId = uint32;
    inline constexpr FragmentTypeId RVX_INVALID_FRAGMENT_TYPE_ID = 0;

    enum class StructuralChangeKind : uint8
    {
        EntityCreated = 0,
        EntityDestroyed,
        FragmentAdded,
        FragmentRemoved,
        FragmentEnabled,
        FragmentDisabled,
        EntityEnabled,
        EntityDisabled,
    };

    struct StructuralChange
    {
        uint64 sequence = 0;
        EntityHandle entity = EntityHandle::Invalid();
        StructuralChangeKind kind = StructuralChangeKind::EntityCreated;
        std::type_index fragmentType = std::type_index(typeid(void));
        FragmentTypeId fragmentTypeId = RVX_INVALID_FRAGMENT_TYPE_ID;
    };

    struct StructuralJournalCursor
    {
        uint64 nextSequence = 1;
    };

    enum class StructuralJournalContinuity : uint8
    {
        Continuous = 0,
        Lost,
    };

    struct StructuralJournalRead
    {
        StructuralJournalContinuity continuity = StructuralJournalContinuity::Continuous;
        /**
         * @brief Stable copy for this read operation.
         *
         * The journal may receive structural changes or be trimmed immediately
         * after Read returns; consumers may retain this vector safely.
         */
        std::vector<StructuralChange> changes;
        uint64 nextSequence = 1;
    };

    /**
     * @brief Monotonic structural-change log with explicit cursor continuity.
     *
     * A Lost read intentionally returns no partial payload: consumers rebuild
     * authoritatively, then continue from the returned next sequence.
     */
    class StructuralJournal
    {
    public:
        explicit StructuralJournal(uint32 capacity = 8192);

        [[nodiscard]] StructuralJournalCursor CreateCursor() const;
        [[nodiscard]] StructuralJournalRead Read(StructuralJournalCursor& cursor) const;

        void ReserveForAppend(uint32 count = 1);
        void TrimBefore(uint64 sequence);
        void Clear();
        void Swap(StructuralJournal& other) noexcept;

        [[nodiscard]] uint64 GetNextSequence() const { return m_nextSequence; }
        [[nodiscard]] uint64 GetFirstAvailableSequence() const;
        [[nodiscard]] uint32 GetCapacity() const { return m_capacity; }

    private:
        friend class Registry;

        void AppendPrepared(StructuralChangeKind kind,
                            EntityHandle entity,
                            std::type_index fragmentType,
                            FragmentTypeId fragmentTypeId = RVX_INVALID_FRAGMENT_TYPE_ID) noexcept;

        std::vector<StructuralChange> m_changes;
        size_t m_firstChangeIndex = 0;
        uint64 m_firstAvailableSequence = 1;
        uint64 m_nextSequence = 1;
        uint32 m_capacity = 8192;
    };
} // namespace RVX::ECS
