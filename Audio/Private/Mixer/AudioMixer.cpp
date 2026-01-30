/**
 * @file AudioMixer.cpp
 * @brief AudioMixer implementation
 */

#include "Audio/Mixer/AudioMixer.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX::Audio
{

bool AudioMixer::Initialize(const AudioMixerConfig& config)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("AudioMixer already initialized");
        return true;
    }

    m_config = config;

    // Initialize voice pool
    VoicePoolConfig vpConfig;
    vpConfig.maxVoices = 64;
    vpConfig.virtualVoiceCount = 128;
    vpConfig.enableVirtualization = true;
    
    if (!m_voicePool.Initialize(vpConfig))
    {
        RVX_CORE_ERROR("Failed to initialize voice pool");
        return false;
    }

    // Create default bus hierarchy
    CreateDefaultBuses();

    m_initialized = true;
    RVX_CORE_INFO("AudioMixer initialized");

    return true;
}

void AudioMixer::Shutdown()
{
    if (!m_initialized) return;

    m_voicePool.Shutdown();

    m_buses.clear();
    m_busNameToId.clear();
    m_masterBus.reset();
    m_snapshots.clear();
    m_duckingRules.clear();

    m_initialized = false;
    RVX_CORE_INFO("AudioMixer shutdown");
}

void AudioMixer::CreateDefaultBuses()
{
    // Create master bus
    m_masterBus = std::make_shared<AudioBusNode>(BusId::Master, "Master");
    m_buses[BusId::Master] = m_masterBus;
    m_busNameToId["Master"] = BusId::Master;

    // Create default child buses
    auto musicBus = std::make_shared<AudioBusNode>(BusId::Music, "Music");
    auto sfxBus = std::make_shared<AudioBusNode>(BusId::SFX, "SFX");
    auto voiceBus = std::make_shared<AudioBusNode>(BusId::Voice, "Voice");
    auto ambientBus = std::make_shared<AudioBusNode>(BusId::Ambient, "Ambient");
    auto uiBus = std::make_shared<AudioBusNode>(BusId::UI, "UI");

    m_masterBus->AddChild(musicBus);
    m_masterBus->AddChild(sfxBus);
    m_masterBus->AddChild(voiceBus);
    m_masterBus->AddChild(ambientBus);
    m_masterBus->AddChild(uiBus);

    m_buses[BusId::Music] = musicBus;
    m_buses[BusId::SFX] = sfxBus;
    m_buses[BusId::Voice] = voiceBus;
    m_buses[BusId::Ambient] = ambientBus;
    m_buses[BusId::UI] = uiBus;

    m_busNameToId["Music"] = BusId::Music;
    m_busNameToId["SFX"] = BusId::SFX;
    m_busNameToId["Voice"] = BusId::Voice;
    m_busNameToId["Ambient"] = BusId::Ambient;
    m_busNameToId["UI"] = BusId::UI;

    RVX_CORE_INFO("Created default audio bus hierarchy");
}

uint32 AudioMixer::CreateBus(const std::string& name, uint32 parentId)
{
    // Check if name already exists
    if (m_busNameToId.find(name) != m_busNameToId.end())
    {
        RVX_CORE_WARN("Bus '{}' already exists", name);
        return m_busNameToId[name];
    }

    // Find parent bus
    auto parentIt = m_buses.find(parentId);
    if (parentIt == m_buses.end())
    {
        RVX_CORE_ERROR("Parent bus {} not found", parentId);
        return BusId::Master;
    }

    // Create new bus
    uint32 newId = m_nextBusId++;
    auto newBus = std::make_shared<AudioBusNode>(newId, name);

    parentIt->second->AddChild(newBus);

    m_buses[newId] = newBus;
    m_busNameToId[name] = newId;

    RVX_CORE_INFO("Created bus '{}' (id: {}) under parent {}", name, newId, parentId);

    return newId;
}

AudioBusNode* AudioMixer::GetBus(uint32 id)
{
    auto it = m_buses.find(id);
    return (it != m_buses.end()) ? it->second.get() : nullptr;
}

const AudioBusNode* AudioMixer::GetBus(uint32 id) const
{
    auto it = m_buses.find(id);
    return (it != m_buses.end()) ? it->second.get() : nullptr;
}

AudioBusNode* AudioMixer::GetBus(const std::string& name)
{
    auto it = m_busNameToId.find(name);
    if (it == m_busNameToId.end()) return nullptr;
    return GetBus(it->second);
}

void AudioMixer::SetMasterVolume(float volume)
{
    if (m_masterBus)
    {
        m_masterBus->SetVolume(volume);
    }
}

float AudioMixer::GetMasterVolume() const
{
    return m_masterBus ? m_masterBus->GetVolume() : 1.0f;
}

void AudioMixer::SetMusicVolume(float volume)
{
    if (auto* bus = GetBus(BusId::Music))
    {
        bus->SetVolume(volume);
    }
}

float AudioMixer::GetMusicVolume() const
{
    auto* bus = GetBus(BusId::Music);
    return bus ? bus->GetVolume() : 1.0f;
}

void AudioMixer::SetSFXVolume(float volume)
{
    if (auto* bus = GetBus(BusId::SFX))
    {
        bus->SetVolume(volume);
    }
}

float AudioMixer::GetSFXVolume() const
{
    auto* bus = GetBus(BusId::SFX);
    return bus ? bus->GetVolume() : 1.0f;
}

void AudioMixer::SetVoiceVolume(float volume)
{
    if (auto* bus = GetBus(BusId::Voice))
    {
        bus->SetVolume(volume);
    }
}

float AudioMixer::GetVoiceVolume() const
{
    auto* bus = GetBus(BusId::Voice);
    return bus ? bus->GetVolume() : 1.0f;
}

void AudioMixer::SetAmbientVolume(float volume)
{
    if (auto* bus = GetBus(BusId::Ambient))
    {
        bus->SetVolume(volume);
    }
}

float AudioMixer::GetAmbientVolume() const
{
    auto* bus = GetBus(BusId::Ambient);
    return bus ? bus->GetVolume() : 1.0f;
}

void AudioMixer::SetUIVolume(float volume)
{
    if (auto* bus = GetBus(BusId::UI))
    {
        bus->SetVolume(volume);
    }
}

float AudioMixer::GetUIVolume() const
{
    auto* bus = GetBus(BusId::UI);
    return bus ? bus->GetVolume() : 1.0f;
}

void AudioMixer::Update(float deltaTime, const Vec3& listenerPosition)
{
    // Update voice pool
    m_voicePool.Update(deltaTime, listenerPosition);

    // Update ducking
    UpdateDucking(deltaTime);
}

void AudioMixer::SaveSnapshot(const std::string& name)
{
    MixerSnapshot snapshot;

    for (const auto& pair : m_buses)
    {
        snapshot.busVolumes[pair.first] = pair.second->GetVolume();
        snapshot.busMuted[pair.first] = pair.second->IsMuted();
    }

    m_snapshots[name] = snapshot;
    RVX_CORE_INFO("Saved mixer snapshot '{}'", name);
}

void AudioMixer::BlendToSnapshot(const std::string& name, float duration)
{
    auto it = m_snapshots.find(name);
    if (it == m_snapshots.end())
    {
        RVX_CORE_WARN("Snapshot '{}' not found", name);
        return;
    }

    const auto& snapshot = it->second;

    // For now, immediate transition
    // TODO: Implement smooth blending over duration
    (void)duration;

    for (const auto& volPair : snapshot.busVolumes)
    {
        if (auto* bus = GetBus(volPair.first))
        {
            bus->SetVolume(volPair.second);
        }
    }

    for (const auto& mutePair : snapshot.busMuted)
    {
        if (auto* bus = GetBus(mutePair.first))
        {
            bus->SetMuted(mutePair.second);
        }
    }

    RVX_CORE_INFO("Blended to snapshot '{}'", name);
}

void AudioMixer::SetDucking(uint32 sourceBus, uint32 targetBus,
                            float duckAmount, float attackTime, float releaseTime)
{
    DuckingRule rule;
    rule.sourceBus = sourceBus;
    rule.targetBus = targetBus;
    rule.duckAmount = duckAmount;
    rule.attackTime = attackTime;
    rule.releaseTime = releaseTime;
    rule.currentDuck = 1.0f;

    m_duckingRules.push_back(rule);
    RVX_CORE_INFO("Set ducking: bus {} ducks bus {} to {}", sourceBus, targetBus, duckAmount);
}

void AudioMixer::UpdateDucking(float deltaTime)
{
    // TODO: Implement ducking based on active sounds in source bus
    // For now, this is a placeholder
    (void)deltaTime;
}

} // namespace RVX::Audio
