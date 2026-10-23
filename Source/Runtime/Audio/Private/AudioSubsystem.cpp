/**
 * @file AudioSubsystem.cpp
 * @brief AudioSubsystem implementation
 */

#include "Audio/AudioSubsystem.h"
#include "Audio/DSP/IAudioEffect.h"
#include "Audio/Mixer/AudioMixer.h"
#include "Audio/Music/MusicPlayer.h"
#include "Audio/Spatial/AudioZoneManager.h"
#include "Audio/Spatial/IOcclusionProvider.h"
#include "Core/Log.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string_view>
#include <vector>

namespace RVX::Audio
{

namespace
{
    constexpr size_t RVX_AUDIO_EMPTY_EFFECT_FINGERPRINT = 0u;

    void HashCombine(size_t& seed, size_t value)
    {
        seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6u) + (seed >> 2u);
    }

    size_t HashFloat(float value)
    {
        return std::hash<float>{}(value);
    }

    size_t CalculateBusEffectFingerprint(const AudioBusNode& bus)
    {
        const auto& effects = bus.GetEffects();
        const auto& sends = bus.GetSends();
        if (effects.empty() && sends.empty())
        {
            return RVX_AUDIO_EMPTY_EFFECT_FINGERPRINT;
        }

        size_t fingerprint = 0xC001D00Du;
        HashCombine(fingerprint, effects.size());

        static constexpr std::array<const char*, 16> kTrackedParameters =
        {
            "cutoff",
            "resonance",
            "roomSize",
            "damping",
            "wetLevel",
            "dryLevel",
            "width",
            "preDelay",
            "delayTime",
            "feedback",
            "spread",
            "threshold",
            "ratio",
            "attack",
            "release",
            "makeupGain"
        };

        for (const std::shared_ptr<IAudioEffect>& effect : effects)
        {
            if (!effect)
            {
                HashCombine(fingerprint, 0u);
                continue;
            }

            HashCombine(fingerprint, reinterpret_cast<size_t>(effect.get()));
            HashCombine(fingerprint, static_cast<size_t>(effect->GetType()));
            HashCombine(fingerprint, effect->IsEnabled() ? 1u : 0u);
            HashCombine(fingerprint, HashFloat(effect->GetMix()));

            for (const char* parameter : kTrackedParameters)
            {
                if (effect->HasParameter(parameter))
                {
                    HashCombine(fingerprint, std::hash<std::string_view>{}(parameter));
                    HashCombine(fingerprint, HashFloat(effect->GetParameter(parameter)));
                }
            }
        }

        std::vector<AudioBusSend> sortedSends;
        sortedSends.reserve(sends.size());
        for (const auto& pair : sends)
        {
            sortedSends.push_back(AudioBusSend{pair.first, pair.second});
        }
        std::sort(sortedSends.begin(), sortedSends.end(),
            [](const AudioBusSend& lhs, const AudioBusSend& rhs) {
                return lhs.targetBusId < rhs.targetBusId;
            });

        HashCombine(fingerprint, sortedSends.size());
        for (const AudioBusSend& send : sortedSends)
        {
            HashCombine(fingerprint, send.targetBusId);
            HashCombine(fingerprint, HashFloat(send.amount));
        }

        return fingerprint;
    }

    std::vector<AudioBusSend> CollectBusSends(const AudioBusNode& bus)
    {
        std::vector<AudioBusSend> sends;
        sends.reserve(bus.GetSends().size());
        for (const auto& pair : bus.GetSends())
        {
            sends.push_back(AudioBusSend{pair.first, pair.second});
        }
        std::sort(sends.begin(), sends.end(),
            [](const AudioBusSend& lhs, const AudioBusSend& rhs) {
                return lhs.targetBusId < rhs.targetBusId;
            });
        return sends;
    }

    bool HasMixerSendPath(const AudioMixer& mixer,
                          uint32 currentBusId,
                          uint32 targetBusId,
                          uint32 ignoredSourceBusId,
                          std::vector<uint32>& visited)
    {
        if (currentBusId == targetBusId)
        {
            return true;
        }
        if (currentBusId == ignoredSourceBusId)
        {
            return false;
        }
        if (std::find(visited.begin(), visited.end(), currentBusId) != visited.end())
        {
            return false;
        }

        visited.push_back(currentBusId);

        const AudioBusNode* bus = mixer.GetBus(currentBusId);
        if (!bus)
        {
            return false;
        }

        for (const auto& pair : bus->GetSends())
        {
            if (HasMixerSendPath(mixer, pair.first, targetBusId, ignoredSourceBusId, visited))
            {
                return true;
            }
        }

        return false;
    }

    bool WouldCreateMixerSendCycle(const AudioMixer& mixer, uint32 sourceBusId, uint32 targetBusId)
    {
        if (sourceBusId == targetBusId)
        {
            return true;
        }

        std::vector<uint32> visited;
        return HasMixerSendPath(mixer, targetBusId, sourceBusId, sourceBusId, visited);
    }
} // namespace

AudioSubsystem::AudioSubsystem() = default;

AudioSubsystem::~AudioSubsystem() = default;

void AudioSubsystem::Initialize()
{
    RVX_CORE_INFO("Initializing AudioSubsystem...");

    m_maxCachedClips = m_config.maxCachedClips;

    if (!m_engine.Initialize(m_config))
    {
        RVX_CORE_ERROR("Failed to initialize AudioEngine");
        return;
    }

    // Create default buses
    m_engine.CreateBus("Master", BusId::Master);
    m_engine.CreateBus("Music", BusId::Master);
    m_engine.CreateBus("SFX", BusId::Master);
    m_engine.CreateBus("Voice", BusId::Master);
    m_engine.CreateBus("Ambient", BusId::Master);
    m_engine.CreateBus("UI", BusId::Master);

    m_mixer = std::make_unique<AudioMixer>();
    AudioMixerConfig mixerConfig;
    mixerConfig.sampleRate = m_config.sampleRate;
    mixerConfig.channels = m_config.channels;
    mixerConfig.bufferSize = m_config.bufferSizeFrames;
    mixerConfig.enableEffects = true;
    if (!m_mixer->Initialize(mixerConfig))
    {
        RVX_CORE_WARN("AudioSubsystem continuing without AudioMixer");
        m_mixer.reset();
    }

    m_zoneManager = std::make_unique<AudioZoneManager>(&m_engine);
    if (m_occlusionProvider)
    {
        m_zoneManager->SetOcclusionProvider(m_occlusionProvider);
    }
    m_zoneManager->Update(m_listenerPosition, 0.0f);

    RVX_CORE_INFO("AudioSubsystem initialized successfully");
}

void AudioSubsystem::Deinitialize()
{
    RVX_CORE_INFO("Shutting down AudioSubsystem...");

    // Clear cached clips
    m_clipCache.clear();
    m_clipCacheLru.clear();
    m_busEffectFingerprints.clear();

    // Shutdown music player
    m_musicPlayer.reset();

    // Shutdown mixer
    if (m_mixer)
    {
        m_mixer->Shutdown();
        m_mixer.reset();
    }

    // Shutdown spatial audio state
    m_zoneManager.reset();
    m_occlusionProvider.reset();

    // Shutdown engine
    m_engine.Shutdown();

    RVX_CORE_INFO("AudioSubsystem shutdown complete");
}

bool AudioSubsystem::AddBusEffect(uint32 busId, std::shared_ptr<IAudioEffect> effect)
{
    if (!effect)
    {
        return false;
    }

    if (!m_mixer)
    {
        RVX_CORE_WARN("Cannot add audio bus effect without an initialized mixer");
        return false;
    }

    if (!m_engine.HasBus(busId))
    {
        RVX_CORE_WARN("Cannot add audio bus effect: runtime bus {} does not exist", busId);
        return false;
    }

    AudioBusNode* bus = m_mixer->GetBus(busId);
    if (!bus)
    {
        RVX_CORE_WARN("Cannot add audio bus effect: mixer bus {} does not exist", busId);
        return false;
    }

    bus->AddEffect(effect);
    SyncBusEffects(busId);
    return true;
}

void AudioSubsystem::SyncBusEffects(uint32 busId)
{
    if (!m_mixer)
    {
        return;
    }

    AudioBusNode* bus = m_mixer->GetBus(busId);
    if (!bus)
    {
        return;
    }

    const size_t fingerprint = CalculateBusEffectFingerprint(*bus);
    m_engine.ApplyBusEffects(busId, bus->GetEffects());

    std::vector<AudioBusSend> sends = CollectBusSends(*bus);
    sends.erase(
        std::remove_if(sends.begin(), sends.end(),
            [this, busId](const AudioBusSend& send) {
                const bool cyclic = m_mixer && WouldCreateMixerSendCycle(*m_mixer, busId, send.targetBusId);
                if (cyclic)
                {
                    RVX_CORE_WARN("Dropping cyclic mixer bus send {} -> {}", busId, send.targetBusId);
                }
                return cyclic;
            }),
        sends.end());
    m_engine.SetBusSends(busId, sends);

    if (fingerprint == RVX_AUDIO_EMPTY_EFFECT_FINGERPRINT)
    {
        m_busEffectFingerprints.erase(busId);
    }
    else
    {
        m_busEffectFingerprints[busId] = fingerprint;
    }
}

bool AudioSubsystem::SetBusSend(uint32 sourceBusId, uint32 targetBusId, float amount)
{
    if (!m_mixer)
    {
        RVX_CORE_WARN("Cannot set audio bus send without an initialized mixer");
        return false;
    }

    if (!m_engine.HasBus(sourceBusId) || !m_engine.HasBus(targetBusId))
    {
        RVX_CORE_WARN("Cannot set audio bus send: source {} or target {} does not exist", sourceBusId, targetBusId);
        return false;
    }

    AudioBusNode* sourceBus = m_mixer->GetBus(sourceBusId);
    AudioBusNode* targetBus = m_mixer->GetBus(targetBusId);
    if (!sourceBus || !targetBus)
    {
        RVX_CORE_WARN("Cannot set audio bus send: mixer source {} or target {} does not exist", sourceBusId, targetBusId);
        return false;
    }
    if (amount > 0.0f && WouldCreateMixerSendCycle(*m_mixer, sourceBusId, targetBusId))
    {
        RVX_CORE_WARN("Cannot set audio bus send: route {} -> {} would create a cycle", sourceBusId, targetBusId);
        return false;
    }

    sourceBus->SetSend(targetBusId, amount);
    SyncBusEffects(sourceBusId);
    return true;
}

void AudioSubsystem::Tick(float deltaTime)
{
    if (m_paused)
    {
        return;
    }

    if (m_mixer)
    {
        m_mixer->Update(deltaTime, m_listenerPosition);
    }

    // Update audio engine
    m_engine.Update(deltaTime);

    if (m_zoneManager)
    {
        m_zoneManager->Update(m_listenerPosition, deltaTime);
    }

    SyncDirtyBusEffects();

    // Update music player if available
    if (m_musicPlayer)
    {
        // Will be implemented in Phase 6
    }
}

void AudioSubsystem::SyncDirtyBusEffects()
{
    if (!m_mixer)
    {
        return;
    }

    for (uint32 busId : m_mixer->GetBusIds())
    {
        AudioBusNode* bus = m_mixer->GetBus(busId);
        if (!bus)
        {
            continue;
        }

        const size_t fingerprint = CalculateBusEffectFingerprint(*bus);
        auto trackedIt = m_busEffectFingerprints.find(busId);
        const bool wasTracked = trackedIt != m_busEffectFingerprints.end();
        if (!wasTracked && fingerprint == RVX_AUDIO_EMPTY_EFFECT_FINGERPRINT)
        {
            continue;
        }
        if (wasTracked && trackedIt->second == fingerprint)
        {
            continue;
        }

        SyncBusEffects(busId);
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

AudioHandle AudioSubsystem::PlaySound(const std::string& path, float volume, uint32 busId)
{
    AudioClip::Ptr clip = GetOrLoadClip(path);

    if (!clip)
    {
        RVX_CORE_WARN("Failed to play sound: {}", path);
        return AudioHandle();
    }

    AudioPlaySettings settings;
    settings.volume = volume;
    settings.busId = busId;
    return m_engine.Play(clip, settings);
}

AudioHandle AudioSubsystem::PlaySound3D(const std::string& path,
                                        const Vec3& position,
                                        float volume,
                                        uint32 busId)
{
    AudioClip::Ptr clip = GetOrLoadClip(path);

    if (!clip)
    {
        RVX_CORE_WARN("Failed to play 3D sound: {}", path);
        return AudioHandle();
    }

    Audio3DSettings settings3D;
    settings3D.position = position;

    AudioPlaySettings playSettings;
    playSettings.volume = volume;
    playSettings.busId = busId;

    return m_engine.Play3D(clip, settings3D, playSettings);
}

uint32 AudioSubsystem::GetBusId(const std::string& name) const
{
    return m_engine.GetBusId(name);
}

void AudioSubsystem::SetListenerTransform(const Vec3& position, const Vec3& forward, const Vec3& up)
{
    m_listenerPosition = position;
    m_listenerForward = forward;
    m_listenerUp = up;

    m_engine.SetListenerTransform(position, forward, up);
    if (m_zoneManager)
    {
        m_zoneManager->Update(position, 0.0f);
    }
}

void AudioSubsystem::SetListenerVelocity(const Vec3& velocity)
{
    m_listenerVelocity = velocity;
    m_engine.SetListenerVelocity(velocity);
}

void AudioSubsystem::SetOcclusionProvider(std::shared_ptr<IOcclusionProvider> provider)
{
    m_occlusionProvider = std::move(provider);
    if (m_zoneManager)
    {
        m_zoneManager->SetOcclusionProvider(m_occlusionProvider);
    }
}

IOcclusionProvider* AudioSubsystem::GetOcclusionProvider()
{
    return m_zoneManager ? m_zoneManager->GetOcclusionProvider() : m_occlusionProvider.get();
}

void AudioSubsystem::SetPhysicsWorld(::RVX::Physics::PhysicsWorld* physicsWorld, uint32 layerMask)
{
    if (!physicsWorld)
    {
        SetOcclusionProvider(nullptr);
        return;
    }

    auto provider = std::make_shared<RaycastOcclusionProvider>();
    provider->SetPhysicsWorld(physicsWorld);
    provider->SetLayerMask(layerMask);
    SetOcclusionProvider(provider);
}

OcclusionResult AudioSubsystem::GetOcclusion(const Vec3& sourcePosition)
{
    return m_zoneManager ? m_zoneManager->GetOcclusion(sourcePosition) : OcclusionResult{};
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

void AudioSubsystem::SetMaxCachedClips(uint32 maxCachedClips)
{
    m_maxCachedClips = maxCachedClips;
    m_config.maxCachedClips = maxCachedClips;
    TrimClipCache();
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

AudioClip::Ptr AudioSubsystem::GetOrLoadClip(const std::string& path)
{
    if (path.empty())
    {
        return nullptr;
    }

    if (m_maxCachedClips > 0)
    {
        auto it = m_clipCache.find(path);
        if (it != m_clipCache.end())
        {
            TouchCachedClip(it);
            return it->second.clip;
        }
    }

    AudioClip::Ptr clip = m_engine.LoadClip(path);
    if (!clip || m_maxCachedClips == 0)
    {
        return clip;
    }

    m_clipCacheLru.push_front(path);
    auto lruIt = m_clipCacheLru.begin();
    m_clipCache.emplace(path, CachedClip{clip, lruIt});
    TrimClipCache();
    return clip;
}

void AudioSubsystem::TouchCachedClip(std::unordered_map<std::string, CachedClip>::iterator it)
{
    if (it == m_clipCache.end())
    {
        return;
    }

    m_clipCacheLru.erase(it->second.lruIt);
    m_clipCacheLru.push_front(it->first);
    it->second.lruIt = m_clipCacheLru.begin();
}

void AudioSubsystem::TrimClipCache()
{
    while (m_clipCache.size() > m_maxCachedClips && !m_clipCacheLru.empty())
    {
        const std::string path = m_clipCacheLru.back();
        m_clipCacheLru.pop_back();
        m_clipCache.erase(path);
    }
}

} // namespace RVX::Audio
