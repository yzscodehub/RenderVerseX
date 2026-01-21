#pragma once

/**
 * @file EventHandle.h
 * @brief Handle for event subscriptions
 */

#include <cstdint>
#include <functional>

namespace RVX
{
    /**
     * @brief Handle to an event subscription
     * 
     * Used to unsubscribe from events. Keep this handle alive
     * as long as you want to receive events.
     */
    class EventHandle
    {
    public:
        using HandleId = uint64_t;
        static constexpr HandleId InvalidId = 0;

        EventHandle() : m_id(InvalidId) {}
        explicit EventHandle(HandleId id) : m_id(id) {}

        bool IsValid() const { return m_id != InvalidId; }
        HandleId GetId() const { return m_id; }

        bool operator==(const EventHandle& other) const { return m_id == other.m_id; }
        bool operator!=(const EventHandle& other) const { return m_id != other.m_id; }

        /// Reset handle (does not unsubscribe)
        void Reset() { m_id = InvalidId; }

    private:
        HandleId m_id;
    };

    /**
     * @brief RAII wrapper for automatic unsubscription
     * 
     * When this object goes out of scope, it automatically
     * unsubscribes from the event.
     */
    class ScopedEventHandle
    {
    public:
        using UnsubscribeFunc = std::function<void(EventHandle)>;

        ScopedEventHandle() = default;
        ScopedEventHandle(EventHandle handle, UnsubscribeFunc unsubscribe)
            : m_handle(handle), m_unsubscribe(std::move(unsubscribe)) {}

        ~ScopedEventHandle()
        {
            if (m_handle.IsValid() && m_unsubscribe)
            {
                m_unsubscribe(m_handle);
            }
        }

        // Non-copyable
        ScopedEventHandle(const ScopedEventHandle&) = delete;
        ScopedEventHandle& operator=(const ScopedEventHandle&) = delete;

        // Movable
        ScopedEventHandle(ScopedEventHandle&& other) noexcept
            : m_handle(other.m_handle), m_unsubscribe(std::move(other.m_unsubscribe))
        {
            other.m_handle.Reset();
        }

        ScopedEventHandle& operator=(ScopedEventHandle&& other) noexcept
        {
            if (this != &other)
            {
                if (m_handle.IsValid() && m_unsubscribe)
                {
                    m_unsubscribe(m_handle);
                }
                m_handle = other.m_handle;
                m_unsubscribe = std::move(other.m_unsubscribe);
                other.m_handle.Reset();
            }
            return *this;
        }

        EventHandle Get() const { return m_handle; }
        bool IsValid() const { return m_handle.IsValid(); }

        /// Release ownership without unsubscribing
        EventHandle Release()
        {
            EventHandle handle = m_handle;
            m_handle.Reset();
            return handle;
        }

    private:
        EventHandle m_handle;
        UnsubscribeFunc m_unsubscribe;
    };

} // namespace RVX
