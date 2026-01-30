#pragma once

/**
 * @file TouchState.h
 * @brief Touch input and gesture recognition
 */

#include "Core/Types.h"
#include <array>
#include <vector>

namespace RVX::HAL
{
    /// Maximum number of simultaneous touch points
    constexpr uint32 MAX_TOUCH_POINTS = 10;

    /**
     * @brief Touch point phase
     */
    enum class TouchPhase : uint8
    {
        None,       ///< Touch point not active
        Began,      ///< Touch just started
        Moved,      ///< Touch is moving
        Stationary, ///< Touch is stationary
        Ended,      ///< Touch just ended
        Cancelled   ///< Touch was cancelled (e.g., phone call)
    };

    /**
     * @brief A single touch point
     */
    struct TouchPoint
    {
        /// Unique identifier for this touch
        uint32 id = 0;

        /// Current phase
        TouchPhase phase = TouchPhase::None;

        /// Current position in screen coordinates
        float x = 0.0f;
        float y = 0.0f;

        /// Previous frame position
        float prevX = 0.0f;
        float prevY = 0.0f;

        /// Initial touch position (for gesture detection)
        float startX = 0.0f;
        float startY = 0.0f;

        /// Movement delta since last frame
        float deltaX = 0.0f;
        float deltaY = 0.0f;

        /// Touch pressure (0.0 to 1.0, if supported)
        float pressure = 1.0f;

        /// Touch radius/size (if supported)
        float radius = 0.0f;

        /// Timestamp when touch began (seconds)
        float startTime = 0.0f;

        /// Current timestamp
        float timestamp = 0.0f;

        bool IsActive() const
        {
            return phase != TouchPhase::None && 
                   phase != TouchPhase::Ended && 
                   phase != TouchPhase::Cancelled;
        }

        float GetDuration() const
        {
            return timestamp - startTime;
        }

        float GetTotalDeltaX() const { return x - startX; }
        float GetTotalDeltaY() const { return y - startY; }
        
        float GetTotalDistance() const
        {
            float dx = GetTotalDeltaX();
            float dy = GetTotalDeltaY();
            return std::sqrt(dx * dx + dy * dy);
        }
    };

    /**
     * @brief Recognized gesture types
     */
    enum class GestureType : uint8
    {
        None,
        Tap,            ///< Quick touch and release
        DoubleTap,      ///< Two quick taps
        LongPress,      ///< Touch and hold
        Swipe,          ///< Quick directional movement
        Pan,            ///< Dragging movement
        Pinch,          ///< Two-finger zoom
        Rotate          ///< Two-finger rotation
    };

    /**
     * @brief Swipe direction
     */
    enum class SwipeDirection : uint8
    {
        None,
        Up,
        Down,
        Left,
        Right
    };

    /**
     * @brief Gesture event data
     */
    struct GestureEvent
    {
        GestureType type = GestureType::None;

        /// Center position of the gesture
        float x = 0.0f;
        float y = 0.0f;

        /// For Swipe
        SwipeDirection swipeDirection = SwipeDirection::None;
        float swipeVelocity = 0.0f;

        /// For Pan
        float panDeltaX = 0.0f;
        float panDeltaY = 0.0f;

        /// For Pinch
        float pinchScale = 1.0f;      ///< Scale factor (1.0 = no change)
        float pinchDelta = 0.0f;      ///< Change in distance between fingers

        /// For Rotate
        float rotationAngle = 0.0f;   ///< Rotation in radians
        float rotationDelta = 0.0f;   ///< Change in rotation since last frame

        /// Number of fingers involved
        int fingerCount = 0;

        /// Is the gesture in progress?
        bool inProgress = false;
    };

    /**
     * @brief Current touch input state
     */
    struct TouchState
    {
        /// All touch points
        std::array<TouchPoint, MAX_TOUCH_POINTS> points{};

        /// Number of active touch points
        uint32 activeCount = 0;

        /// Pending gesture events
        std::vector<GestureEvent> gestures;

        // =====================================================================
        // Query methods
        // =====================================================================

        /// Get number of active touches
        uint32 GetActiveCount() const { return activeCount; }

        /// Get touch point by index (0 to activeCount-1)
        const TouchPoint* GetTouch(uint32 index) const
        {
            if (index >= MAX_TOUCH_POINTS)
                return nullptr;
            
            uint32 found = 0;
            for (const auto& point : points)
            {
                if (point.IsActive())
                {
                    if (found == index)
                        return &point;
                    ++found;
                }
            }
            return nullptr;
        }

        /// Find touch by ID
        const TouchPoint* GetTouchById(uint32 id) const
        {
            for (const auto& point : points)
            {
                if (point.id == id && point.IsActive())
                    return &point;
            }
            return nullptr;
        }

        /// Check if there's a touch at the given position (with radius)
        bool HasTouchAt(float px, float py, float radius) const
        {
            for (const auto& point : points)
            {
                if (point.IsActive())
                {
                    float dx = point.x - px;
                    float dy = point.y - py;
                    if (dx * dx + dy * dy <= radius * radius)
                        return true;
                }
            }
            return false;
        }

        /// Clear gesture events
        void ClearGestures()
        {
            gestures.clear();
        }
    };

    /**
     * @brief Gesture recognition settings
     */
    struct GestureSettings
    {
        /// Maximum movement for a tap (in pixels)
        float tapMaxDistance = 20.0f;

        /// Maximum duration for a tap (in seconds)
        float tapMaxDuration = 0.3f;

        /// Maximum time between double taps
        float doubleTapMaxInterval = 0.3f;

        /// Minimum duration for a long press
        float longPressMinDuration = 0.5f;

        /// Minimum velocity for a swipe (pixels per second)
        float swipeMinVelocity = 500.0f;

        /// Minimum distance for a swipe (in pixels)
        float swipeMinDistance = 50.0f;

        /// Minimum scale change for pinch detection
        float pinchMinScale = 0.1f;

        /// Minimum rotation for rotation detection (in radians)
        float rotationMinAngle = 0.1f;
    };

    /**
     * @brief Abstract interface for touch backend
     */
    class ITouchBackend
    {
    public:
        virtual ~ITouchBackend() = default;

        /**
         * @brief Poll touch state
         * @param state Touch state to fill
         */
        virtual void Poll(TouchState& state) = 0;

        /**
         * @brief Check if touch input is available
         */
        virtual bool IsAvailable() const = 0;
    };

} // namespace RVX::HAL

// Backward compatibility
namespace RVX
{
    using HAL::MAX_TOUCH_POINTS;
    using HAL::TouchPhase;
    using HAL::TouchPoint;
    using HAL::GestureType;
    using HAL::SwipeDirection;
    using HAL::GestureEvent;
    using HAL::TouchState;
    using HAL::GestureSettings;
    using HAL::ITouchBackend;
}
