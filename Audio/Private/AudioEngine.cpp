/**
 * @file AudioEngine.cpp
 * @brief AudioEngine implementation with miniaudio backend
 */

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "Audio/AudioEngine.h"
#include "Audio/AudioClip.h"
#include "Audio/AudioSource.h"
#include "Core/Log.h"
#include <algorithm>
#include <unordered_map>
#include <mutex>

namespace RVX::Audio
{

// =============================================================================
// Internal Data Structures
// =============================================================================

/**
 * @brief Internal sound instance tracking
 */
struct SoundInstance
{
    ma_sound sound;
    uint64 handleId = 0;
    bool is3D = false;
    bool isActive = true;
    AudioClip::Ptr clip;
};

/**
 * @brief Miniaudio backend implementation data
 */
struct AudioEngineBackend
{
    ma_engine engine;
    ma_resource_manager resourceManager;
    bool engineInitialized = false;
    bool resourceManagerInitialized = false;

    std::unordered_map<uint64, std::unique_ptr<SoundInstance>> sounds;
    std::mutex soundsMutex;

    // Bus groups (implemented as ma_sound_group)
    std::unordered_map<uint32, ma_sound_group> busGroups;
};

// =============================================================================
// AudioEngine Implementation
// =============================================================================

AudioEngine::~AudioEngine()
{
    Shutdown();
}

bool AudioEngine::Initialize(const AudioEngineConfig& config)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("AudioEngine already initialized");
        return true;
    }

    m_config = config;

    // Create backend data
    auto backend = std::make_unique<AudioEngineBackend>();

    // Configure and initialize miniaudio engine
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels = config.channels;
    engineConfig.sampleRate = config.sampleRate;
    engineConfig.listenerCount = 1;

    ma_result result = ma_engine_init(&engineConfig, &backend->engine);
    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to initialize miniaudio engine: {}", static_cast<int>(result));
        return false;
    }
    backend->engineInitialized = true;

    // Set initial listener position
    ma_engine_listener_set_position(&backend->engine, 0,
        m_listenerPosition.x, m_listenerPosition.y, m_listenerPosition.z);
    ma_engine_listener_set_direction(&backend->engine, 0,
        m_listenerForward.x, m_listenerForward.y, m_listenerForward.z);
    ma_engine_listener_set_world_up(&backend->engine, 0,
        m_listenerUp.x, m_listenerUp.y, m_listenerUp.z);

    m_backendData = backend.release();
    m_initialized = true;

    RVX_CORE_INFO("AudioEngine initialized (sample rate: {}, channels: {})",
        config.sampleRate, config.channels);

    return true;
}

void AudioEngine::Shutdown()
{
    if (!m_initialized) return;

    StopAll();

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (backend)
    {
        // Clean up all sounds
        {
            std::lock_guard<std::mutex> lock(backend->soundsMutex);
            for (auto& pair : backend->sounds)
            {
                ma_sound_uninit(&pair.second->sound);
            }
            backend->sounds.clear();
        }

        // Clean up bus groups
        for (auto& pair : backend->busGroups)
        {
            ma_sound_group_uninit(&pair.second);
        }
        backend->busGroups.clear();

        // Uninitialize engine
        if (backend->engineInitialized)
        {
            ma_engine_uninit(&backend->engine);
        }

        delete backend;
        m_backendData = nullptr;
    }

    m_sources.clear();
    m_initialized = false;

    RVX_CORE_INFO("AudioEngine shutdown");
}

void AudioEngine::Update(float deltaTime)
{
    if (!m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    // Update listener (in case it changed)
    ma_engine_listener_set_position(&backend->engine, 0,
        m_listenerPosition.x, m_listenerPosition.y, m_listenerPosition.z);
    ma_engine_listener_set_direction(&backend->engine, 0,
        m_listenerForward.x, m_listenerForward.y, m_listenerForward.z);
    ma_engine_listener_set_world_up(&backend->engine, 0,
        m_listenerUp.x, m_listenerUp.y, m_listenerUp.z);
    ma_engine_listener_set_velocity(&backend->engine, 0,
        m_listenerVelocity.x, m_listenerVelocity.y, m_listenerVelocity.z);

    // Clean up finished sounds
    {
        std::lock_guard<std::mutex> lock(backend->soundsMutex);
        for (auto it = backend->sounds.begin(); it != backend->sounds.end();)
        {
            if (!ma_sound_is_playing(&it->second->sound) && !it->second->isActive)
            {
                ma_sound_uninit(&it->second->sound);
                it = backend->sounds.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

AudioClip::Ptr AudioEngine::LoadClip(const std::string& path)
{
    auto clip = AudioClip::Create();
    if (clip->LoadFromFile(path))
    {
        return clip;
    }
    return nullptr;
}

void AudioEngine::UnloadClip(AudioClip::Ptr clip)
{
    if (clip)
    {
        clip->Unload();
    }
}

AudioHandle AudioEngine::Play(AudioClip::Ptr clip, const AudioPlaySettings& settings)
{
    if (!clip || !clip->IsLoaded() || !m_initialized)
    {
        return AudioHandle();
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return AudioHandle();

    uint64 id = m_nextHandleId++;
    auto instance = std::make_unique<SoundInstance>();
    instance->handleId = id;
    instance->is3D = false;
    instance->clip = clip;

    // Initialize sound from file
    ma_result result = ma_sound_init_from_file(&backend->engine,
        clip->GetPath().c_str(),
        MA_SOUND_FLAG_DECODE,
        nullptr, nullptr,
        &instance->sound);

    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to create sound from clip '{}': {}",
            clip->GetPath(), static_cast<int>(result));
        return AudioHandle();
    }

    // Apply settings
    ma_sound_set_volume(&instance->sound, settings.volume * (m_muted ? 0.0f : m_masterVolume));
    ma_sound_set_pitch(&instance->sound, settings.pitch);
    ma_sound_set_pan(&instance->sound, settings.pan);
    ma_sound_set_looping(&instance->sound, settings.loop);

    // Disable spatialization for 2D sounds
    ma_sound_set_spatialization_enabled(&instance->sound, MA_FALSE);

    // Handle fade in
    if (settings.fadeInTime > 0.0f)
    {
        ma_sound_set_fade_in_milliseconds(&instance->sound, 0.0f, settings.volume,
            static_cast<ma_uint64>(settings.fadeInTime * 1000.0f));
    }

    // Start playback (unless starting paused)
    if (!settings.startPaused)
    {
        ma_sound_start(&instance->sound);
    }

    {
        std::lock_guard<std::mutex> lock(backend->soundsMutex);
        backend->sounds[id] = std::move(instance);
    }

    return AudioHandle(id);
}

AudioHandle AudioEngine::Play3D(AudioClip::Ptr clip, const Audio3DSettings& settings3D,
                                const AudioPlaySettings& playSettings)
{
    if (!clip || !clip->IsLoaded() || !m_initialized)
    {
        return AudioHandle();
    }

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return AudioHandle();

    uint64 id = m_nextHandleId++;
    auto instance = std::make_unique<SoundInstance>();
    instance->handleId = id;
    instance->is3D = true;
    instance->clip = clip;

    // Initialize sound from file
    ma_result result = ma_sound_init_from_file(&backend->engine,
        clip->GetPath().c_str(),
        MA_SOUND_FLAG_DECODE,
        nullptr, nullptr,
        &instance->sound);

    if (result != MA_SUCCESS)
    {
        RVX_CORE_ERROR("Failed to create 3D sound from clip '{}': {}",
            clip->GetPath(), static_cast<int>(result));
        return AudioHandle();
    }

    // Apply playback settings
    ma_sound_set_volume(&instance->sound, playSettings.volume * (m_muted ? 0.0f : m_masterVolume));
    ma_sound_set_pitch(&instance->sound, playSettings.pitch);
    ma_sound_set_looping(&instance->sound, playSettings.loop);

    // Enable and configure 3D spatialization
    ma_sound_set_spatialization_enabled(&instance->sound, MA_TRUE);
    ma_sound_set_position(&instance->sound,
        settings3D.position.x, settings3D.position.y, settings3D.position.z);
    ma_sound_set_velocity(&instance->sound,
        settings3D.velocity.x, settings3D.velocity.y, settings3D.velocity.z);
    ma_sound_set_direction(&instance->sound,
        settings3D.direction.x, settings3D.direction.y, settings3D.direction.z);

    // Attenuation settings
    ma_sound_set_min_distance(&instance->sound, settings3D.minDistance);
    ma_sound_set_max_distance(&instance->sound, settings3D.maxDistance);
    ma_sound_set_rolloff(&instance->sound, settings3D.rolloffFactor);

    // Attenuation model
    ma_attenuation_model attModel = ma_attenuation_model_inverse;
    switch (settings3D.attenuationModel)
    {
        case AttenuationModel::None:
            attModel = ma_attenuation_model_none;
            break;
        case AttenuationModel::Linear:
            attModel = ma_attenuation_model_linear;
            break;
        case AttenuationModel::Inverse:
            attModel = ma_attenuation_model_inverse;
            break;
        case AttenuationModel::ExponentialDistance:
            attModel = ma_attenuation_model_exponential;
            break;
    }
    ma_sound_set_attenuation_model(&instance->sound, attModel);

    // Cone settings
    ma_sound_set_cone(&instance->sound,
        settings3D.coneInnerAngle * (3.14159265f / 180.0f),
        settings3D.coneOuterAngle * (3.14159265f / 180.0f),
        settings3D.coneOuterGain);

    // Handle fade in
    if (playSettings.fadeInTime > 0.0f)
    {
        ma_sound_set_fade_in_milliseconds(&instance->sound, 0.0f, playSettings.volume,
            static_cast<ma_uint64>(playSettings.fadeInTime * 1000.0f));
    }

    // Start playback
    if (!playSettings.startPaused)
    {
        ma_sound_start(&instance->sound);
    }

    {
        std::lock_guard<std::mutex> lock(backend->soundsMutex);
        backend->sounds[id] = std::move(instance);
    }

    return AudioHandle(id);
}

void AudioEngine::Stop(AudioHandle handle, float fadeOutTime)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        if (fadeOutTime > 0.0f)
        {
            float currentVolume = ma_sound_get_volume(&it->second->sound);
            ma_sound_set_fade_in_milliseconds(&it->second->sound,
                currentVolume, 0.0f,
                static_cast<ma_uint64>(fadeOutTime * 1000.0f));
            ma_sound_set_stop_time_in_milliseconds(&it->second->sound,
                static_cast<ma_uint64>(fadeOutTime * 1000.0f));
        }
        else
        {
            ma_sound_stop(&it->second->sound);
        }
        it->second->isActive = false;
    }
}

void AudioEngine::StopAll(float fadeOutTime)
{
    if (!m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    for (auto& pair : backend->sounds)
    {
        if (fadeOutTime > 0.0f)
        {
            float currentVolume = ma_sound_get_volume(&pair.second->sound);
            ma_sound_set_fade_in_milliseconds(&pair.second->sound,
                currentVolume, 0.0f,
                static_cast<ma_uint64>(fadeOutTime * 1000.0f));
            ma_sound_set_stop_time_in_milliseconds(&pair.second->sound,
                static_cast<ma_uint64>(fadeOutTime * 1000.0f));
        }
        else
        {
            ma_sound_stop(&pair.second->sound);
        }
        pair.second->isActive = false;
    }
}

void AudioEngine::Pause(AudioHandle handle)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        ma_sound_stop(&it->second->sound);
    }
}

void AudioEngine::Resume(AudioHandle handle)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        ma_sound_start(&it->second->sound);
    }
}

bool AudioEngine::IsPlaying(AudioHandle handle) const
{
    if (!handle.IsValid() || !m_initialized) return false;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return false;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        return ma_sound_is_playing(&it->second->sound) != 0;
    }
    return false;
}

float AudioEngine::GetPlaybackPosition(AudioHandle handle) const
{
    if (!handle.IsValid() || !m_initialized) return 0.0f;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return 0.0f;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        float cursor = 0.0f;
        ma_sound_get_cursor_in_seconds(&it->second->sound, &cursor);
        return cursor;
    }
    return 0.0f;
}

void AudioEngine::SetPlaybackPosition(AudioHandle handle, float position)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        ma_uint64 frameIndex = static_cast<ma_uint64>(position * m_config.sampleRate);
        ma_sound_seek_to_pcm_frame(&it->second->sound, frameIndex);
    }
}

void AudioEngine::SetVolume(AudioHandle handle, float volume)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        ma_sound_set_volume(&it->second->sound, volume * (m_muted ? 0.0f : m_masterVolume));
    }
}

void AudioEngine::SetPitch(AudioHandle handle, float pitch)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        ma_sound_set_pitch(&it->second->sound, pitch);
    }
}

void AudioEngine::SetPan(AudioHandle handle, float pan)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        ma_sound_set_pan(&it->second->sound, pan);
    }
}

void AudioEngine::SetLooping(AudioHandle handle, bool loop)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end())
    {
        ma_sound_set_looping(&it->second->sound, loop);
    }
}

void AudioEngine::SetPosition(AudioHandle handle, const Vec3& position)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end() && it->second->is3D)
    {
        ma_sound_set_position(&it->second->sound, position.x, position.y, position.z);
    }
}

void AudioEngine::SetVelocity(AudioHandle handle, const Vec3& velocity)
{
    if (!handle.IsValid() || !m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    std::lock_guard<std::mutex> lock(backend->soundsMutex);
    auto it = backend->sounds.find(handle.GetId());
    if (it != backend->sounds.end() && it->second->is3D)
    {
        ma_sound_set_velocity(&it->second->sound, velocity.x, velocity.y, velocity.z);
    }
}

void AudioEngine::SetListenerTransform(const Vec3& position, const Vec3& forward, const Vec3& up)
{
    m_listenerPosition = position;
    m_listenerForward = forward;
    m_listenerUp = up;

    if (!m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    ma_engine_listener_set_position(&backend->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&backend->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&backend->engine, 0, up.x, up.y, up.z);
}

void AudioEngine::SetListenerVelocity(const Vec3& velocity)
{
    m_listenerVelocity = velocity;

    if (!m_initialized) return;

    auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
    if (!backend) return;

    ma_engine_listener_set_velocity(&backend->engine, 0, velocity.x, velocity.y, velocity.z);
}

uint32 AudioEngine::CreateBus(const std::string& name, uint32 parentBus)
{
    AudioBus bus;
    bus.id = static_cast<uint32>(m_buses.size() + 1);
    bus.parentBus = parentBus;
    m_buses.push_back(bus);

    // Create miniaudio sound group for this bus
    if (m_initialized)
    {
        auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
        if (backend)
        {
            ma_sound_group group;
            ma_sound_group_config groupConfig = ma_sound_group_config_init();
            
            ma_sound_group* parentGroup = nullptr;
            if (parentBus > 0)
            {
                auto it = backend->busGroups.find(parentBus);
                if (it != backend->busGroups.end())
                {
                    parentGroup = &it->second;
                }
            }

            if (ma_sound_group_init(&backend->engine, 0, parentGroup, &group) == MA_SUCCESS)
            {
                backend->busGroups[bus.id] = group;
            }
        }
    }

    return bus.id;
}

void AudioEngine::SetBusVolume(uint32 busId, float volume)
{
    for (auto& bus : m_buses)
    {
        if (bus.id == busId)
        {
            bus.volume = volume;

            if (m_initialized)
            {
                auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
                if (backend)
                {
                    auto it = backend->busGroups.find(busId);
                    if (it != backend->busGroups.end())
                    {
                        ma_sound_group_set_volume(&it->second, volume);
                    }
                }
            }
            break;
        }
    }
}

void AudioEngine::SetBusMuted(uint32 busId, bool muted)
{
    for (auto& bus : m_buses)
    {
        if (bus.id == busId)
        {
            bus.muted = muted;

            if (m_initialized)
            {
                auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
                if (backend)
                {
                    auto it = backend->busGroups.find(busId);
                    if (it != backend->busGroups.end())
                    {
                        ma_sound_group_set_volume(&it->second, muted ? 0.0f : bus.volume);
                    }
                }
            }
            break;
        }
    }
}

AudioEngine::Statistics AudioEngine::GetStatistics() const
{
    Statistics stats;
    stats.totalVoices = m_config.maxVoices;

    if (m_initialized)
    {
        auto* backend = static_cast<AudioEngineBackend*>(m_backendData);
        if (backend)
        {
            std::lock_guard<std::mutex> lock(backend->soundsMutex);
            stats.activeVoices = static_cast<uint32>(backend->sounds.size());
        }
    }

    return stats;
}

AudioEngine& GetAudioEngine()
{
    static AudioEngine instance;
    return instance;
}

} // namespace RVX::Audio
