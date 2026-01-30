/**
 * @file AnimationEvent.h
 * @brief Animation event system for triggering callbacks during animation playback
 * 
 * Supports:
 * - Time-based animation events
 * - Event parameters (variant data)
 * - Event dispatching and handling
 */

#pragma once

#include "Animation/Core/Types.h"
#include "Core/MathTypes.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace RVX::Animation
{

// ============================================================================
// Event Parameter Types
// ============================================================================

/**
 * @brief Variant type for event parameters
 */
using EventParamValue = std::variant<
    bool,
    int32_t,
    float,
    std::string,
    Vec2,
    Vec3,
    Vec4
>;

/**
 * @brief Named event parameter
 */
struct EventParameter
{
    std::string name;
    EventParamValue value;

    EventParameter() = default;
    EventParameter(const std::string& n, const EventParamValue& v)
        : name(n), value(v) {}

    // ========================================================================
    // Type-safe accessors
    // ========================================================================

    template<typename T>
    const T* Get() const
    {
        return std::get_if<T>(&value);
    }

    template<typename T>
    T GetOr(const T& defaultValue) const
    {
        const T* ptr = std::get_if<T>(&value);
        return ptr ? *ptr : defaultValue;
    }

    bool GetBool(bool defaultValue = false) const { return GetOr(defaultValue); }
    int32_t GetInt(int32_t defaultValue = 0) const { return GetOr(defaultValue); }
    float GetFloat(float defaultValue = 0.0f) const { return GetOr(defaultValue); }
    std::string GetString(const std::string& defaultValue = "") const { return GetOr(defaultValue); }
    Vec3 GetVec3(const Vec3& defaultValue = Vec3(0.0f)) const { return GetOr(defaultValue); }
};

// ============================================================================
// Animation Event
// ============================================================================

/**
 * @brief Animation event triggered at a specific time
 */
struct AnimationEvent
{
    /// Event name/identifier
    std::string name;

    /// Time at which the event should be triggered (microseconds)
    TimeUs time{0};

    /// Optional function/callback name
    std::string functionName;

    /// Event parameters
    std::unordered_map<std::string, EventParamValue> parameters;

    // ========================================================================
    // Construction
    // ========================================================================

    AnimationEvent() = default;

    AnimationEvent(const std::string& eventName, TimeUs eventTime)
        : name(eventName), time(eventTime) {}

    AnimationEvent(const std::string& eventName, double timeSeconds)
        : name(eventName), time(SecondsToTimeUs(timeSeconds)) {}

    // ========================================================================
    // Parameter Access
    // ========================================================================

    void SetParameter(const std::string& paramName, const EventParamValue& value)
    {
        parameters[paramName] = value;
    }

    template<typename T>
    void SetParameter(const std::string& paramName, const T& value)
    {
        parameters[paramName] = EventParamValue(value);
    }

    bool HasParameter(const std::string& paramName) const
    {
        return parameters.find(paramName) != parameters.end();
    }

    template<typename T>
    T GetParameter(const std::string& paramName, const T& defaultValue = T{}) const
    {
        auto it = parameters.find(paramName);
        if (it == parameters.end()) return defaultValue;

        const T* ptr = std::get_if<T>(&it->second);
        return ptr ? *ptr : defaultValue;
    }

    float GetFloat(const std::string& paramName, float defaultValue = 0.0f) const
    {
        return GetParameter<float>(paramName, defaultValue);
    }

    int32_t GetInt(const std::string& paramName, int32_t defaultValue = 0) const
    {
        return GetParameter<int32_t>(paramName, defaultValue);
    }

    bool GetBool(const std::string& paramName, bool defaultValue = false) const
    {
        return GetParameter<bool>(paramName, defaultValue);
    }

    std::string GetString(const std::string& paramName, const std::string& defaultValue = "") const
    {
        return GetParameter<std::string>(paramName, defaultValue);
    }

    Vec3 GetVec3(const std::string& paramName, const Vec3& defaultValue = Vec3(0.0f)) const
    {
        return GetParameter<Vec3>(paramName, defaultValue);
    }

    // ========================================================================
    // Comparison (for sorting)
    // ========================================================================

    bool operator<(const AnimationEvent& other) const { return time < other.time; }
    bool operator==(const AnimationEvent& other) const
    {
        return time == other.time && name == other.name;
    }
};

// ============================================================================
// Animation Event Track
// ============================================================================

/**
 * @brief Collection of animation events for a clip
 */
struct AnimationEventTrack
{
    /// All events sorted by time
    std::vector<AnimationEvent> events;

    // ========================================================================
    // Event Management
    // ========================================================================

    void AddEvent(const AnimationEvent& event)
    {
        events.push_back(event);
        SortEvents();
    }

    void AddEvent(const std::string& name, TimeUs time)
    {
        events.emplace_back(name, time);
        SortEvents();
    }

    void AddEvent(const std::string& name, double timeSeconds)
    {
        events.emplace_back(name, SecondsToTimeUs(timeSeconds));
        SortEvents();
    }

    void RemoveEvent(size_t index)
    {
        if (index < events.size())
        {
            events.erase(events.begin() + static_cast<ptrdiff_t>(index));
        }
    }

    void Clear() { events.clear(); }

    void SortEvents()
    {
        std::sort(events.begin(), events.end(),
            [](const AnimationEvent& a, const AnimationEvent& b) {
                return a.time < b.time;
            });
    }

    // ========================================================================
    // Query
    // ========================================================================

    size_t GetEventCount() const { return events.size(); }
    bool IsEmpty() const { return events.empty(); }

    const AnimationEvent* GetEvent(size_t index) const
    {
        return index < events.size() ? &events[index] : nullptr;
    }

    AnimationEvent* GetEvent(size_t index)
    {
        return index < events.size() ? &events[index] : nullptr;
    }

    /**
     * @brief Find events in a time range [startTime, endTime)
     */
    std::vector<const AnimationEvent*> FindEventsInRange(TimeUs startTime, TimeUs endTime) const
    {
        std::vector<const AnimationEvent*> result;

        for (const auto& event : events)
        {
            if (event.time >= startTime && event.time < endTime)
            {
                result.push_back(&event);
            }
            else if (event.time >= endTime)
            {
                break; // Events are sorted, no need to continue
            }
        }

        return result;
    }

    /**
     * @brief Find events at exact time
     */
    std::vector<const AnimationEvent*> FindEventsAt(TimeUs time) const
    {
        std::vector<const AnimationEvent*> result;

        for (const auto& event : events)
        {
            if (event.time == time)
            {
                result.push_back(&event);
            }
            else if (event.time > time)
            {
                break;
            }
        }

        return result;
    }

    /**
     * @brief Find first event with given name
     */
    const AnimationEvent* FindEventByName(const std::string& name) const
    {
        for (const auto& event : events)
        {
            if (event.name == name) return &event;
        }
        return nullptr;
    }

    /**
     * @brief Get time range of events
     */
    std::pair<TimeUs, TimeUs> GetTimeRange() const
    {
        if (events.empty()) return {0, 0};
        return {events.front().time, events.back().time};
    }
};

// ============================================================================
// Animation Event Dispatcher
// ============================================================================

/**
 * @brief Event handler callback signature
 */
using EventHandler = std::function<void(const AnimationEvent& event)>;

/**
 * @brief Named event handler callback signature
 */
using NamedEventHandler = std::function<void(const std::string& eventName, const AnimationEvent& event)>;

/**
 * @brief Dispatches animation events to registered handlers
 * 
 * Usage:
 * @code
 * AnimationEventDispatcher dispatcher;
 * 
 * // Register handler for specific event
 * dispatcher.RegisterHandler("Footstep", [](const AnimationEvent& e) {
 *     PlaySound(e.GetString("sound_file"));
 * });
 * 
 * // Register global handler
 * dispatcher.SetGlobalHandler([](const AnimationEvent& e) {
 *     Log("Event: {}", e.name);
 * });
 * 
 * // Dispatch events
 * dispatcher.Dispatch(eventTrack, previousTime, currentTime);
 * @endcode
 */
class AnimationEventDispatcher
{
public:
    AnimationEventDispatcher() = default;

    // =========================================================================
    // Handler Registration
    // =========================================================================

    /**
     * @brief Register handler for a specific event name
     */
    void RegisterHandler(const std::string& eventName, EventHandler handler)
    {
        m_handlers[eventName] = std::move(handler);
    }

    /**
     * @brief Unregister handler for an event
     */
    void UnregisterHandler(const std::string& eventName)
    {
        m_handlers.erase(eventName);
    }

    /**
     * @brief Set global handler for all events
     */
    void SetGlobalHandler(EventHandler handler)
    {
        m_globalHandler = std::move(handler);
    }

    /**
     * @brief Clear global handler
     */
    void ClearGlobalHandler()
    {
        m_globalHandler = nullptr;
    }

    /**
     * @brief Check if handler exists for event
     */
    bool HasHandler(const std::string& eventName) const
    {
        return m_handlers.find(eventName) != m_handlers.end();
    }

    /**
     * @brief Clear all handlers
     */
    void ClearAllHandlers()
    {
        m_handlers.clear();
        m_globalHandler = nullptr;
    }

    // =========================================================================
    // Event Dispatching
    // =========================================================================

    /**
     * @brief Dispatch a single event
     */
    void Dispatch(const AnimationEvent& event) const
    {
        // Call specific handler if exists
        auto it = m_handlers.find(event.name);
        if (it != m_handlers.end() && it->second)
        {
            it->second(event);
        }

        // Call global handler
        if (m_globalHandler)
        {
            m_globalHandler(event);
        }
    }

    /**
     * @brief Dispatch events in a time range
     * @param track Event track to check
     * @param previousTime Previous frame time
     * @param currentTime Current frame time
     * @param looped Whether animation looped this frame
     * @param duration Animation duration (for loop handling)
     */
    void Dispatch(const AnimationEventTrack& track,
                  TimeUs previousTime,
                  TimeUs currentTime,
                  bool looped = false,
                  TimeUs duration = 0) const
    {
        if (track.IsEmpty())
            return;

        if (!looped)
        {
            // Normal playback - dispatch events in range [previousTime, currentTime)
            auto events = track.FindEventsInRange(previousTime, currentTime);
            for (const auto* event : events)
            {
                Dispatch(*event);
            }
        }
        else
        {
            // Looped - dispatch events from previousTime to end, then start to currentTime
            auto eventsEnd = track.FindEventsInRange(previousTime, duration);
            for (const auto* event : eventsEnd)
            {
                Dispatch(*event);
            }

            auto eventsStart = track.FindEventsInRange(0, currentTime);
            for (const auto* event : eventsStart)
            {
                Dispatch(*event);
            }
        }
    }

    /**
     * @brief Dispatch events with reverse playback support
     */
    void DispatchReverse(const AnimationEventTrack& track,
                         TimeUs previousTime,
                         TimeUs currentTime) const
    {
        if (track.IsEmpty())
            return;

        // For reverse playback, swap times and dispatch
        auto events = track.FindEventsInRange(currentTime, previousTime);
        // Dispatch in reverse order
        for (auto it = events.rbegin(); it != events.rend(); ++it)
        {
            Dispatch(**it);
        }
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    size_t GetHandlerCount() const { return m_handlers.size(); }
    bool HasGlobalHandler() const { return m_globalHandler != nullptr; }

private:
    std::unordered_map<std::string, EventHandler> m_handlers;
    EventHandler m_globalHandler;
};

// ============================================================================
// Event Track Cursor (for efficient sequential access)
// ============================================================================

/**
 * @brief Cursor for efficient event lookup during playback
 */
struct EventTrackCursor
{
    size_t lastEventIndex{0};
    TimeUs lastTime{0};

    void Reset()
    {
        lastEventIndex = 0;
        lastTime = 0;
    }
};

} // namespace RVX::Animation
