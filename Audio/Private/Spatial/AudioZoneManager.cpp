/**
 * @file AudioZoneManager.cpp
 * @brief AudioZoneManager implementation
 */

#include "Audio/Spatial/AudioZoneManager.h"
#include "Audio/AudioEngine.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX::Audio
{

AudioZoneManager::AudioZoneManager(AudioEngine* engine)
    : m_engine(engine)
{
    // Default to null occlusion
    m_occlusionProvider = std::make_shared<NullOcclusionProvider>();
}

void AudioZoneManager::AddZone(AudioZone::Ptr zone)
{
    if (!zone)
    {
        return;
    }

    m_zones.push_back(zone);
    RVX_CORE_DEBUG("Added audio zone: {}", zone->GetName());
}

void AudioZoneManager::RemoveZone(const std::string& name)
{
    // Stop any ambient sound
    auto handleIt = m_ambientHandles.find(name);
    if (handleIt != m_ambientHandles.end())
    {
        if (m_engine && handleIt->second.IsValid())
        {
            m_engine->Stop(handleIt->second, 0.5f);
        }
        m_ambientHandles.erase(handleIt);
    }

    // Remove from active zones
    m_activeZones.erase(
        std::remove_if(m_activeZones.begin(), m_activeZones.end(),
            [&name](const ActiveZone& az) { return az.zone->GetName() == name; }),
        m_activeZones.end()
    );

    // Remove zone
    m_zones.erase(
        std::remove_if(m_zones.begin(), m_zones.end(),
            [&name](const AudioZone::Ptr& z) { return z->GetName() == name; }),
        m_zones.end()
    );

    RVX_CORE_DEBUG("Removed audio zone: {}", name);
}

AudioZone* AudioZoneManager::GetZone(const std::string& name)
{
    for (auto& zone : m_zones)
    {
        if (zone->GetName() == name)
        {
            return zone.get();
        }
    }
    return nullptr;
}

void AudioZoneManager::ClearZones()
{
    // Stop all ambient sounds
    for (auto& pair : m_ambientHandles)
    {
        if (m_engine && pair.second.IsValid())
        {
            m_engine->Stop(pair.second);
        }
    }
    m_ambientHandles.clear();

    m_activeZones.clear();
    m_zones.clear();
}

void AudioZoneManager::SetOcclusionProvider(std::shared_ptr<IOcclusionProvider> provider)
{
    m_occlusionProvider = provider ? provider : std::make_shared<NullOcclusionProvider>();
}

OcclusionResult AudioZoneManager::GetOcclusion(const Vec3& sourcePosition)
{
    if (m_occlusionProvider && m_occlusionProvider->IsEnabled())
    {
        return m_occlusionProvider->CalculateOcclusion(sourcePosition, m_listenerPosition);
    }
    return OcclusionResult{};
}

void AudioZoneManager::Update(const Vec3& listenerPosition, float deltaTime)
{
    m_listenerPosition = listenerPosition;

    // Update active zones based on listener position
    UpdateActiveZones(listenerPosition);

    // Update zone blending
    UpdateZoneBlending(deltaTime);

    // Update ambient sounds
    UpdateAmbientSounds();

    // Update effects
    if (m_effectsEnabled)
    {
        UpdateEffects();
    }

    // Update occlusion provider
    if (m_occlusionProvider)
    {
        m_occlusionProvider->Update(deltaTime);
    }
}

AudioZone* AudioZoneManager::GetPrimaryZone()
{
    if (m_activeZones.empty())
    {
        return nullptr;
    }
    return m_activeZones[0].zone.get();
}

ReverbSettings AudioZoneManager::GetBlendedReverbSettings() const
{
    ReverbSettings result;
    result.wetLevel = 0.0f;
    result.dryLevel = 1.0f;

    float totalWeight = 0.0f;

    for (const auto& az : m_activeZones)
    {
        if (az.blendWeight > 0.0f && az.zone->GetReverbPreset() != ReverbZonePreset::None)
        {
            const auto& settings = az.zone->GetReverbSettings();
            float w = az.blendWeight;

            result.roomSize += settings.roomSize * w;
            result.damping += settings.damping * w;
            result.wetLevel += settings.wetLevel * w;
            result.dryLevel += settings.dryLevel * w;
            result.width += settings.width * w;

            totalWeight += w;
        }
    }

    if (totalWeight > 0.0f)
    {
        result.roomSize /= totalWeight;
        result.damping /= totalWeight;
        result.wetLevel /= totalWeight;
        result.dryLevel /= totalWeight;
        result.width /= totalWeight;
    }

    return result;
}

void AudioZoneManager::UpdateActiveZones(const Vec3& listenerPosition)
{
    // Calculate blend weights for all zones
    std::vector<std::pair<AudioZone::Ptr, float>> zoneWeights;

    for (auto& zone : m_zones)
    {
        if (!zone->IsEnabled())
        {
            continue;
        }

        float weight = zone->CalculateBlendWeight(listenerPosition);
        if (weight > 0.0f)
        {
            zoneWeights.emplace_back(zone, weight);
        }
    }

    // Sort by priority (descending)
    std::sort(zoneWeights.begin(), zoneWeights.end(),
        [](const auto& a, const auto& b) {
            return a.first->GetPriority() > b.first->GetPriority();
        });

    // Update active zones list
    // Keep existing active zones, remove ones that are no longer active
    for (auto it = m_activeZones.begin(); it != m_activeZones.end();)
    {
        bool stillActive = false;
        for (const auto& zw : zoneWeights)
        {
            if (zw.first == it->zone)
            {
                stillActive = true;
                break;
            }
        }

        if (!stillActive)
        {
            // Zone is no longer active, remove after blend out
            if (it->blendWeight <= 0.01f)
            {
                it = m_activeZones.erase(it);
            }
            else
            {
                ++it;
            }
        }
        else
        {
            ++it;
        }
    }

    // Add new active zones
    for (const auto& zw : zoneWeights)
    {
        bool found = false;
        for (auto& az : m_activeZones)
        {
            if (az.zone == zw.first)
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            ActiveZone newZone;
            newZone.zone = zw.first;
            newZone.blendWeight = 0.0f;  // Will blend in
            m_activeZones.push_back(newZone);
        }
    }
}

void AudioZoneManager::UpdateZoneBlending(float deltaTime)
{
    for (auto& az : m_activeZones)
    {
        float targetWeight = az.zone->CalculateBlendWeight(m_listenerPosition);
        float diff = targetWeight - az.blendWeight;

        float maxChange = m_blendSpeed * deltaTime;

        if (std::abs(diff) < maxChange)
        {
            az.blendWeight = targetWeight;
        }
        else if (diff > 0)
        {
            az.blendWeight += maxChange;
        }
        else
        {
            az.blendWeight -= maxChange;
        }

        az.blendWeight = std::clamp(az.blendWeight, 0.0f, 1.0f);
    }
}

void AudioZoneManager::UpdateAmbientSounds()
{
    if (!m_engine)
    {
        return;
    }

    for (auto& az : m_activeZones)
    {
        auto ambientClip = az.zone->GetAmbientClip();
        if (!ambientClip)
        {
            continue;
        }

        const std::string& zoneName = az.zone->GetName();
        auto handleIt = m_ambientHandles.find(zoneName);

        if (az.blendWeight > 0.01f)
        {
            // Should be playing
            if (handleIt == m_ambientHandles.end() || !m_engine->IsPlaying(handleIt->second))
            {
                // Start playing
                AudioPlaySettings settings;
                settings.volume = az.blendWeight * az.zone->GetAmbientVolume();
                settings.loop = true;

                auto handle = m_engine->Play(ambientClip, settings);
                m_ambientHandles[zoneName] = handle;
                az.ambientHandle = handle;
            }
            else
            {
                // Update volume
                float volume = az.blendWeight * az.zone->GetAmbientVolume();
                m_engine->SetVolume(handleIt->second, volume);
            }
        }
        else
        {
            // Should stop
            if (handleIt != m_ambientHandles.end() && handleIt->second.IsValid())
            {
                m_engine->Stop(handleIt->second, 0.5f);
                m_ambientHandles.erase(handleIt);
            }
        }
    }
}

void AudioZoneManager::UpdateEffects()
{
    // Update reverb effect with blended settings
    ReverbSettings blended = GetBlendedReverbSettings();

    m_reverbEffect.SetParameter("roomSize", blended.roomSize);
    m_reverbEffect.SetParameter("damping", blended.damping);
    m_reverbEffect.SetParameter("wetLevel", blended.wetLevel);
    m_reverbEffect.SetParameter("dryLevel", blended.dryLevel);
    m_reverbEffect.SetParameter("width", blended.width);

    // Update low-pass from active zones
    float minCutoff = 20000.0f;
    for (const auto& az : m_activeZones)
    {
        if (az.zone->IsLowPassEnabled() && az.blendWeight > 0.0f)
        {
            float zoneCutoff = az.zone->GetLowPassCutoff();
            float blendedCutoff = 20000.0f - (20000.0f - zoneCutoff) * az.blendWeight;
            minCutoff = std::min(minCutoff, blendedCutoff);
        }
    }

    m_lowPassEffect.SetParameter("cutoff", minCutoff);
    m_lowPassEffect.SetEnabled(minCutoff < 20000.0f);
}

} // namespace RVX::Audio
