/**
 * @file AudioEvent.h
 * @brief Data-driven audio event system
 */

#pragma once

#include "Audio/AudioTypes.h"
#include "Audio/AudioClip.h"
#include "Audio/Mixer/AudioBus.h"
#include <string>
#include <vector>
#include <random>

namespace RVX::Audio
{

/**
 * @brief Clip selection mode for events with multiple clips
 */
enum class ClipSelectionMode : uint8
{
    Random,         ///< Random selection
    Sequential,     ///< Play in order
    Shuffle,        ///< Random but avoid repeats
    Weighted        ///< Random with weights
};

/**
 * @brief Audio event clip entry with weight
 */
struct AudioEventClip
{
    AudioClip::Ptr clip;
    float weight = 1.0f;
};

/**
 * @brief Audio event description
 * 
 * Defines how a sound should play, including:
 * - Multiple clip variations
 * - Random volume/pitch variation
 * - 3D spatial settings
 * - Playback limits and cooldowns
 */
struct AudioEventDesc
{
    std::string name;
    
    // Clips
    std::vector<AudioEventClip> clips;
    ClipSelectionMode selectionMode = ClipSelectionMode::Random;
    
    // Volume
    float volumeMin = 1.0f;
    float volumeMax = 1.0f;
    
    // Pitch
    float pitchMin = 1.0f;
    float pitchMax = 1.0f;
    
    // Playback control
    float cooldown = 0.0f;          ///< Minimum time between plays
    int maxInstances = -1;          ///< Max concurrent instances (-1 = unlimited)
    bool loop = false;
    
    // Routing
    uint32 targetBus = BusId::SFX;
    
    // 3D settings (optional)
    bool is3D = false;
    Audio3DSettings spatial;
    
    // Priority
    uint8 priority = 128;
    
    // Fade
    float fadeInTime = 0.0f;
    float fadeOutTime = 0.0f;
};

/**
 * @brief Runtime audio event instance
 * 
 * Created when an AudioEventDesc is triggered.
 * Handles clip selection, variation, and instance tracking.
 */
class AudioEvent
{
public:
    using Ptr = std::shared_ptr<AudioEvent>;

    AudioEvent(const AudioEventDesc& desc);
    ~AudioEvent() = default;

    // =========================================================================
    // Properties
    // =========================================================================

    const std::string& GetName() const { return m_desc.name; }
    const AudioEventDesc& GetDesc() const { return m_desc; }

    // =========================================================================
    // Playback
    // =========================================================================

    /**
     * @brief Check if the event can play (cooldown, instance limits)
     */
    bool CanPlay() const;

    /**
     * @brief Get the next clip to play
     */
    AudioClip::Ptr GetNextClip();

    /**
     * @brief Generate randomized play settings
     */
    AudioPlaySettings GenerateSettings() const;

    /**
     * @brief Record that the event was played
     */
    void RecordPlay(AudioHandle handle);

    /**
     * @brief Remove a finished instance
     */
    void RemoveInstance(AudioHandle handle);

    /**
     * @brief Get active instance count
     */
    int GetActiveInstanceCount() const { return static_cast<int>(m_activeHandles.size()); }

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update cooldown timer
     */
    void Update(float deltaTime);

private:
    AudioEventDesc m_desc;
    
    // Instance tracking
    std::vector<AudioHandle> m_activeHandles;
    float m_cooldownRemaining = 0.0f;
    
    // Clip selection state
    int m_sequentialIndex = 0;
    std::vector<int> m_shuffleOrder;
    int m_shuffleIndex = 0;
    
    // Random generator
    mutable std::mt19937 m_rng;

    void InitShuffleOrder();
};

} // namespace RVX::Audio
