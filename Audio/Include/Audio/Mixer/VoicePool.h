/**
 * @file VoicePool.h
 * @brief Voice pool for managing concurrent audio playback
 */

#pragma once

#include "Audio/AudioTypes.h"
#include "Audio/AudioClip.h"
#include <memory>
#include <vector>
#include <functional>

namespace RVX::Audio
{

/**
 * @brief Voice priority levels
 */
enum class VoicePriority : uint8
{
    Lowest = 0,    ///< Background sounds, can be stolen easily
    Low = 64,      ///< Ambient sounds
    Normal = 128,  ///< Standard sound effects
    High = 192,    ///< Important sounds (explosions, alerts)
    Critical = 255 ///< Never stolen (dialogue, UI)
};

/**
 * @brief Voice state
 */
enum class VoiceState : uint8
{
    Free,        ///< Voice is available
    Playing,     ///< Voice is playing
    Paused,      ///< Voice is paused
    Virtual,     ///< Voice is virtualized (audibility too low)
    FadingOut    ///< Voice is fading out before stop
};

/**
 * @brief Information about a voice
 */
struct VoiceInfo
{
    uint64 handleId = 0;
    VoiceState state = VoiceState::Free;
    VoicePriority priority = VoicePriority::Normal;
    float volume = 1.0f;
    float audibility = 1.0f;  // Computed from distance, volume, etc.
    bool is3D = false;
    Vec3 position{0.0f};
    AudioClip::Ptr clip;
};

/**
 * @brief Voice stealing mode
 */
enum class VoiceStealMode : uint8
{
    None,          ///< Don't steal voices
    OldestFirst,   ///< Steal oldest voice first
    LowestPriority,///< Steal lowest priority first
    QuietestFirst  ///< Steal quietest voice first
};

/**
 * @brief Voice pool configuration
 */
struct VoicePoolConfig
{
    uint32 maxVoices = 64;              ///< Maximum concurrent voices
    uint32 virtualVoiceCount = 128;     ///< Additional virtual voices
    VoiceStealMode stealMode = VoiceStealMode::LowestPriority;
    float virtualizationThreshold = 0.01f;  ///< Audibility threshold for virtualization
    bool enableVirtualization = true;
};

/**
 * @brief Voice pool manages concurrent audio playback
 * 
 * Responsibilities:
 * - Limit concurrent voices
 * - Priority-based voice stealing
 * - Voice virtualization (pause inaudible sounds)
 * - Audibility calculation
 */
class VoicePool
{
public:
    VoicePool() = default;
    ~VoicePool() = default;

    // =========================================================================
    // Initialization
    // =========================================================================

    /**
     * @brief Initialize the voice pool
     */
    bool Initialize(const VoicePoolConfig& config = {});

    /**
     * @brief Shutdown the voice pool
     */
    void Shutdown();

    // =========================================================================
    // Voice Management
    // =========================================================================

    /**
     * @brief Request a voice for playback
     * @return Voice handle, or invalid if no voice available
     */
    AudioHandle RequestVoice(VoicePriority priority = VoicePriority::Normal);

    /**
     * @brief Release a voice
     */
    void ReleaseVoice(AudioHandle handle);

    /**
     * @brief Get voice info
     */
    const VoiceInfo* GetVoiceInfo(AudioHandle handle) const;

    /**
     * @brief Update voice state (call for 3D position changes)
     */
    void UpdateVoice(AudioHandle handle, const VoiceInfo& info);

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update voice pool (call each frame)
     */
    void Update(float deltaTime, const Vec3& listenerPosition);

    // =========================================================================
    // Statistics
    // =========================================================================

    uint32 GetActiveVoiceCount() const { return m_activeCount; }
    uint32 GetVirtualVoiceCount() const { return m_virtualCount; }
    uint32 GetMaxVoices() const { return m_config.maxVoices; }

    /**
     * @brief Check if there are available voices
     */
    bool HasAvailableVoice(VoicePriority priority) const;

private:
    VoicePoolConfig m_config;
    std::vector<VoiceInfo> m_voices;
    std::vector<VoiceInfo> m_virtualVoices;

    uint32 m_activeCount = 0;
    uint32 m_virtualCount = 0;
    uint64 m_nextHandleId = 1;

    /**
     * @brief Find a free voice or steal one
     */
    int32 FindFreeVoice(VoicePriority priority);

    /**
     * @brief Calculate audibility for a voice
     */
    float CalculateAudibility(const VoiceInfo& voice, const Vec3& listenerPosition) const;

    /**
     * @brief Try to virtualize low-audibility voices
     */
    void VirtualizeVoices(const Vec3& listenerPosition);

    /**
     * @brief Try to devirtualize voices
     */
    void DevirtualizeVoices(const Vec3& listenerPosition);
};

} // namespace RVX::Audio
