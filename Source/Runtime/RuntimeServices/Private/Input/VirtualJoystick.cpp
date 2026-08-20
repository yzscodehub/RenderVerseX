/**
 * @file VirtualJoystick.cpp
 * @brief VirtualJoystick implementation
 */

#include "Runtime/Input/VirtualJoystick.h"
#include <cmath>
#include <algorithm>

namespace RVX
{

VirtualJoystick::VirtualJoystick()
{
    m_currentCenterX = m_config.centerX;
    m_currentCenterY = m_config.centerY;
    m_knobX = m_currentCenterX;
    m_knobY = m_currentCenterY;
}

VirtualJoystick::VirtualJoystick(const VirtualJoystickConfig& config)
    : m_config(config)
{
    m_currentCenterX = m_config.centerX;
    m_currentCenterY = m_config.centerY;
    m_knobX = m_currentCenterX;
    m_knobY = m_currentCenterY;
}

void VirtualJoystick::SetConfig(const VirtualJoystickConfig& config)
{
    m_config = config;
    m_currentCenterX = m_config.centerX;
    m_currentCenterY = m_config.centerY;
    if (!m_isActive)
    {
        m_knobX = m_currentCenterX;
        m_knobY = m_currentCenterY;
    }
}

void VirtualJoystick::SetScreenSize(float width, float height)
{
    m_screenWidth = width;
    m_screenHeight = height;
}

void VirtualJoystick::Process(const TouchState& touch, float deltaTime)
{
    const TouchPoint* trackedPoint = nullptr;

    // Find our tracked touch
    if (m_trackedTouchId != 0)
    {
        trackedPoint = touch.GetTouchById(m_trackedTouchId);
        
        // Check if touch ended
        if (!trackedPoint || !trackedPoint->IsActive())
        {
            m_trackedTouchId = 0;
            m_isActive = false;
            trackedPoint = nullptr;
        }
    }

    // If not tracking, look for new touch in activation area
    if (!m_isActive && m_trackedTouchId == 0)
    {
        for (const auto& point : touch.points)
        {
            if (point.phase == TouchPhase::Began)
            {
                if (TryClaimTouch(point))
                {
                    trackedPoint = &point;
                    break;
                }
            }
        }
    }

    // Process the touch
    ProcessTouch(trackedPoint, deltaTime);

    // Update visual state
    UpdateVisual(deltaTime);
}

void VirtualJoystick::Reset()
{
    m_outputX = 0.0f;
    m_outputY = 0.0f;
    m_isActive = false;
    m_trackedTouchId = 0;
    m_knobX = m_currentCenterX;
    m_knobY = m_currentCenterY;
    m_opacity = 0.0f;
    m_fadeTimer = 0.0f;
}

float VirtualJoystick::GetMagnitude() const
{
    return std::sqrt(m_outputX * m_outputX + m_outputY * m_outputY);
}

float VirtualJoystick::GetAngle() const
{
    return std::atan2(m_outputY, m_outputX);
}

VirtualJoystickVisual VirtualJoystick::GetVisual() const
{
    VirtualJoystickVisual visual;
    visual.visible = m_opacity > 0.01f;
    visual.active = m_isActive;
    visual.opacity = m_opacity;
    visual.outerX = m_currentCenterX;
    visual.outerY = m_currentCenterY;
    visual.outerRadius = m_config.outerRadius;
    visual.innerX = m_knobX;
    visual.innerY = m_knobY;
    visual.innerRadius = m_config.innerRadius;
    return visual;
}

bool VirtualJoystick::TryClaimTouch(const TouchPoint& point)
{
    if (!IsInActivationArea(point.x, point.y))
        return false;

    m_trackedTouchId = point.id;
    m_isActive = true;

    // Set center based on mode
    if (m_config.mode == VirtualJoystickMode::Fixed)
    {
        m_currentCenterX = m_config.centerX;
        m_currentCenterY = m_config.centerY;
    }
    else
    {
        // Floating or Dynamic: center at touch position
        m_currentCenterX = point.x;
        m_currentCenterY = point.y;
    }

    m_knobX = m_currentCenterX;
    m_knobY = m_currentCenterY;

    return true;
}

bool VirtualJoystick::IsInActivationArea(float x, float y) const
{
    switch (m_config.mode)
    {
    case VirtualJoystickMode::Fixed:
    {
        // Check if within outer radius of fixed position
        float dx = x - m_config.centerX;
        float dy = y - m_config.centerY;
        return (dx * dx + dy * dy) <= (m_config.outerRadius * m_config.outerRadius);
    }

    case VirtualJoystickMode::Floating:
    case VirtualJoystickMode::Dynamic:
    {
        // Check if on the correct side of the screen
        float normalizedX = x / m_screenWidth;
        return normalizedX < m_config.screenSideThreshold;
    }

    default:
        return false;
    }
}

void VirtualJoystick::ProcessTouch(const TouchPoint* point, float deltaTime)
{
    (void)deltaTime;

    if (!point || !m_isActive)
    {
        // Return to center
        m_outputX = 0.0f;
        m_outputY = 0.0f;
        m_knobX = m_currentCenterX;
        m_knobY = m_currentCenterY;
        return;
    }

    // Calculate offset from center
    float dx = point->x - m_currentCenterX;
    float dy = point->y - m_currentCenterY;
    float distance = std::sqrt(dx * dx + dy * dy);

    // Dynamic mode: move center if finger goes beyond range
    if (m_config.mode == VirtualJoystickMode::Dynamic && distance > m_config.maxDistance)
    {
        float excess = distance - m_config.maxDistance;
        float dirX = dx / distance;
        float dirY = dy / distance;
        m_currentCenterX += dirX * excess;
        m_currentCenterY += dirY * excess;

        // Recalculate offset
        dx = point->x - m_currentCenterX;
        dy = point->y - m_currentCenterY;
        distance = m_config.maxDistance;
    }

    // Clamp distance to max
    if (distance > m_config.maxDistance)
    {
        float scale = m_config.maxDistance / distance;
        dx *= scale;
        dy *= scale;
        distance = m_config.maxDistance;
    }

    // Update knob position
    m_knobX = m_currentCenterX + dx;
    m_knobY = m_currentCenterY + dy;

    // Calculate normalized output
    if (m_config.maxDistance > 0.0f)
    {
        m_outputX = dx / m_config.maxDistance;
        m_outputY = dy / m_config.maxDistance;
    }
    else
    {
        m_outputX = 0.0f;
        m_outputY = 0.0f;
    }

    // Apply dead zone
    m_outputX = ApplyDeadZone(m_outputX);
    m_outputY = ApplyDeadZone(m_outputY);

    // Normalize to unit circle if requested
    if (m_config.normalizeOutput)
    {
        float mag = std::sqrt(m_outputX * m_outputX + m_outputY * m_outputY);
        if (mag > 1.0f)
        {
            m_outputX /= mag;
            m_outputY /= mag;
        }
    }

    // Invert Y for typical game coordinates (up = positive)
    m_outputY = -m_outputY;
}

void VirtualJoystick::UpdateVisual(float deltaTime)
{
    if (m_isActive)
    {
        // Fade in
        m_opacity = std::min(m_opacity + deltaTime * 4.0f, m_config.maxOpacity);
        m_fadeTimer = 0.0f;
    }
    else if (m_opacity > 0.0f)
    {
        // Fade out
        m_fadeTimer += deltaTime;
        if (m_fadeTimer >= m_config.fadeOutTime)
        {
            m_opacity = 0.0f;
        }
        else
        {
            float fadeProgress = m_fadeTimer / m_config.fadeOutTime;
            m_opacity = m_config.minOpacity * (1.0f - fadeProgress);
        }

        // Smoothly return knob to center
        float returnSpeed = 10.0f;
        m_knobX += (m_currentCenterX - m_knobX) * returnSpeed * deltaTime;
        m_knobY += (m_currentCenterY - m_knobY) * returnSpeed * deltaTime;
    }
}

float VirtualJoystick::ApplyDeadZone(float value) const
{
    float absValue = std::abs(value);
    if (absValue < m_config.deadZone)
        return 0.0f;

    // Remap to 0-1 range
    float sign = value > 0.0f ? 1.0f : -1.0f;
    return sign * (absValue - m_config.deadZone) / (1.0f - m_config.deadZone);
}

} // namespace RVX
