#pragma once

#include "Core/Types.h"

#include <functional>
#include <new>

namespace RVX
{
    // =============================================================================
    // Generic Handle Type
    // A type-safe handle that wraps an index with generation for safe access
    // =============================================================================
    template<typename Tag, typename IndexType = uint32>
    class Handle
    {
    public:
        using Index = IndexType;

        // Invalid handle constant
        static constexpr Index InvalidIndex = static_cast<Index>(-1);

        constexpr Handle() = default;

        static constexpr Handle Create(Index index, uint32 generation = 0)
        {
            Handle h;
            h.m_index = index;
            h.m_generation = generation;
            return h;
        }

        static constexpr Handle Invalid()
        {
            Handle h;
            h.m_index = InvalidIndex;
            h.m_generation = 0;
            return h;
        }

        constexpr bool IsValid() const { return m_index != InvalidIndex; }
        constexpr Index GetIndex() const { return m_index; }
        constexpr uint32 GetGeneration() const { return m_generation; }

        constexpr bool operator==(const Handle& other) const
        {
            return m_index == other.m_index && m_generation == other.m_generation;
        }

        constexpr bool operator!=(const Handle& other) const
        {
            return !(*this == other);
        }

        constexpr bool operator<(const Handle& other) const
        {
            return m_index < other.m_index ||
                   (m_index == other.m_index && m_generation < other.m_generation);
        }

        /** @brief Stable non-zero value for diagnostics and transient bridge payloads. */
        constexpr uint64 GetPackedValue() const
        {
            static_assert(sizeof(Index) <= sizeof(uint32),
                          "Packed handle values support index types up to 32 bits");
            return (static_cast<uint64>(m_generation) << 32u) |
                   static_cast<uint64>(static_cast<uint32>(m_index) + 1u);
        }

        constexpr explicit operator bool() const { return IsValid(); }

    private:
        Index m_index = InvalidIndex;
        uint32 m_generation = 0;
    };

    // =============================================================================
    // Handle Pool
    // Manages allocation and deallocation of handles with generational safety
    // =============================================================================
    template<typename HandleType>
    class HandlePool
    {
    public:
        using Index = typename HandleType::Index;

        HandlePool() = default;

        explicit HandlePool(uint32 initialCapacity)
        {
            m_entries.reserve(initialCapacity);
        }

        HandleType Allocate()
        {
            if (!m_freeList.empty())
            {
                Index index = m_freeList.back();
                m_freeList.pop_back();
                m_entries[index].allocated = true;
                return HandleType::Create(index, m_entries[index].generation);
            }

            Index index = static_cast<Index>(m_entries.size());
            m_entries.push_back({true, 0});
            return HandleType::Create(index, 0);
        }

        /**
         * @brief Try to retire a handle without exposing a partial pool mutation.
         *
         * The free-list growth is the only potentially allocating operation.
         * Reserve it before invalidating the entry so callers on noexcept
         * boundaries can fail closed and retry with the exact same handle.
         */
        [[nodiscard]] bool TryFree(HandleType handle) noexcept
        {
            if (!IsValid(handle))
                return false;

            try
            {
                if (m_freeList.size() == m_freeList.capacity())
                {
                    m_freeList.reserve(m_freeList.size() + 1);
                }
            }
            catch (...)
            {
                return false;
            }

            Index index = handle.GetIndex();
            m_entries[index].allocated = false;
            m_entries[index].generation++;
            m_freeList.push_back(index);
            return true;
        }

        /**
         * @brief Compatibility release API that preserves the previous throwing contract.
         *
         * @throws std::bad_alloc when free-list growth cannot be prepared. The
         * handle remains valid in that case and can be retried through
         * TryFree().
         */
        void Free(HandleType handle)
        {
            if (!IsValid(handle))
                return;
            if (!TryFree(handle))
            {
                throw std::bad_alloc();
            }
        }

        bool IsValid(HandleType handle) const
        {
            if (!handle.IsValid())
                return false;

            Index index = handle.GetIndex();
            if (index >= m_entries.size())
                return false;

            return m_entries[index].allocated &&
                   m_entries[index].generation == handle.GetGeneration();
        }

        void Clear()
        {
            m_entries.clear();
            m_freeList.clear();
        }

        uint32 GetAllocatedCount() const
        {
            return static_cast<uint32>(m_entries.size() - m_freeList.size());
        }

    private:
        struct Entry
        {
            bool allocated = false;
            uint32 generation = 0;
        };

        std::vector<Entry> m_entries;
        std::vector<Index> m_freeList;
    };

} // namespace RVX

namespace std
{
    /** @brief Hash support for generation-safe RVX handles. */
    template<typename Tag, typename IndexType>
    struct hash<RVX::Handle<Tag, IndexType>>
    {
        size_t operator()(const RVX::Handle<Tag, IndexType>& handle) const noexcept
        {
            const size_t indexHash = hash<IndexType>{}(handle.GetIndex());
            const size_t generationHash = hash<RVX::uint32>{}(handle.GetGeneration());
            return indexHash ^ (generationHash + static_cast<size_t>(0x9e3779b9u) +
                                (indexHash << 6u) + (indexHash >> 2u));
        }
    };
} // namespace std
