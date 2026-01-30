/**
 * @file AudioEventManager.h
 * @brief Manager for data-driven audio events
 */

#pragma once

#include "Audio/Events/AudioEvent.h"
#include "Audio/AudioEngine.h"
#include <unordered_map>
#include <string>
#include <functional>

namespace RVX::Audio
{

/**
 * @brief Callback for event playback
 */
using AudioEventCallback = std::function<void(const std::string& eventName, AudioHandle handle)>;

/**
 * @brief Manager for data-driven audio events
 * 
 * The AudioEventManager provides a higher-level interface for playing
 * sounds based on event names rather than direct clip references.
 * 
 * Features:
 * - Event registration and lookup by name
 * - Automatic clip variation and randomization
 * - Instance limiting and cooldowns
 * - 3D spatial playback
 * - Event callbacks
 * 
 * Usage:
 * @code
 * AudioEventManager manager(&engine);
 * 
 * // Register an event
 * AudioEventDesc footstep;
 * footstep.name = "player/footstep";
 * footstep.clips = { {clip1, 1.0f}, {clip2, 1.0f}, {clip3, 0.5f} };
 * footstep.volumeMin = 0.8f;
 * footstep.volumeMax = 1.0f;
 * footstep.pitchMin = 0.95f;
 * footstep.pitchMax = 1.05f;
 * manager.RegisterEvent(footstep);
 * 
 * // Play the event
 * manager.PostEvent("player/footstep");
 * 
 * // 3D event
 * manager.PostEvent3D("explosion", position);
 * @endcode
 */
class AudioEventManager
{
public:
    AudioEventManager(AudioEngine* engine);
    ~AudioEventManager() = default;

    // =========================================================================
    // Event Registration
    // =========================================================================

    /**
     * @brief Register an audio event
     */
    void RegisterEvent(const AudioEventDesc& desc);

    /**
     * @brief Unregister an event
     */
    void UnregisterEvent(const std::string& name);

    /**
     * @brief Check if an event exists
     */
    bool HasEvent(const std::string& name) const;

    /**
     * @brief Get an event by name
     */
    AudioEvent* GetEvent(const std::string& name);
    const AudioEvent* GetEvent(const std::string& name) const;

    /**
     * @brief Clear all events
     */
    void ClearAllEvents();

    // =========================================================================
    // Event Playback
    // =========================================================================

    /**
     * @brief Post (trigger) an audio event by name
     * @return Handle to the playing sound, or invalid if event doesn't exist or can't play
     */
    AudioHandle PostEvent(const std::string& name);

    /**
     * @brief Post a 3D audio event
     */
    AudioHandle PostEvent3D(const std::string& name, const Vec3& position);

    /**
     * @brief Post a 3D audio event with full spatial settings
     */
    AudioHandle PostEvent3D(const std::string& name, const Audio3DSettings& spatial);

    /**
     * @brief Stop all instances of an event
     */
    void StopEvent(const std::string& name, float fadeOutTime = 0.0f);

    /**
     * @brief Stop all events
     */
    void StopAllEvents(float fadeOutTime = 0.0f);

    // =========================================================================
    // Callbacks
    // =========================================================================

    /**
     * @brief Set callback for when an event starts playing
     */
    void SetOnEventPlayed(AudioEventCallback callback);

    /**
     * @brief Set callback for when an event stops
     */
    void SetOnEventStopped(AudioEventCallback callback);

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update all events (call each frame)
     */
    void Update(float deltaTime);

    // =========================================================================
    // Statistics
    // =========================================================================

    /**
     * @brief Get total registered event count
     */
    size_t GetEventCount() const { return m_events.size(); }

    /**
     * @brief Get total active instance count across all events
     */
    int GetTotalActiveInstances() const;

private:
    AudioEngine* m_engine;
    std::unordered_map<std::string, AudioEvent::Ptr> m_events;
    
    AudioEventCallback m_onEventPlayed;
    AudioEventCallback m_onEventStopped;
    
    // Track all active handles for cleanup
    std::unordered_map<uint64, std::string> m_handleToEventName;
};

} // namespace RVX::Audio
