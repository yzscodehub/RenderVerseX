#pragma once

/**
 * @file GestureRecognizer.h
 * @brief Gesture recognition for touch input
 * 
 * Detects common gestures like tap, swipe, pinch, and rotate
 * from raw touch input data.
 */

#include "HAL/Input/TouchState.h"
#include <vector>
#include <functional>

namespace RVX
{
    /**
     * @brief Callback for gesture events
     */
    using GestureCallback = std::function<void(const GestureEvent&)>;

    /**
     * @brief Gesture recognizer - detects gestures from touch input
     */
    class GestureRecognizer
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        GestureRecognizer();
        ~GestureRecognizer() = default;

        // =====================================================================
        // Configuration
        // =====================================================================

        /**
         * @brief Set gesture detection settings
         */
        void SetSettings(const GestureSettings& settings);

        /**
         * @brief Get current settings
         */
        const GestureSettings& GetSettings() const { return m_settings; }

        // =====================================================================
        // Processing
        // =====================================================================

        /**
         * @brief Process touch input and detect gestures
         * @param touch Current touch state
         * @param deltaTime Frame delta time
         * @return Detected gestures
         */
        std::vector<GestureEvent> Process(TouchState& touch, float deltaTime);

        /**
         * @brief Reset all gesture state
         */
        void Reset();

        // =====================================================================
        // Callbacks
        // =====================================================================

        /**
         * @brief Set callback for tap gestures
         */
        void OnTap(GestureCallback callback) { m_onTap = std::move(callback); }

        /**
         * @brief Set callback for double tap gestures
         */
        void OnDoubleTap(GestureCallback callback) { m_onDoubleTap = std::move(callback); }

        /**
         * @brief Set callback for long press gestures
         */
        void OnLongPress(GestureCallback callback) { m_onLongPress = std::move(callback); }

        /**
         * @brief Set callback for swipe gestures
         */
        void OnSwipe(GestureCallback callback) { m_onSwipe = std::move(callback); }

        /**
         * @brief Set callback for pan gestures
         */
        void OnPan(GestureCallback callback) { m_onPan = std::move(callback); }

        /**
         * @brief Set callback for pinch gestures
         */
        void OnPinch(GestureCallback callback) { m_onPinch = std::move(callback); }

        /**
         * @brief Set callback for rotate gestures
         */
        void OnRotate(GestureCallback callback) { m_onRotate = std::move(callback); }

    private:
        // =====================================================================
        // Internal Types
        // =====================================================================

        struct TapCandidate
        {
            float startX = 0.0f;
            float startY = 0.0f;
            float time = 0.0f;
            bool valid = false;
        };

        struct PinchState
        {
            float initialDistance = 0.0f;
            float lastDistance = 0.0f;
            float initialAngle = 0.0f;
            float lastAngle = 0.0f;
            bool active = false;
        };

        // =====================================================================
        // Internal Methods
        // =====================================================================

        void DetectTap(const TouchState& touch, std::vector<GestureEvent>& events);
        void DetectLongPress(const TouchState& touch, float deltaTime, std::vector<GestureEvent>& events);
        void DetectSwipe(const TouchState& touch, std::vector<GestureEvent>& events);
        void DetectPan(const TouchState& touch, std::vector<GestureEvent>& events);
        void DetectPinchAndRotate(const TouchState& touch, std::vector<GestureEvent>& events);

        float Distance(float x1, float y1, float x2, float y2) const;
        float Angle(float x1, float y1, float x2, float y2) const;
        SwipeDirection GetSwipeDirection(float dx, float dy) const;

        void InvokeCallback(const GestureEvent& event);

        // =====================================================================
        // Data
        // =====================================================================

        GestureSettings m_settings;

        // Tap detection
        TapCandidate m_lastTap;
        float m_doubleTapTimer = 0.0f;
        bool m_waitingForDoubleTap = false;

        // Long press detection
        float m_longPressTimer = 0.0f;
        bool m_longPressTriggered = false;
        uint32 m_longPressTouchId = 0;

        // Swipe/Pan detection
        bool m_isPanning = false;
        float m_panStartX = 0.0f;
        float m_panStartY = 0.0f;

        // Pinch/Rotate detection
        PinchState m_pinch;

        // Previous touch count for state transitions
        uint32 m_prevTouchCount = 0;

        // Callbacks
        GestureCallback m_onTap;
        GestureCallback m_onDoubleTap;
        GestureCallback m_onLongPress;
        GestureCallback m_onSwipe;
        GestureCallback m_onPan;
        GestureCallback m_onPinch;
        GestureCallback m_onRotate;
    };

} // namespace RVX
