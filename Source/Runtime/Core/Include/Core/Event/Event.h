#pragma once

/**
 * @file Event.h
 * @brief Base event class and common event types
 * 
 * Enhanced with:
 * - Event channels for isolation
 * - Priority levels for handler ordering
 * - Source tracking for filtering
 */

#include <cstdint>
#include <typeindex>
#include <string>
#include <limits>
#include <functional>

namespace RVX
{
    // =========================================================================
    // Event Channel System
    // =========================================================================

    /**
     * @brief Event channel for isolating event domains
     * 
     * Channels allow different parts of the engine to have isolated
     * event buses. For example, UI events can be on a separate channel
     * from gameplay events.
     */
    enum class EventChannel : uint32_t
    {
        Default = 0,        ///< Default global channel
        Engine = 1,         ///< Engine-level events (frame, shutdown)
        Input = 2,          ///< Input events (keyboard, mouse, gamepad)
        Window = 3,         ///< Window events (resize, focus, close)
        Render = 4,         ///< Render events (device lost, swap chain)
        World = 5,          ///< World/scene events (load, unload)
        Entity = 6,         ///< Entity events (create, destroy, modify)
        Physics = 7,        ///< Physics events (collision, trigger)
        Audio = 8,          ///< Audio events (play, stop)
        UI = 9,             ///< UI events (click, hover, focus)
        Network = 10,       ///< Network events (connect, disconnect)
        Resource = 11,      ///< Resource events (load, unload, hot-reload)
        Debug = 12,         ///< Debug/profiling events
        
        User = 100,         ///< Start of user-defined channels
        All = 0xFFFFFFFF    ///< Special: subscribe to all channels
    };

    /**
     * @brief Event handler priority
     * 
     * Higher priority handlers are called first.
     * Use for things like input capture or event filtering.
     */
    enum class EventPriority : int32_t
    {
        Lowest = -1000,
        Low = -100,
        Normal = 0,
        High = 100,
        Highest = 1000,
        
        // Special priorities
        Monitor = std::numeric_limits<int32_t>::max(),  ///< Always called first, for debugging/logging
        Final = std::numeric_limits<int32_t>::min()     ///< Always called last
    };

    /**
     * @brief Event source identifier for filtering
     */
    struct EventSource
    {
        uint64_t id = 0;
        const char* name = nullptr;

        EventSource() = default;
        EventSource(uint64_t sourceId, const char* sourceName = nullptr)
            : id(sourceId), name(sourceName) {}

        bool operator==(const EventSource& other) const { return id == other.id; }
        bool operator!=(const EventSource& other) const { return id != other.id; }
        
        static EventSource None() { return EventSource{0, "None"}; }
        static EventSource System() { return EventSource{1, "System"}; }
    };

    /**
     * @brief Base class for all events
     * 
     * Events are used for decoupled communication between modules.
     * Derive from this class to create custom events.
     * 
     * Enhanced features:
     * - Channel isolation
     * - Source tracking
     * - Priority-based dispatch
     */
    struct Event
    {
        virtual ~Event() = default;
        
        /// Get the type name for debugging
        virtual const char* GetTypeName() const = 0;
        
        /// Get unique type ID for fast dispatch
        virtual std::type_index GetTypeIndex() const = 0;
        
        /// Get the channel this event should be published to
        virtual EventChannel GetChannel() const { return EventChannel::Default; }
        
        /// Whether this event has been handled (stops propagation if true)
        mutable bool handled = false;
        
        /// Source of this event (for filtering)
        EventSource source;
        
        /// Timestamp when event was created (optional, 0 = not set)
        uint64_t timestamp = 0;
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

    /**
     * @brief Helper macro to implement event type info with channel
     */
    #define RVX_EVENT_TYPE_CHANNEL(EventClass, Channel) \
        RVX_EVENT_TYPE(EventClass) \
        EventChannel GetChannel() const override { return Channel; }

    // =========================================================================
    // Event Filter
    // =========================================================================

    /**
     * @brief Filter for event subscription
     * 
     * Allows subscribers to filter which events they receive based on
     * source, channel, or custom criteria.
     */
    struct EventFilter
    {
        /// Only receive events from this source (0 = any source)
        uint64_t sourceId = 0;
        
        /// Only receive events on these channels (All = no filtering)
        EventChannel channelMask = EventChannel::All;
        
        /// Custom filter function (optional)
        std::function<bool(const Event&)> customFilter;

        /// Check if an event passes this filter
        bool Accepts(const Event& event) const
        {
            // Source filter
            if (sourceId != 0 && event.source.id != sourceId)
                return false;
            
            // Channel filter
            if (channelMask != EventChannel::All)
            {
                if (static_cast<uint32_t>(event.GetChannel()) != static_cast<uint32_t>(channelMask))
                    return false;
            }
            
            // Custom filter
            if (customFilter && !customFilter(event))
                return false;
            
            return true;
        }

        /// Create a filter for a specific source
        static EventFilter FromSource(uint64_t id) 
        { 
            EventFilter f; 
            f.sourceId = id; 
            return f; 
        }

        /// Create a filter for a specific channel
        static EventFilter FromChannel(EventChannel channel) 
        { 
            EventFilter f; 
            f.channelMask = channel; 
            return f; 
        }
    };

    // =========================================================================
    // Common Engine Events
    // =========================================================================

    /// Window resize event
    struct WindowResizeEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(WindowResizeEvent, EventChannel::Window)
        
        uint32_t width = 0;
        uint32_t height = 0;
        
        WindowResizeEvent() = default;
        WindowResizeEvent(uint32_t w, uint32_t h) : width(w), height(h) {}
    };

    /// Window close event
    struct WindowCloseEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(WindowCloseEvent, EventChannel::Window)
    };

    /// Window focus event
    struct WindowFocusEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(WindowFocusEvent, EventChannel::Window)
        
        bool focused = false;
        
        WindowFocusEvent() = default;
        explicit WindowFocusEvent(bool f) : focused(f) {}
    };

    /// Frame begin event
    struct FrameBeginEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(FrameBeginEvent, EventChannel::Engine)
        
        uint64_t frameNumber = 0;
        float deltaTime = 0.0f;
    };

    /// Frame end event
    struct FrameEndEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(FrameEndEvent, EventChannel::Engine)
        
        uint64_t frameNumber = 0;
    };

    /// Engine shutdown request event
    struct ShutdownRequestEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(ShutdownRequestEvent, EventChannel::Engine)
        
        int exitCode = 0;
        const char* reason = nullptr;
    };

    /// Entity created event
    struct EntityCreatedEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(EntityCreatedEvent, EventChannel::Entity)
        
        uint64_t entityId = 0;
        const char* entityName = nullptr;
    };

    /// Entity destroyed event
    struct EntityDestroyedEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(EntityDestroyedEvent, EventChannel::Entity)
        
        uint64_t entityId = 0;
    };

    /// Resource loaded event
    struct ResourceLoadedEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(ResourceLoadedEvent, EventChannel::Resource)
        
        uint64_t resourceId = 0;
        const char* path = nullptr;
        const char* type = nullptr;
    };

    /// Resource hot-reload event
    struct ResourceReloadedEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(ResourceReloadedEvent, EventChannel::Resource)
        
        uint64_t resourceId = 0;
        const char* path = nullptr;
    };

} // namespace RVX
