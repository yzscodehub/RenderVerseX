/**
 * @file AudioMixer.h
 * @brief Audio mixer with bus hierarchy
 */

#pragma once

#include "Audio/AudioTypes.h"
#include "Audio/Mixer/AudioBus.h"
#include "Audio/Mixer/VoicePool.h"
#include <memory>
#include <unordered_map>
#include <string>

namespace RVX::Audio
{

/**
 * @brief Audio mixer configuration
 */
struct AudioMixerConfig
{
    uint32 sampleRate = 48000;
    uint32 channels = 2;
    uint32 bufferSize = 256;
    bool enableEffects = true;
};

/**
 * @brief Audio mixer manages buses and voice mixing
 * 
 * The mixer handles:
 * - Bus hierarchy management
 * - Volume and pan control per bus
 * - Effect chain processing
 * - Voice pool management
 * 
 * Default bus structure:
 * Master (0)
 * ├── Music (1)
 * ├── SFX (2)
 * ├── Voice (3)
 * ├── Ambient (4)
 * └── UI (5)
 */
class AudioMixer
{
public:
    AudioMixer() = default;
    ~AudioMixer() = default;

    // =========================================================================
    // Initialization
    // =========================================================================

    /**
     * @brief Initialize the mixer
     */
    bool Initialize(const AudioMixerConfig& config = {});

    /**
     * @brief Shutdown the mixer
     */
    void Shutdown();

    // =========================================================================
    // Bus Management
    // =========================================================================

    /**
     * @brief Create a new bus
     * @param name Bus name
     * @param parentId Parent bus ID (default: Master)
     * @return New bus ID
     */
    uint32 CreateBus(const std::string& name, uint32 parentId = BusId::Master);

    /**
     * @brief Get a bus by ID
     */
    AudioBusNode* GetBus(uint32 id);
    const AudioBusNode* GetBus(uint32 id) const;

    /**
     * @brief Get a bus by name
     */
    AudioBusNode* GetBus(const std::string& name);

    /**
     * @brief Get the master bus
     */
    AudioBusNode* GetMasterBus() { return m_masterBus.get(); }

    // =========================================================================
    // Volume Control (Convenience)
    // =========================================================================

    void SetMasterVolume(float volume);
    float GetMasterVolume() const;

    void SetMusicVolume(float volume);
    float GetMusicVolume() const;

    void SetSFXVolume(float volume);
    float GetSFXVolume() const;

    void SetVoiceVolume(float volume);
    float GetVoiceVolume() const;

    void SetAmbientVolume(float volume);
    float GetAmbientVolume() const;

    void SetUIVolume(float volume);
    float GetUIVolume() const;

    // =========================================================================
    // Voice Pool
    // =========================================================================

    /**
     * @brief Get the voice pool
     */
    VoicePool& GetVoicePool() { return m_voicePool; }
    const VoicePool& GetVoicePool() const { return m_voicePool; }

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update the mixer (call each frame)
     */
    void Update(float deltaTime, const Vec3& listenerPosition);

    // =========================================================================
    // Snapshots
    // =========================================================================

    /**
     * @brief Save current mixer state as a snapshot
     */
    void SaveSnapshot(const std::string& name);

    /**
     * @brief Blend to a saved snapshot
     */
    void BlendToSnapshot(const std::string& name, float duration);

    // =========================================================================
    // Ducking
    // =========================================================================

    /**
     * @brief Enable ducking (reduce volume) for a bus when another plays
     * @param sourceBus Bus that triggers ducking
     * @param targetBus Bus that gets ducked
     * @param duckAmount Volume multiplier when ducked (0-1)
     * @param attackTime Time to duck
     * @param releaseTime Time to unduck
     */
    void SetDucking(uint32 sourceBus, uint32 targetBus, 
                    float duckAmount, float attackTime, float releaseTime);

private:
    AudioMixerConfig m_config;
    bool m_initialized = false;

    // Bus hierarchy
    AudioBusNode::Ptr m_masterBus;
    std::unordered_map<uint32, AudioBusNode::Ptr> m_buses;
    std::unordered_map<std::string, uint32> m_busNameToId;
    uint32 m_nextBusId = 6;  // After default buses

    // Voice pool
    VoicePool m_voicePool;

    // Snapshots
    struct MixerSnapshot
    {
        std::unordered_map<uint32, float> busVolumes;
        std::unordered_map<uint32, bool> busMuted;
    };
    std::unordered_map<std::string, MixerSnapshot> m_snapshots;

    // Ducking state
    struct DuckingRule
    {
        uint32 sourceBus;
        uint32 targetBus;
        float duckAmount;
        float attackTime;
        float releaseTime;
        float currentDuck = 1.0f;
    };
    std::vector<DuckingRule> m_duckingRules;

    void CreateDefaultBuses();
    void UpdateDucking(float deltaTime);
};

} // namespace RVX::Audio
