/**
 * @file AudioSubsystem.h
 * @brief Engine subsystem for audio management
 */

#pragma once

#include "Core/Subsystem/EngineSubsystem.h"
#include "Audio/AudioEngine.h"
#include "Audio/Mixer/AudioBus.h"
#include "Audio/Spatial/IOcclusionProvider.h"
#include <list>
#include <memory>
#include <unordered_map>

namespace RVX::Physics
{
    class PhysicsWorld;
} // namespace RVX::Physics

namespace RVX::Audio
{

class AudioMixer;
class MusicPlayer;
class AudioZoneManager;

/**
 * @brief Engine subsystem for audio management
 * 
 * Provides centralized audio functionality as an engine subsystem.
 * Initializes the AudioEngine and provides access to audio services.
 * 
 * Usage:
 * @code
 * // Get the subsystem from engine
 * auto* audioSub = engine->GetSubsystem<AudioSubsystem>();
 * 
 * // Play a sound
 * auto clip = audioSub->GetEngine().LoadClip("explosion.wav");
 * audioSub->GetEngine().Play(clip);
 * @endcode
 */
class AudioSubsystem : public EngineSubsystem
{
public:
    // =========================================================================
    // Construction
    // =========================================================================

    AudioSubsystem();
    ~AudioSubsystem() override;

    // Non-copyable
    AudioSubsystem(const AudioSubsystem&) = delete;
    AudioSubsystem& operator=(const AudioSubsystem&) = delete;

    // =========================================================================
    // ISubsystem Interface
    // =========================================================================

    const char* GetName() const override { return "AudioSubsystem"; }

    bool ShouldTick() const override { return true; }

    TickPhase GetTickPhase() const override { return TickPhase::PostUpdate; }

    void Initialize() override;
    void Deinitialize() override;
    void Tick(float deltaTime) override;

    // =========================================================================
    // Audio Access
    // =========================================================================

    /**
     * @brief Get the audio engine
     */
    AudioEngine& GetEngine() { return m_engine; }
    const AudioEngine& GetEngine() const { return m_engine; }

    /**
     * @brief Get the audio mixer (if available)
     */
    AudioMixer* GetMixer() { return m_mixer.get(); }
    const AudioMixer* GetMixer() const { return m_mixer.get(); }

    /**
     * @brief Add an effect to a mixer bus and sync it to the runtime graph
     */
    bool AddBusEffect(uint32 busId, std::shared_ptr<IAudioEffect> effect);

    /**
     * @brief Sync a mixer bus effect chain to the runtime graph
     */
    void SyncBusEffects(uint32 busId);

    /**
     * @brief Set a mixer bus send amount and sync it to the runtime graph
     */
    bool SetBusSend(uint32 sourceBusId, uint32 targetBusId, float amount);

    /**
     * @brief Get the music player (if available)
     */
    MusicPlayer* GetMusicPlayer() { return m_musicPlayer.get(); }
    const MusicPlayer* GetMusicPlayer() const { return m_musicPlayer.get(); }

    /**
     * @brief Get the spatial audio zone manager
     */
    AudioZoneManager* GetZoneManager() { return m_zoneManager.get(); }
    const AudioZoneManager* GetZoneManager() const { return m_zoneManager.get(); }

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set audio configuration before initialization
     * @note Must be called before Initialize()
     */
    void SetConfig(const AudioEngineConfig& config);

    /**
     * @brief Get current configuration
     */
    const AudioEngineConfig& GetConfig() const { return m_config; }

    // =========================================================================
    // Convenience Methods
    // =========================================================================

    /**
     * @brief Quick play a sound by path
     */
    AudioHandle PlaySound(const std::string& path,
                          float volume = 1.0f,
                          uint32 busId = BusId::SFX);

    /**
     * @brief Quick play a 3D sound by path
     */
    AudioHandle PlaySound3D(const std::string& path,
                            const Vec3& position,
                            float volume = 1.0f,
                            uint32 busId = BusId::SFX);

    /**
     * @brief Get a runtime bus id by name
     * @return Bus id, or RVX_INVALID_INDEX when no bus with that name exists
     */
    uint32 GetBusId(const std::string& name) const;

    uint32 GetMasterBusId() const { return BusId::Master; }
    uint32 GetMusicBusId() const { return BusId::Music; }
    uint32 GetSFXBusId() const { return BusId::SFX; }
    uint32 GetVoiceBusId() const { return BusId::Voice; }
    uint32 GetAmbientBusId() const { return BusId::Ambient; }
    uint32 GetUIBusId() const { return BusId::UI; }

    // =========================================================================
    // Listener and Spatial Audio
    // =========================================================================

    /**
     * @brief Set listener transform for spatialization and audio zones
     */
    void SetListenerTransform(const Vec3& position,
                              const Vec3& forward = Vec3(0.0f, 0.0f, -1.0f),
                              const Vec3& up = Vec3(0.0f, 1.0f, 0.0f));

    /**
     * @brief Set listener velocity for doppler
     */
    void SetListenerVelocity(const Vec3& velocity);

    const Vec3& GetListenerPosition() const { return m_listenerPosition; }
    const Vec3& GetListenerVelocity() const { return m_listenerVelocity; }

    /**
     * @brief Set custom occlusion provider used by the zone manager
     */
    void SetOcclusionProvider(std::shared_ptr<IOcclusionProvider> provider);
    IOcclusionProvider* GetOcclusionProvider();

    /**
     * @brief Bind a PhysicsWorld-backed raycast occlusion provider
     */
    void SetPhysicsWorld(::RVX::Physics::PhysicsWorld* physicsWorld, uint32 layerMask = 0xFFFFFFFFu);

    /**
     * @brief Calculate occlusion from source position to the current listener
     */
    OcclusionResult GetOcclusion(const Vec3& sourcePosition);

    /**
     * @brief Set global audio pause state
     */
    void SetPaused(bool paused);
    bool IsPaused() const { return m_paused; }

    /**
     * @brief Configure maximum number of quick-play clips retained in memory
     */
    void SetMaxCachedClips(uint32 maxCachedClips);
    uint32 GetMaxCachedClips() const { return m_maxCachedClips; }
    size_t GetCachedClipCount() const { return m_clipCache.size(); }
    bool HasCachedClip(const std::string& path) const { return m_clipCache.find(path) != m_clipCache.end(); }

    /**
     * @brief Pause all audio (e.g., when game is paused)
     */
    void PauseAll();

    /**
     * @brief Resume all audio
     */
    void ResumeAll();

private:
    struct CachedClip
    {
        AudioClip::Ptr clip;
        std::list<std::string>::iterator lruIt;
    };

    AudioEngineConfig m_config;
    AudioEngine m_engine;

    std::unique_ptr<AudioMixer> m_mixer;
    std::unique_ptr<MusicPlayer> m_musicPlayer;
    std::unique_ptr<AudioZoneManager> m_zoneManager;
    std::shared_ptr<IOcclusionProvider> m_occlusionProvider;

    bool m_paused = false;
    Vec3 m_listenerPosition{0.0f};
    Vec3 m_listenerForward{0.0f, 0.0f, -1.0f};
    Vec3 m_listenerUp{0.0f, 1.0f, 0.0f};
    Vec3 m_listenerVelocity{0.0f};

    // Cache for quick play sounds
    uint32 m_maxCachedClips = 128;
    std::list<std::string> m_clipCacheLru;
    std::unordered_map<std::string, CachedClip> m_clipCache;
    std::unordered_map<uint32, size_t> m_busEffectFingerprints;

    AudioClip::Ptr GetOrLoadClip(const std::string& path);
    void SyncDirtyBusEffects();
    void TouchCachedClip(std::unordered_map<std::string, CachedClip>::iterator it);
    void TrimClipCache();
};

} // namespace RVX::Audio
