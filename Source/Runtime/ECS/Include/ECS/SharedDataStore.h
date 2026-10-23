#pragma once

#include "Core/Handle.h"
#include "ECS/Fragment.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RVX::ECS
{
    /** @brief Type-specific tag for generation-safe immutable shared-data handles. */
    template<Fragment T>
    struct SharedDataHandleTag {};

    /** @brief Strong identifier for an interned immutable T value. */
    template<Fragment T>
    using SharedDataHandle = Handle<SharedDataHandleTag<T>>;

    /**
     * @brief FNV-1a hash over an exact object representation.
     *
     * This default deliberately treats object representation as the value
     * identity. Types with semantic equality that differs from byte equality
     * (for example values with meaningful normalization or padding concerns)
     * should supply matching Hash and Equal policies to SharedDataStore.
     */
    template<Fragment T>
    struct ByteValueHash
    {
        static_assert(std::has_unique_object_representations_v<T>,
                      "ByteValueHash requires a unique object representation; "
                      "supply matching Hash and Equal policies for padded or normalized values.");

        [[nodiscard]] size_t operator()(const T& value) const noexcept
        {
            constexpr uint64 offsetBasis = 14695981039346656037ull;
            constexpr uint64 prime = 1099511628211ull;

            uint64 hash = offsetBasis;
            const auto* bytes = reinterpret_cast<const uint8*>(&value);
            for (size_t index = 0; index < sizeof(T); ++index)
            {
                hash ^= bytes[index];
                hash *= prime;
            }
            return static_cast<size_t>(hash);
        }
    };

    /** @brief Equality over the exact object representation used by ByteValueHash. */
    template<Fragment T>
    struct ByteValueEqual
    {
        static_assert(std::has_unique_object_representations_v<T>,
                      "ByteValueEqual requires a unique object representation; "
                      "supply matching Hash and Equal policies for padded or normalized values.");

        [[nodiscard]] bool operator()(const T& left, const T& right) const noexcept
        {
            return std::memcmp(&left, &right, sizeof(T)) == 0;
        }
    };

    /**
     * @brief Interns immutable data-only values and tracks explicit shared ownership.
     *
     * Hash and Equal must describe the same value identity. Read returns a
     * value copy, preserving immutability and preventing callers from holding
     * an internal pointer across Acquire or Release.
     */
    template<Fragment T, typename Hash = ByteValueHash<T>, typename Equal = ByteValueEqual<T>>
        requires std::copy_constructible<T> &&
                 std::invocable<Hash&, const T&> &&
                 std::predicate<Equal&, const T&, const T&>
    class SharedDataStore final
    {
    public:
        using Handle = SharedDataHandle<T>;

        explicit SharedDataStore(Hash hash = {}, Equal equal = {})
            : m_hash(std::move(hash))
            , m_equal(std::move(equal))
        {
        }

        /** @brief Acquire a shared handle for value, or Invalid if preparation fails. */
        [[nodiscard]] Handle Acquire(const T& value) noexcept
        {
            try
            {
                const size_t hash = static_cast<size_t>(m_hash(value));
                const auto bucket = m_buckets.find(hash);
                if (bucket != m_buckets.end())
                {
                    for (uint32 index : bucket->second)
                    {
                        Slot& slot = m_slots[index];
                        if (!slot.allocated || slot.hash != hash || !m_equal(*slot.value, value))
                        {
                            continue;
                        }

                        if (slot.referenceCount == std::numeric_limits<uint32>::max())
                        {
                            return Handle::Invalid();
                        }

                        ++slot.referenceCount;
                        return Handle::Create(index, slot.generation);
                    }
                }

                return AcquireNew(value, hash);
            }
            catch (...)
            {
                return Handle::Invalid();
            }
        }

        /** @brief Return a value copy for a live handle, or std::nullopt if stale or unable to copy. */
        [[nodiscard]] std::optional<T> Read(Handle handle) const noexcept
        {
            if (!IsValid(handle))
            {
                return std::nullopt;
            }

            try
            {
                return m_slots[handle.GetIndex()].value;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        /** @brief Drop one acquired reference; the final release invalidates the handle. */
        bool Release(Handle handle) noexcept
        {
            if (!IsValid(handle))
            {
                return false;
            }

            Slot& slot = m_slots[handle.GetIndex()];
            if (slot.referenceCount > 1u)
            {
                --slot.referenceCount;
                return true;
            }

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

            const auto bucket = m_buckets.find(slot.hash);
            if (bucket == m_buckets.end() || !RemoveBucketIndex(bucket->second, handle.GetIndex()))
            {
                return false;
            }

            if (bucket->second.empty())
            {
                m_buckets.erase(bucket);
            }

            slot.value.reset();
            slot.referenceCount = 0;
            slot.allocated = false;
            --m_liveCount;

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

        [[nodiscard]] uint32 GetReferenceCount(Handle handle) const noexcept
        {
            return IsValid(handle) ? m_slots[handle.GetIndex()].referenceCount : 0;
        }

        [[nodiscard]] uint32 GetLiveCount() const noexcept { return m_liveCount; }

    private:
        struct Slot
        {
            std::optional<T> value;
            size_t hash = 0;
            uint32 generation = 0;
            uint32 referenceCount = 0;
            bool allocated = false;
            bool retired = false;
        };

        [[nodiscard]] Handle AcquireNew(const T& value, size_t hash) noexcept
        {
            if (m_slots.size() >= static_cast<size_t>(Handle::InvalidIndex) && m_freeIndices.empty())
            {
                return Handle::Invalid();
            }

            const bool reusesSlot = !m_freeIndices.empty();
            const uint32 index = reusesSlot
                                     ? m_freeIndices.back()
                                     : static_cast<uint32>(m_slots.size());

            try
            {
                const auto [bucket, inserted] = m_buckets.try_emplace(hash);
                try
                {
                    bucket->second.push_back(index);
                }
                catch (...)
                {
                    if (inserted)
                    {
                        m_buckets.erase(bucket);
                    }
                    return Handle::Invalid();
                }

                if (!reusesSlot)
                {
                    try
                    {
                        m_slots.emplace_back();
                    }
                    catch (...)
                    {
                        RemoveBucketIndex(bucket->second, index);
                        if (bucket->second.empty())
                        {
                            m_buckets.erase(bucket);
                        }
                        return Handle::Invalid();
                    }
                }

                Slot& slot = m_slots[index];
                try
                {
                    slot.value.emplace(value);
                }
                catch (...)
                {
                    if (!reusesSlot)
                    {
                        m_slots.pop_back();
                    }
                    RemoveBucketIndex(bucket->second, index);
                    if (bucket->second.empty())
                    {
                        m_buckets.erase(bucket);
                    }
                    return Handle::Invalid();
                }

                slot.hash = hash;
                slot.referenceCount = 1;
                slot.allocated = true;
                if (reusesSlot)
                {
                    m_freeIndices.pop_back();
                }
                ++m_liveCount;
                return Handle::Create(index, slot.generation);
            }
            catch (...)
            {
                return Handle::Invalid();
            }
        }

        static bool RemoveBucketIndex(std::vector<uint32>& bucket, uint32 index) noexcept
        {
            for (auto entry = bucket.begin(); entry != bucket.end(); ++entry)
            {
                if (*entry == index)
                {
                    bucket.erase(entry);
                    return true;
                }
            }
            return false;
        }

        Hash m_hash;
        Equal m_equal;
        std::vector<Slot> m_slots;
        std::vector<uint32> m_freeIndices;
        std::unordered_map<size_t, std::vector<uint32>> m_buckets;
        uint32 m_liveCount = 0;
    };
} // namespace RVX::ECS
