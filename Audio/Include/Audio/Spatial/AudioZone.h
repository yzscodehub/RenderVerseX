/**
 * @file AudioZone.h
 * @brief Audio zones for spatial audio effects
 */

#pragma once

#include "Audio/AudioClip.h"
#include "Audio/AudioTypes.h"
#include "Audio/DSP/IAudioEffect.h"
#include <memory>
#include <string>
#include <vector>

namespace RVX::Audio
{

/**
 * @brief Zone shape type
 */
enum class ZoneShape : uint8
{
    Box,
    Sphere,
    Capsule
};

/**
 * @brief Reverb zone preset
 */
enum class ReverbZonePreset : uint8
{
    None,
    SmallRoom,
    MediumRoom,
    LargeRoom,
    Hall,
    Cathedral,
    Cave,
    Forest,
    Underwater,
    Custom
};

/**
 * @brief Audio zone for spatial audio effects
 * 
 * Audio zones define regions where specific audio effects
 * or ambient sounds are applied. When the listener enters
 * a zone, its effects are smoothly blended in.
 * 
 * Features:
 * - Multiple shape types (box, sphere, capsule)
 * - Reverb presets or custom effects
 * - Ambient sound loops
 * - Blend weights for smooth transitions
 * - Priority for overlapping zones
 */
class AudioZone
{
public:
    using Ptr = std::shared_ptr<AudioZone>;

    AudioZone(const std::string& name);
    ~AudioZone() = default;

    // =========================================================================
    // Properties
    // =========================================================================

    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    void SetPriority(int priority) { m_priority = priority; }
    int GetPriority() const { return m_priority; }

    // =========================================================================
    // Shape
    // =========================================================================

    void SetShape(ZoneShape shape) { m_shape = shape; }
    ZoneShape GetShape() const { return m_shape; }

    // Box shape
    void SetBoxExtents(const Vec3& center, const Vec3& halfExtents);
    const Vec3& GetBoxCenter() const { return m_center; }
    const Vec3& GetBoxHalfExtents() const { return m_halfExtents; }

    // Sphere shape
    void SetSphereRadius(const Vec3& center, float radius);
    float GetSphereRadius() const { return m_radius; }

    // Capsule shape
    void SetCapsule(const Vec3& start, const Vec3& end, float radius);
    const Vec3& GetCapsuleStart() const { return m_capsuleStart; }
    const Vec3& GetCapsuleEnd() const { return m_capsuleEnd; }

    // =========================================================================
    // Fade/Blend
    // =========================================================================

    /**
     * @brief Set the fade distance at the edge of the zone
     */
    void SetFadeDistance(float distance) { m_fadeDistance = distance; }
    float GetFadeDistance() const { return m_fadeDistance; }

    /**
     * @brief Calculate blend weight for a position
     * @return 0 = outside, 1 = fully inside
     */
    float CalculateBlendWeight(const Vec3& position) const;

    /**
     * @brief Check if a position is inside the zone
     */
    bool ContainsPoint(const Vec3& position) const;

    // =========================================================================
    // Reverb
    // =========================================================================

    void SetReverbPreset(ReverbZonePreset preset);
    ReverbZonePreset GetReverbPreset() const { return m_reverbPreset; }

    void SetCustomReverb(const ReverbSettings& settings);
    const ReverbSettings& GetReverbSettings() const { return m_reverbSettings; }

    // =========================================================================
    // Ambient Sound
    // =========================================================================

    void SetAmbientClip(AudioClip::Ptr clip) { m_ambientClip = std::move(clip); }
    AudioClip::Ptr GetAmbientClip() const { return m_ambientClip; }

    void SetAmbientVolume(float volume) { m_ambientVolume = volume; }
    float GetAmbientVolume() const { return m_ambientVolume; }

    // =========================================================================
    // Low-pass (for occlusion/muffling)
    // =========================================================================

    void SetLowPassEnabled(bool enabled) { m_lowPassEnabled = enabled; }
    bool IsLowPassEnabled() const { return m_lowPassEnabled; }

    void SetLowPassCutoff(float cutoff) { m_lowPassCutoff = cutoff; }
    float GetLowPassCutoff() const { return m_lowPassCutoff; }

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr CreateBox(const std::string& name, const Vec3& center, const Vec3& halfExtents);
    static Ptr CreateSphere(const std::string& name, const Vec3& center, float radius);

private:
    std::string m_name;
    bool m_enabled = true;
    int m_priority = 0;

    // Shape
    ZoneShape m_shape = ZoneShape::Box;
    Vec3 m_center{0.0f};
    Vec3 m_halfExtents{1.0f};
    float m_radius = 1.0f;
    Vec3 m_capsuleStart{0.0f};
    Vec3 m_capsuleEnd{0.0f, 1.0f, 0.0f};

    float m_fadeDistance = 1.0f;

    // Effects
    ReverbZonePreset m_reverbPreset = ReverbZonePreset::None;
    ReverbSettings m_reverbSettings;
    
    bool m_lowPassEnabled = false;
    float m_lowPassCutoff = 5000.0f;

    // Ambient
    AudioClip::Ptr m_ambientClip;
    float m_ambientVolume = 1.0f;

    float CalculateDistanceToSurface(const Vec3& position) const;
};

} // namespace RVX::Audio
