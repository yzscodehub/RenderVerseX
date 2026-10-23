#pragma once

#include "Core/Handle.h"
#include "ECS/Fragment.h"

#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace RVX::ECS
{
    /** @brief Type-specific tag for generation-safe dynamic-buffer handles. */
    template<Fragment T>
    struct DynamicBufferHandleTag {};

    /** @brief Strong identifier for one variable-length buffer of T values. */
    template<Fragment T>
    using DynamicBufferHandle = Handle<DynamicBufferHandleTag<T>>;

    /**
     * @brief Owns independently addressable variable-length buffers of data-only values.
     *
     * Read returns an owned snapshot rather than an element pointer or span. This
     * keeps callers from retaining an alias across Replace, Append, Clear, or
     * Release, all of which may relocate internal storage.
     */
    template<Fragment T>
        requires std::copy_constructible<T>
    class DynamicBufferPool final
    {
    public:
        using Handle = DynamicBufferHandle<T>;

        /** @brief Create a buffer containing a copy of values, or Invalid on failure. */
        [[nodiscard]] Handle Create(std::span<const T> values = {}) noexcept
        {
            try
            {
                std::vector<T> snapshot(values.begin(), values.end());

                if (!m_freeIndices.empty())
                {
                    const uint32 index = m_freeIndices.back();
                    Slot& slot = m_slots[index];
                    slot.values.swap(snapshot);
                    slot.allocated = true;
                    m_freeIndices.pop_back();
                    ++m_activeCount;
                    return Handle::Create(index, slot.generation);
                }

                if (m_slots.size() >= static_cast<size_t>(Handle::InvalidIndex))
                {
                    return Handle::Invalid();
                }

                Slot slot;
                slot.values.swap(snapshot);
                slot.allocated = true;
                m_slots.push_back(std::move(slot));
                ++m_activeCount;
                return Handle::Create(static_cast<uint32>(m_slots.size() - 1u), 0);
            }
            catch (...)
            {
                return Handle::Invalid();
            }
        }

        /** @brief Return an owned value snapshot, or std::nullopt if stale or unable to copy. */
        [[nodiscard]] std::optional<std::vector<T>> Read(Handle handle) const noexcept
        {
            if (!IsValid(handle))
            {
                return std::nullopt;
            }

            try
            {
                return m_slots[handle.GetIndex()].values;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        /** @brief Replace one buffer atomically with a copy of values. */
        bool Replace(Handle handle, std::span<const T> values) noexcept
        {
            if (!IsValid(handle))
            {
                return false;
            }

            try
            {
                std::vector<T> replacement(values.begin(), values.end());
                m_slots[handle.GetIndex()].values.swap(replacement);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        /** @brief Append a copied sequence atomically; an empty sequence is a no-op. */
        bool Append(Handle handle, std::span<const T> values) noexcept
        {
            if (!IsValid(handle))
            {
                return false;
            }

            if (values.empty())
            {
                return true;
            }

            try
            {
                Slot& slot = m_slots[handle.GetIndex()];
                std::vector<T> replacement = slot.values;
                replacement.insert(replacement.end(), values.begin(), values.end());
                slot.values.swap(replacement);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        /** @brief Append one copied value atomically. */
        bool Append(Handle handle, const T& value) noexcept
        {
            return Append(handle, std::span<const T>(&value, 1));
        }

        /** @brief Remove all values from one live buffer without releasing its handle. */
        bool Clear(Handle handle) noexcept
        {
            if (!IsValid(handle))
            {
                return false;
            }

            m_slots[handle.GetIndex()].values.clear();
            return true;
        }

        /** @brief Release one buffer and invalidate its handle generation. */
        bool Release(Handle handle) noexcept
        {
            if (!IsValid(handle))
            {
                return false;
            }

            Slot& slot = m_slots[handle.GetIndex()];
            const bool retiresSlot = slot.generation == std::numeric_limits<uint32>::max();
            if (!retiresSlot)
            {
                try
                {
                    if (m_freeIndices.size() == m_freeIndices.capacity())
                    {
                        m_freeIndices.reserve(m_freeIndices.size() + 1u);
                    }
                }
                catch (...)
                {
                    return false;
                }
            }

            slot.values.clear();
            slot.allocated = false;
            --m_activeCount;

            if (retiresSlot)
            {
                slot.retired = true;
                return true;
            }

            ++slot.generation;
            m_freeIndices.push_back(handle.GetIndex());
            return true;
        }

        [[nodiscard]] bool IsValid(Handle handle) const noexcept
        {
            if (!handle.IsValid() || handle.GetIndex() >= m_slots.size())
            {
                return false;
            }

            const Slot& slot = m_slots[handle.GetIndex()];
            return slot.allocated && !slot.retired && slot.generation == handle.GetGeneration();
        }

        [[nodiscard]] uint32 GetBufferCount() const noexcept { return m_activeCount; }

    private:
        struct Slot
        {
            std::vector<T> values;
            uint32 generation = 0;
            bool allocated = false;
            bool retired = false;
        };

        std::vector<Slot> m_slots;
        std::vector<uint32> m_freeIndices;
        uint32 m_activeCount = 0;
    };
} // namespace RVX::ECS
