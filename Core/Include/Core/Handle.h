#pragma once

#include "Core/Types.h"

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

        Handle() = default;

        static Handle Create(Index index, uint32 generation = 0)
        {
            Handle h;
            h.m_index = index;
            h.m_generation = generation;
            return h;
        }

        static Handle Invalid()
        {
            Handle h;
            h.m_index = InvalidIndex;
            h.m_generation = 0;
            return h;
        }

        bool IsValid() const { return m_index != InvalidIndex; }
        Index GetIndex() const { return m_index; }
        uint32 GetGeneration() const { return m_generation; }

        bool operator==(const Handle& other) const
        {
            return m_index == other.m_index && m_generation == other.m_generation;
        }

        bool operator!=(const Handle& other) const
        {
            return !(*this == other);
        }

        explicit operator bool() const { return IsValid(); }

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

        void Free(HandleType handle)
        {
            if (!IsValid(handle))
                return;

            Index index = handle.GetIndex();
            m_entries[index].allocated = false;
            m_entries[index].generation++;
            m_freeList.push_back(index);
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
