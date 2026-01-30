/**
 * @file AudioListener.h
 * @brief Audio listener for 3D audio
 */

#pragma once

#include "Audio/AudioTypes.h"

namespace RVX::Audio
{

/**
 * @brief Audio listener represents the "ears" in 3D audio space
 */
class AudioListener
{
public:
    AudioListener() = default;
    ~AudioListener() = default;

    // =========================================================================
    // Transform
    // =========================================================================

    void SetPosition(const Vec3& position) { m_position = position; }
    const Vec3& GetPosition() const { return m_position; }

    void SetForward(const Vec3& forward) { m_forward = forward; }
    const Vec3& GetForward() const { return m_forward; }

    void SetUp(const Vec3& up) { m_up = up; }
    const Vec3& GetUp() const { return m_up; }

    void SetVelocity(const Vec3& velocity) { m_velocity = velocity; }
    const Vec3& GetVelocity() const { return m_velocity; }

    // =========================================================================
    // Settings
    // =========================================================================

    void SetVolume(float volume) { m_volume = volume; }
    float GetVolume() const { return m_volume; }

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

private:
    Vec3 m_position{0.0f};
    Vec3 m_forward{0.0f, 0.0f, -1.0f};
    Vec3 m_up{0.0f, 1.0f, 0.0f};
    Vec3 m_velocity{0.0f};
    float m_volume = 1.0f;
    bool m_enabled = true;
};

} // namespace RVX::Audio
