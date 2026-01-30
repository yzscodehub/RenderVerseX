/**
 * @file GestureRecognizer.cpp
 * @brief GestureRecognizer implementation
 */

#include "Runtime/Input/GestureRecognizer.h"
#include <cmath>
#include <algorithm>

namespace RVX
{

GestureRecognizer::GestureRecognizer()
{
    // Use default settings
}

void GestureRecognizer::SetSettings(const GestureSettings& settings)
{
    m_settings = settings;
}

std::vector<GestureEvent> GestureRecognizer::Process(TouchState& touch, float deltaTime)
{
    std::vector<GestureEvent> events;

    // Detect various gestures
    DetectTap(touch, events);
    DetectLongPress(touch, deltaTime, events);
    DetectSwipe(touch, events);
    DetectPan(touch, events);
    DetectPinchAndRotate(touch, events);

    // Update double tap timer
    if (m_waitingForDoubleTap)
    {
        m_doubleTapTimer += deltaTime;
        if (m_doubleTapTimer > m_settings.doubleTapMaxInterval)
        {
            // Double tap window expired, trigger single tap
            m_waitingForDoubleTap = false;
            if (m_lastTap.valid)
            {
                GestureEvent tap;
                tap.type = GestureType::Tap;
                tap.x = m_lastTap.startX;
                tap.y = m_lastTap.startY;
                tap.fingerCount = 1;
                events.push_back(tap);
                InvokeCallback(tap);
                m_lastTap.valid = false;
            }
        }
    }

    // Store touch count for next frame
    m_prevTouchCount = touch.activeCount;

    // Store detected gestures in touch state
    touch.gestures = events;

    return events;
}

void GestureRecognizer::Reset()
{
    m_lastTap = TapCandidate{};
    m_doubleTapTimer = 0.0f;
    m_waitingForDoubleTap = false;
    m_longPressTimer = 0.0f;
    m_longPressTriggered = false;
    m_longPressTouchId = 0;
    m_isPanning = false;
    m_pinch = PinchState{};
    m_prevTouchCount = 0;
}

void GestureRecognizer::DetectTap(const TouchState& touch, std::vector<GestureEvent>& events)
{
    // Look for touch that just ended
    for (const auto& point : touch.points)
    {
        if (point.phase != TouchPhase::Ended)
            continue;

        // Check if it qualifies as a tap
        float distance = point.GetTotalDistance();
        float duration = point.GetDuration();

        if (distance <= m_settings.tapMaxDistance && duration <= m_settings.tapMaxDuration)
        {
            // This is a tap candidate
            if (m_waitingForDoubleTap)
            {
                // Check if it's close enough to the previous tap
                float dx = point.startX - m_lastTap.startX;
                float dy = point.startY - m_lastTap.startY;
                float tapDistance = std::sqrt(dx * dx + dy * dy);

                if (tapDistance <= m_settings.tapMaxDistance * 2.0f)
                {
                    // Double tap!
                    GestureEvent doubleTap;
                    doubleTap.type = GestureType::DoubleTap;
                    doubleTap.x = (point.startX + m_lastTap.startX) * 0.5f;
                    doubleTap.y = (point.startY + m_lastTap.startY) * 0.5f;
                    doubleTap.fingerCount = 1;
                    events.push_back(doubleTap);
                    InvokeCallback(doubleTap);

                    m_waitingForDoubleTap = false;
                    m_lastTap.valid = false;
                }
                else
                {
                    // Too far, trigger the previous tap and start new wait
                    GestureEvent tap;
                    tap.type = GestureType::Tap;
                    tap.x = m_lastTap.startX;
                    tap.y = m_lastTap.startY;
                    tap.fingerCount = 1;
                    events.push_back(tap);
                    InvokeCallback(tap);

                    // Set up for new double tap detection
                    m_lastTap.startX = point.startX;
                    m_lastTap.startY = point.startY;
                    m_lastTap.valid = true;
                    m_doubleTapTimer = 0.0f;
                }
            }
            else
            {
                // Start waiting for double tap
                m_lastTap.startX = point.startX;
                m_lastTap.startY = point.startY;
                m_lastTap.valid = true;
                m_waitingForDoubleTap = true;
                m_doubleTapTimer = 0.0f;
            }
        }
    }
}

void GestureRecognizer::DetectLongPress(const TouchState& touch, float deltaTime, std::vector<GestureEvent>& events)
{
    if (touch.activeCount != 1)
    {
        // Long press only with single touch
        m_longPressTimer = 0.0f;
        m_longPressTriggered = false;
        return;
    }

    const auto* point = touch.GetTouch(0);
    if (!point)
        return;

    // Check if touch is stationary
    float distance = point->GetTotalDistance();
    if (distance > m_settings.tapMaxDistance)
    {
        // Moved too much, reset
        m_longPressTimer = 0.0f;
        m_longPressTriggered = false;
        return;
    }

    // Track same touch
    if (m_longPressTouchId != 0 && m_longPressTouchId != point->id)
    {
        m_longPressTimer = 0.0f;
        m_longPressTriggered = false;
    }
    m_longPressTouchId = point->id;

    if (!m_longPressTriggered)
    {
        m_longPressTimer += deltaTime;

        if (m_longPressTimer >= m_settings.longPressMinDuration)
        {
            GestureEvent longPress;
            longPress.type = GestureType::LongPress;
            longPress.x = point->x;
            longPress.y = point->y;
            longPress.fingerCount = 1;
            events.push_back(longPress);
            InvokeCallback(longPress);

            m_longPressTriggered = true;

            // Cancel any pending tap
            m_waitingForDoubleTap = false;
            m_lastTap.valid = false;
        }
    }
}

void GestureRecognizer::DetectSwipe(const TouchState& touch, std::vector<GestureEvent>& events)
{
    // Look for touch that just ended with significant velocity
    for (const auto& point : touch.points)
    {
        if (point.phase != TouchPhase::Ended)
            continue;

        float dx = point.GetTotalDeltaX();
        float dy = point.GetTotalDeltaY();
        float distance = point.GetTotalDistance();
        float duration = point.GetDuration();

        if (distance < m_settings.swipeMinDistance || duration <= 0.0f)
            continue;

        float velocity = distance / duration;
        if (velocity >= m_settings.swipeMinVelocity)
        {
            GestureEvent swipe;
            swipe.type = GestureType::Swipe;
            swipe.x = point.startX;
            swipe.y = point.startY;
            swipe.swipeDirection = GetSwipeDirection(dx, dy);
            swipe.swipeVelocity = velocity;
            swipe.fingerCount = 1;
            events.push_back(swipe);
            InvokeCallback(swipe);

            // Cancel tap detection
            m_waitingForDoubleTap = false;
            m_lastTap.valid = false;
        }
    }
}

void GestureRecognizer::DetectPan(const TouchState& touch, std::vector<GestureEvent>& events)
{
    if (touch.activeCount != 1)
    {
        if (m_isPanning)
        {
            m_isPanning = false;
        }
        return;
    }

    const auto* point = touch.GetTouch(0);
    if (!point)
        return;

    if (point->phase == TouchPhase::Began)
    {
        m_panStartX = point->x;
        m_panStartY = point->y;
    }
    else if (point->phase == TouchPhase::Moved)
    {
        float totalDistance = point->GetTotalDistance();

        // Start panning if moved enough
        if (!m_isPanning && totalDistance > m_settings.tapMaxDistance)
        {
            m_isPanning = true;
        }

        if (m_isPanning)
        {
            GestureEvent pan;
            pan.type = GestureType::Pan;
            pan.x = point->x;
            pan.y = point->y;
            pan.panDeltaX = point->deltaX;
            pan.panDeltaY = point->deltaY;
            pan.fingerCount = 1;
            pan.inProgress = true;
            events.push_back(pan);
            InvokeCallback(pan);
        }
    }
    else if (point->phase == TouchPhase::Ended && m_isPanning)
    {
        GestureEvent pan;
        pan.type = GestureType::Pan;
        pan.x = point->x;
        pan.y = point->y;
        pan.panDeltaX = 0.0f;
        pan.panDeltaY = 0.0f;
        pan.fingerCount = 1;
        pan.inProgress = false;
        events.push_back(pan);
        InvokeCallback(pan);

        m_isPanning = false;
    }
}

void GestureRecognizer::DetectPinchAndRotate(const TouchState& touch, std::vector<GestureEvent>& events)
{
    if (touch.activeCount < 2)
    {
        if (m_pinch.active)
        {
            // End pinch/rotate
            m_pinch.active = false;
        }
        return;
    }

    const auto* p1 = touch.GetTouch(0);
    const auto* p2 = touch.GetTouch(1);
    if (!p1 || !p2)
        return;

    float currentDistance = Distance(p1->x, p1->y, p2->x, p2->y);
    float currentAngle = Angle(p1->x, p1->y, p2->x, p2->y);
    float centerX = (p1->x + p2->x) * 0.5f;
    float centerY = (p1->y + p2->y) * 0.5f;

    if (!m_pinch.active)
    {
        // Start pinch/rotate
        m_pinch.initialDistance = currentDistance;
        m_pinch.lastDistance = currentDistance;
        m_pinch.initialAngle = currentAngle;
        m_pinch.lastAngle = currentAngle;
        m_pinch.active = true;
    }
    else
    {
        // Calculate deltas
        float distanceDelta = currentDistance - m_pinch.lastDistance;
        float scale = (m_pinch.initialDistance > 0.0f) ? 
                      (currentDistance / m_pinch.initialDistance) : 1.0f;

        float angleDelta = currentAngle - m_pinch.lastAngle;
        // Normalize angle delta to -PI to PI
        while (angleDelta > 3.14159f) angleDelta -= 6.28318f;
        while (angleDelta < -3.14159f) angleDelta += 6.28318f;

        // Detect pinch
        if (std::abs(scale - 1.0f) >= m_settings.pinchMinScale ||
            std::abs(distanceDelta) > 5.0f)
        {
            GestureEvent pinch;
            pinch.type = GestureType::Pinch;
            pinch.x = centerX;
            pinch.y = centerY;
            pinch.pinchScale = scale;
            pinch.pinchDelta = distanceDelta;
            pinch.fingerCount = 2;
            pinch.inProgress = true;
            events.push_back(pinch);
            InvokeCallback(pinch);
        }

        // Detect rotation
        if (std::abs(angleDelta) >= m_settings.rotationMinAngle)
        {
            GestureEvent rotate;
            rotate.type = GestureType::Rotate;
            rotate.x = centerX;
            rotate.y = centerY;
            rotate.rotationAngle = currentAngle - m_pinch.initialAngle;
            rotate.rotationDelta = angleDelta;
            rotate.fingerCount = 2;
            rotate.inProgress = true;
            events.push_back(rotate);
            InvokeCallback(rotate);
        }

        m_pinch.lastDistance = currentDistance;
        m_pinch.lastAngle = currentAngle;
    }
}

float GestureRecognizer::Distance(float x1, float y1, float x2, float y2) const
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    return std::sqrt(dx * dx + dy * dy);
}

float GestureRecognizer::Angle(float x1, float y1, float x2, float y2) const
{
    return std::atan2(y2 - y1, x2 - x1);
}

SwipeDirection GestureRecognizer::GetSwipeDirection(float dx, float dy) const
{
    // Determine primary direction
    if (std::abs(dx) > std::abs(dy))
    {
        return dx > 0 ? SwipeDirection::Right : SwipeDirection::Left;
    }
    else
    {
        return dy > 0 ? SwipeDirection::Down : SwipeDirection::Up;
    }
}

void GestureRecognizer::InvokeCallback(const GestureEvent& event)
{
    switch (event.type)
    {
    case GestureType::Tap:
        if (m_onTap) m_onTap(event);
        break;
    case GestureType::DoubleTap:
        if (m_onDoubleTap) m_onDoubleTap(event);
        break;
    case GestureType::LongPress:
        if (m_onLongPress) m_onLongPress(event);
        break;
    case GestureType::Swipe:
        if (m_onSwipe) m_onSwipe(event);
        break;
    case GestureType::Pan:
        if (m_onPan) m_onPan(event);
        break;
    case GestureType::Pinch:
        if (m_onPinch) m_onPinch(event);
        break;
    case GestureType::Rotate:
        if (m_onRotate) m_onRotate(event);
        break;
    default:
        break;
    }
}

} // namespace RVX
