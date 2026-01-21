#pragma once

/**
 * @file Event.h
 * @brief Base event class and common event types
 */

#include <cstdint>
#include <typeindex>

namespace RVX
{
    /**
     * @brief Base class for all events
     * 
     * Events are used for decoupled communication between modules.
     * Derive from this class to create custom events.
     */
    struct Event
    {
        virtual ~Event() = default;
        
        /// Get the type name for debugging
        virtual const char* GetTypeName() const = 0;
        
        /// Get unique type ID for fast dispatch
        virtual std::type_index GetTypeIndex() const = 0;
        
        /// Whether this event has been handled (stops propagation if true)
        bool handled = false;
    };

    /**
     * @brief Helper macro to implement event type info
     * 
     * Usage:
     * @code
     * struct MyEvent : Event
     * {
     *     RVX_EVENT_TYPE(MyEvent)
     *     int myData;
     * };
     * @endcode
     */
    #define RVX_EVENT_TYPE(EventClass) \
        const char* GetTypeName() const override { return #EventClass; } \
        std::type_index GetTypeIndex() const override { return std::type_index(typeid(EventClass)); } \
        static std::type_index StaticTypeIndex() { return std::type_index(typeid(EventClass)); }

    // =========================================================================
    // Common Engine Events
    // =========================================================================

    /// Window resize event
    struct WindowResizeEvent : Event
    {
        RVX_EVENT_TYPE(WindowResizeEvent)
        
        uint32_t width = 0;
        uint32_t height = 0;
        
        WindowResizeEvent() = default;
        WindowResizeEvent(uint32_t w, uint32_t h) : width(w), height(h) {}
    };

    /// Window close event
    struct WindowCloseEvent : Event
    {
        RVX_EVENT_TYPE(WindowCloseEvent)
    };

    /// Frame begin event
    struct FrameBeginEvent : Event
    {
        RVX_EVENT_TYPE(FrameBeginEvent)
        
        uint64_t frameNumber = 0;
        float deltaTime = 0.0f;
    };

    /// Frame end event
    struct FrameEndEvent : Event
    {
        RVX_EVENT_TYPE(FrameEndEvent)
        
        uint64_t frameNumber = 0;
    };

} // namespace RVX
