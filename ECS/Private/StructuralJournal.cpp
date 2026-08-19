#include "ECS/StructuralJournal.h"

#include <algorithm>
#include <utility>

namespace RVX::ECS
{
    StructuralJournal::StructuralJournal(uint32 capacity)
        : m_capacity(std::max(capacity, 1u))
    {
    }

    StructuralJournalCursor StructuralJournal::CreateCursor() const
    {
        return {.nextSequence = m_nextSequence};
    }

    StructuralJournalRead StructuralJournal::Read(StructuralJournalCursor& cursor) const
    {
        StructuralJournalRead result;
        if (cursor.nextSequence < m_firstAvailableSequence || cursor.nextSequence > m_nextSequence)
        {
            result.continuity = StructuralJournalContinuity::Lost;
            // A partial delta after a continuity loss is not authoritative.
            // Callers rebuild from their source snapshot, then resume at now.
            cursor.nextSequence = m_nextSequence;
            result.nextSequence = m_nextSequence;
            return result;
        }

        const uint64 startSequence = cursor.nextSequence;
        const size_t startIndex = m_firstChangeIndex +
                                  static_cast<size_t>(startSequence - m_firstAvailableSequence);
        result.changes.assign(m_changes.begin() + static_cast<std::ptrdiff_t>(startIndex), m_changes.end());
        cursor.nextSequence = m_nextSequence;
        result.nextSequence = m_nextSequence;
        return result;
    }

    void StructuralJournal::ReserveForAppend(uint32 count)
    {
        if (count == 0)
        {
            return;
        }

        const size_t requiredCapacity = m_changes.size() + static_cast<size_t>(count);
        if (requiredCapacity > m_changes.capacity())
        {
            const size_t currentCapacity = m_changes.capacity();
            const size_t geometricCapacity = currentCapacity == 0 ? 8u : currentCapacity + currentCapacity / 2u;
            m_changes.reserve(std::max(requiredCapacity, geometricCapacity));
        }
    }

    void StructuralJournal::TrimBefore(uint64 sequence)
    {
        const uint64 retainedFrom = std::clamp(sequence, m_firstAvailableSequence, m_nextSequence);
        const size_t removeCount = static_cast<size_t>(retainedFrom - m_firstAvailableSequence);
        m_firstChangeIndex += removeCount;
        m_firstAvailableSequence = retainedFrom;

        // Advance the logical prefix cheaply for normal consumer cursors. The
        // physical erase is deliberately amortized so frequent trimming does
        // not repeatedly move every retained change.
        if (m_firstChangeIndex >= 1024u && m_firstChangeIndex * 2u >= m_changes.size())
        {
            m_changes.erase(m_changes.begin(),
                            m_changes.begin() + static_cast<std::ptrdiff_t>(m_firstChangeIndex));
            m_firstChangeIndex = 0;
        }
    }

    void StructuralJournal::Clear()
    {
        m_changes.clear();
        m_firstChangeIndex = 0;
        m_firstAvailableSequence = m_nextSequence;
    }

    void StructuralJournal::Swap(StructuralJournal& other) noexcept
    {
        using std::swap;
        m_changes.swap(other.m_changes);
        swap(m_firstChangeIndex, other.m_firstChangeIndex);
        swap(m_firstAvailableSequence, other.m_firstAvailableSequence);
        swap(m_nextSequence, other.m_nextSequence);
        swap(m_capacity, other.m_capacity);
    }

    uint64 StructuralJournal::GetFirstAvailableSequence() const
    {
        return m_firstAvailableSequence;
    }

    void StructuralJournal::AppendPrepared(StructuralChangeKind kind,
                                           EntityHandle entity,
                                           std::type_index fragmentType,
                                           FragmentTypeId fragmentTypeId) noexcept
    {
        m_changes.push_back({
            .sequence = m_nextSequence++,
            .entity = entity,
            .kind = kind,
            .fragmentType = fragmentType,
            .fragmentTypeId = fragmentTypeId,
        });

        const size_t retainedCount = m_changes.size() - m_firstChangeIndex;
        if (retainedCount > static_cast<size_t>(m_capacity))
        {
            TrimBefore(m_nextSequence - static_cast<uint64>(m_capacity));
        }
    }
} // namespace RVX::ECS
