/**
 * @file AudioZoneManager.h
 * @brief Manager for audio zones
 */

#pragma once

#include "Audio/Spatial/AudioZone.h"
#include "Audio/Spatial/IOcclusionProvider.h"
#include "Audio/DSP/ReverbEffect.h"
#include "Audio/DSP/LowPassEffect.h"
#include <vector>
#include <memory>
#include <unordered_map>

namespace RVX::Audio
{

class AudioEngine;

/**
 * @brief Active zone info
 */
struct ActiveZone
{
    AudioZone::Ptr zone;
    float blendWeight = 0.0f;
    AudioHandle ambientHandle;
};

/**
 * @brief Manager for audio zones
 * 
 * Tracks the listener's position and manages active audio zones,
 * applying their effects and playing ambient sounds.
 * 
 * Usage:
 * @code
 * AudioZoneManager manager(&engine);
 * 
 * // Add zones
 * auto cave = AudioZone::CreateSphere("Cave", Vec3(0, 0, 0), 10.0f);
 * cave->SetReverbPreset(ReverbZonePreset::Cave);
 * manager.AddZone(cave);
 * 
 * // Update each frame
 * manager.Update(listenerPosition, deltaTime);
 * @endcode
 */
class AudioZoneManager
{
public:
    AudioZoneManager(AudioEngine* engine);
    ~AudioZoneManager() = default;

    // =========================================================================
    // Zone Management
    // =========================================================================

    /**
     * @brief Add a zone
     */
    void AddZone(AudioZone::Ptr zone);

    /**
     * @brief Remove a zone by name
     */
    void RemoveZone(const std::string& name);

    /**
     * @brief Get a zone by name
     */
    AudioZone* GetZone(const std::string& name);

    /**
     * @brief Clear all zones
     */
    void ClearZones();

    /**
     * @brief Get all zones
     */
    const std::vector<AudioZone::Ptr>& GetZones() const { return m_zones; }

    // =========================================================================
    // Occlusion
    // =========================================================================

    /**
     * @brief Set the occlusion provider
     */
    void SetOcclusionProvider(std::shared_ptr<IOcclusionProvider> provider);

    /**
     * @brief Get the occlusion provider
     */
    IOcclusionProvider* GetOcclusionProvider() { return m_occlusionProvider.get(); }

    /**
     * @brief Calculate occlusion for a sound source
     */
    OcclusionResult GetOcclusion(const Vec3& sourcePosition);

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update zones based on listener position
     */
    void Update(const Vec3& listenerPosition, float deltaTime);

    // =========================================================================
    // Active Zones
    // =========================================================================

    /**
     * @brief Get currently active zones (sorted by priority)
     */
    const std::vector<ActiveZone>& GetActiveZones() const { return m_activeZones; }

    /**
     * @brief Get the primary (highest priority) active zone
     */
    AudioZone* GetPrimaryZone();

    /**
     * @brief Get blended reverb settings from all active zones
     */
    ReverbSettings GetBlendedReverbSettings() const;

    // =========================================================================
    // Effects
    // =========================================================================

    /**
     * @brief Get the zone reverb effect (to attach to mixer bus)
     */
    ReverbEffect* GetReverbEffect() { return &m_reverbEffect; }

    /**
     * @brief Get the zone low-pass effect
     */
    LowPassEffect* GetLowPassEffect() { return &m_lowPassEffect; }

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set blend speed for zone transitions
     */
    void SetBlendSpeed(float speed) { m_blendSpeed = speed; }

    /**
     * @brief Enable/disable zone effects
     */
    void SetEffectsEnabled(bool enabled) { m_effectsEnabled = enabled; }
    bool AreEffectsEnabled() const { return m_effectsEnabled; }

private:
    AudioEngine* m_engine;
    
    std::vector<AudioZone::Ptr> m_zones;
    std::vector<ActiveZone> m_activeZones;
    
    std::shared_ptr<IOcclusionProvider> m_occlusionProvider;
    
    // Effects
    ReverbEffect m_reverbEffect;
    LowPassEffect m_lowPassEffect;
    bool m_effectsEnabled = true;
    
    // Listener tracking
    Vec3 m_listenerPosition{0.0f};
    
    // Blending
    float m_blendSpeed = 2.0f;  // Units per second
    
    // Ambient sound handles
    std::unordered_map<std::string, AudioHandle> m_ambientHandles;

    void UpdateActiveZones(const Vec3& listenerPosition);
    void UpdateZoneBlending(float deltaTime);
    void UpdateAmbientSounds();
    void UpdateEffects();
};

} // namespace RVX::Audio
