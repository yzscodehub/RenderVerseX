/**
 * @file AudioZone.cpp
 * @brief AudioZone implementation
 */

#include "Audio/Spatial/AudioZone.h"
#include <cmath>
#include <algorithm>

namespace RVX::Audio
{

AudioZone::AudioZone(const std::string& name)
    : m_name(name)
{
}

void AudioZone::SetBoxExtents(const Vec3& center, const Vec3& halfExtents)
{
    m_shape = ZoneShape::Box;
    m_center = center;
    m_halfExtents = halfExtents;
}

void AudioZone::SetSphereRadius(const Vec3& center, float radius)
{
    m_shape = ZoneShape::Sphere;
    m_center = center;
    m_radius = radius;
}

void AudioZone::SetCapsule(const Vec3& start, const Vec3& end, float radius)
{
    m_shape = ZoneShape::Capsule;
    m_capsuleStart = start;
    m_capsuleEnd = end;
    m_radius = radius;
    m_center = Vec3(
        (start.x + end.x) * 0.5f,
        (start.y + end.y) * 0.5f,
        (start.z + end.z) * 0.5f
    );
}

float AudioZone::CalculateBlendWeight(const Vec3& position) const
{
    if (!m_enabled)
    {
        return 0.0f;
    }

    float distance = CalculateDistanceToSurface(position);

    if (distance <= 0.0f)
    {
        // Inside the zone
        return 1.0f;
    }
    else if (distance >= m_fadeDistance)
    {
        // Outside fade region
        return 0.0f;
    }
    else
    {
        // In fade region
        return 1.0f - (distance / m_fadeDistance);
    }
}

bool AudioZone::ContainsPoint(const Vec3& position) const
{
    return CalculateDistanceToSurface(position) <= 0.0f;
}

void AudioZone::SetReverbPreset(ReverbZonePreset preset)
{
    m_reverbPreset = preset;

    switch (preset)
    {
        case ReverbZonePreset::None:
            m_reverbSettings = ReverbSettings{};
            m_reverbSettings.wetLevel = 0.0f;
            break;

        case ReverbZonePreset::SmallRoom:
            m_reverbSettings.roomSize = 0.3f;
            m_reverbSettings.damping = 0.7f;
            m_reverbSettings.wetLevel = 0.2f;
            m_reverbSettings.dryLevel = 0.8f;
            m_reverbSettings.width = 0.8f;
            break;

        case ReverbZonePreset::MediumRoom:
            m_reverbSettings.roomSize = 0.5f;
            m_reverbSettings.damping = 0.5f;
            m_reverbSettings.wetLevel = 0.3f;
            m_reverbSettings.dryLevel = 0.7f;
            m_reverbSettings.width = 1.0f;
            break;

        case ReverbZonePreset::LargeRoom:
            m_reverbSettings.roomSize = 0.7f;
            m_reverbSettings.damping = 0.4f;
            m_reverbSettings.wetLevel = 0.35f;
            m_reverbSettings.dryLevel = 0.65f;
            m_reverbSettings.width = 1.0f;
            break;

        case ReverbZonePreset::Hall:
            m_reverbSettings.roomSize = 0.8f;
            m_reverbSettings.damping = 0.3f;
            m_reverbSettings.wetLevel = 0.4f;
            m_reverbSettings.dryLevel = 0.6f;
            m_reverbSettings.width = 1.0f;
            break;

        case ReverbZonePreset::Cathedral:
            m_reverbSettings.roomSize = 0.95f;
            m_reverbSettings.damping = 0.2f;
            m_reverbSettings.wetLevel = 0.5f;
            m_reverbSettings.dryLevel = 0.5f;
            m_reverbSettings.width = 1.0f;
            break;

        case ReverbZonePreset::Cave:
            m_reverbSettings.roomSize = 0.9f;
            m_reverbSettings.damping = 0.6f;
            m_reverbSettings.wetLevel = 0.6f;
            m_reverbSettings.dryLevel = 0.4f;
            m_reverbSettings.width = 0.9f;
            break;

        case ReverbZonePreset::Forest:
            m_reverbSettings.roomSize = 0.4f;
            m_reverbSettings.damping = 0.8f;
            m_reverbSettings.wetLevel = 0.15f;
            m_reverbSettings.dryLevel = 0.85f;
            m_reverbSettings.width = 1.0f;
            break;

        case ReverbZonePreset::Underwater:
            m_reverbSettings.roomSize = 0.6f;
            m_reverbSettings.damping = 0.3f;
            m_reverbSettings.wetLevel = 0.7f;
            m_reverbSettings.dryLevel = 0.3f;
            m_reverbSettings.width = 0.5f;
            m_lowPassEnabled = true;
            m_lowPassCutoff = 800.0f;
            break;

        case ReverbZonePreset::Custom:
        default:
            break;
    }
}

void AudioZone::SetCustomReverb(const ReverbSettings& settings)
{
    m_reverbPreset = ReverbZonePreset::Custom;
    m_reverbSettings = settings;
}

AudioZone::Ptr AudioZone::CreateBox(const std::string& name, const Vec3& center, const Vec3& halfExtents)
{
    auto zone = std::make_shared<AudioZone>(name);
    zone->SetBoxExtents(center, halfExtents);
    return zone;
}

AudioZone::Ptr AudioZone::CreateSphere(const std::string& name, const Vec3& center, float radius)
{
    auto zone = std::make_shared<AudioZone>(name);
    zone->SetSphereRadius(center, radius);
    return zone;
}

float AudioZone::CalculateDistanceToSurface(const Vec3& position) const
{
    switch (m_shape)
    {
        case ZoneShape::Box:
        {
            // Distance to box surface (negative = inside)
            Vec3 d = Vec3(
                std::abs(position.x - m_center.x) - m_halfExtents.x,
                std::abs(position.y - m_center.y) - m_halfExtents.y,
                std::abs(position.z - m_center.z) - m_halfExtents.z
            );

            Vec3 dClamped = Vec3(
                std::max(d.x, 0.0f),
                std::max(d.y, 0.0f),
                std::max(d.z, 0.0f)
            );

            float outsideDistance = std::sqrt(
                dClamped.x * dClamped.x + 
                dClamped.y * dClamped.y + 
                dClamped.z * dClamped.z
            );

            float insideDistance = std::min(std::max(d.x, std::max(d.y, d.z)), 0.0f);

            return outsideDistance + insideDistance;
        }

        case ZoneShape::Sphere:
        {
            Vec3 diff = Vec3(
                position.x - m_center.x,
                position.y - m_center.y,
                position.z - m_center.z
            );
            float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
            return distance - m_radius;
        }

        case ZoneShape::Capsule:
        {
            // Distance to capsule = distance to line segment - radius
            Vec3 ab = Vec3(
                m_capsuleEnd.x - m_capsuleStart.x,
                m_capsuleEnd.y - m_capsuleStart.y,
                m_capsuleEnd.z - m_capsuleStart.z
            );
            Vec3 ap = Vec3(
                position.x - m_capsuleStart.x,
                position.y - m_capsuleStart.y,
                position.z - m_capsuleStart.z
            );

            float abLenSq = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
            float t = 0.0f;
            if (abLenSq > 0.0f)
            {
                t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / abLenSq;
                t = std::clamp(t, 0.0f, 1.0f);
            }

            Vec3 closest = Vec3(
                m_capsuleStart.x + t * ab.x,
                m_capsuleStart.y + t * ab.y,
                m_capsuleStart.z + t * ab.z
            );

            Vec3 diff = Vec3(
                position.x - closest.x,
                position.y - closest.y,
                position.z - closest.z
            );

            float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
            return distance - m_radius;
        }
    }

    return 0.0f;
}

} // namespace RVX::Audio
