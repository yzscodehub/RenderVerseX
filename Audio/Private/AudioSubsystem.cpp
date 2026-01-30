/**
 * @file AudioSubsystem.cpp
 * @brief AudioSubsystem implementation
 */

#include "Audio/AudioSubsystem.h"
#include "Audio/Mixer/AudioMixer.h"
#include "Audio/Music/MusicPlayer.h"
#include "Core/Log.h"

namespace RVX::Audio
{

void AudioSubsystem::Initialize()
{
    RVX_CORE_INFO("Initializing AudioSubsystem...");

    if (!m_engine.Initialize(m_config))
    {
        RVX_CORE_ERROR("Failed to initialize AudioEngine");
        return;
    }

    // Create default buses
    m_engine.CreateBus("Master", 0);
    m_engine.CreateBus("Music", 1);
    m_engine.CreateBus("SFX", 1);
    m_engine.CreateBus("Voice", 1);
    m_engine.CreateBus("Ambient", 1);
    m_engine.CreateBus("UI", 1);

    RVX_CORE_INFO("AudioSubsystem initialized successfully");
}

void AudioSubsystem::Deinitialize()
{
    RVX_CORE_INFO("Shutting down AudioSubsystem...");

    // Clear cached clips
    m_clipCache.clear();

    // Shutdown music player
    m_musicPlayer.reset();

    // Shutdown mixer
    m_mixer.reset();

    // Shutdown engine
    m_engine.Shutdown();

    RVX_CORE_INFO("AudioSubsystem shutdown complete");
}

void AudioSubsystem::Tick(float deltaTime)
{
    if (m_paused)
    {
        return;
    }

    // Update audio engine
    m_engine.Update(deltaTime);

    // Update music player if available
    if (m_musicPlayer)
    {
        // Will be implemented in Phase 6
    }
}

void AudioSubsystem::SetConfig(const AudioEngineConfig& config)
{
    if (m_engine.IsInitialized())
    {
        RVX_CORE_WARN("Cannot change audio config after initialization");
        return;
    }
    m_config = config;
}

AudioHandle AudioSubsystem::PlaySound(const std::string& path, float volume)
{
    // Check cache first
    auto it = m_clipCache.find(path);
    AudioClip::Ptr clip;

    if (it != m_clipCache.end())
    {
        clip = it->second;
    }
    else
    {
        clip = m_engine.LoadClip(path);
        if (clip)
        {
            m_clipCache[path] = clip;
        }
    }

    if (!clip)
    {
        RVX_CORE_WARN("Failed to play sound: {}", path);
        return AudioHandle();
    }

    AudioPlaySettings settings;
    settings.volume = volume;
    return m_engine.Play(clip, settings);
}

AudioHandle AudioSubsystem::PlaySound3D(const std::string& path, const Vec3& position, float volume)
{
    // Check cache first
    auto it = m_clipCache.find(path);
    AudioClip::Ptr clip;

    if (it != m_clipCache.end())
    {
        clip = it->second;
    }
    else
    {
        clip = m_engine.LoadClip(path);
        if (clip)
        {
            m_clipCache[path] = clip;
        }
    }

    if (!clip)
    {
        RVX_CORE_WARN("Failed to play 3D sound: {}", path);
        return AudioHandle();
    }

    Audio3DSettings settings3D;
    settings3D.position = position;

    AudioPlaySettings playSettings;
    playSettings.volume = volume;

    return m_engine.Play3D(clip, settings3D, playSettings);
}

void AudioSubsystem::SetPaused(bool paused)
{
    if (m_paused == paused)
    {
        return;
    }

    m_paused = paused;

    if (paused)
    {
        PauseAll();
    }
    else
    {
        ResumeAll();
    }
}

void AudioSubsystem::PauseAll()
{
    // Note: This would need to track all playing sounds
    // For now, we rely on the Update loop being paused
    m_paused = true;
    RVX_CORE_DEBUG("Audio paused");
}

void AudioSubsystem::ResumeAll()
{
    m_paused = false;
    RVX_CORE_DEBUG("Audio resumed");
}

} // namespace RVX::Audio
