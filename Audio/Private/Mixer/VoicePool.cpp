/**
 * @file VoicePool.cpp
 * @brief VoicePool implementation
 */

#include "Audio/Mixer/VoicePool.h"
#include "Core/Log.h"
#include <algorithm>
#include <cmath>

namespace RVX::Audio
{

bool VoicePool::Initialize(const VoicePoolConfig& config)
{
    m_config = config;
    
    // Allocate voice slots
    m_voices.resize(config.maxVoices);
    for (auto& voice : m_voices)
    {
        voice.state = VoiceState::Free;
    }

    if (config.enableVirtualization)
    {
        m_virtualVoices.resize(config.virtualVoiceCount);
        for (auto& voice : m_virtualVoices)
        {
            voice.state = VoiceState::Free;
        }
    }

    m_activeCount = 0;
    m_virtualCount = 0;

    RVX_CORE_INFO("VoicePool initialized with {} voices ({} virtual)",
        config.maxVoices, config.virtualVoiceCount);

    return true;
}

void VoicePool::Shutdown()
{
    m_voices.clear();
    m_virtualVoices.clear();
    m_activeCount = 0;
    m_virtualCount = 0;

    RVX_CORE_INFO("VoicePool shutdown");
}

AudioHandle VoicePool::RequestVoice(VoicePriority priority)
{
    int32 slotIndex = FindFreeVoice(priority);
    if (slotIndex < 0)
    {
        RVX_CORE_DEBUG("No available voice slot for priority {}", static_cast<int>(priority));
        return AudioHandle();
    }

    uint64 handleId = m_nextHandleId++;
    
    auto& voice = m_voices[slotIndex];
    voice.handleId = handleId;
    voice.state = VoiceState::Playing;
    voice.priority = priority;
    voice.volume = 1.0f;
    voice.audibility = 1.0f;
    voice.is3D = false;
    voice.position = Vec3(0.0f);
    voice.clip = nullptr;

    m_activeCount++;

    return AudioHandle(handleId);
}

void VoicePool::ReleaseVoice(AudioHandle handle)
{
    if (!handle.IsValid()) return;

    for (auto& voice : m_voices)
    {
        if (voice.handleId == handle.GetId())
        {
            voice.state = VoiceState::Free;
            voice.handleId = 0;
            voice.clip = nullptr;
            m_activeCount--;
            return;
        }
    }

    // Check virtual voices
    for (auto& voice : m_virtualVoices)
    {
        if (voice.handleId == handle.GetId())
        {
            voice.state = VoiceState::Free;
            voice.handleId = 0;
            voice.clip = nullptr;
            m_virtualCount--;
            return;
        }
    }
}

const VoiceInfo* VoicePool::GetVoiceInfo(AudioHandle handle) const
{
    if (!handle.IsValid()) return nullptr;

    for (const auto& voice : m_voices)
    {
        if (voice.handleId == handle.GetId())
        {
            return &voice;
        }
    }

    for (const auto& voice : m_virtualVoices)
    {
        if (voice.handleId == handle.GetId())
        {
            return &voice;
        }
    }

    return nullptr;
}

void VoicePool::UpdateVoice(AudioHandle handle, const VoiceInfo& info)
{
    if (!handle.IsValid()) return;

    for (auto& voice : m_voices)
    {
        if (voice.handleId == handle.GetId())
        {
            voice.volume = info.volume;
            voice.is3D = info.is3D;
            voice.position = info.position;
            voice.priority = info.priority;
            return;
        }
    }

    for (auto& voice : m_virtualVoices)
    {
        if (voice.handleId == handle.GetId())
        {
            voice.volume = info.volume;
            voice.is3D = info.is3D;
            voice.position = info.position;
            voice.priority = info.priority;
            return;
        }
    }
}

void VoicePool::Update(float deltaTime, const Vec3& listenerPosition)
{
    (void)deltaTime;

    // Update audibility for all voices
    for (auto& voice : m_voices)
    {
        if (voice.state == VoiceState::Playing || voice.state == VoiceState::Paused)
        {
            voice.audibility = CalculateAudibility(voice, listenerPosition);
        }
    }

    // Virtualize low-audibility voices
    if (m_config.enableVirtualization)
    {
        VirtualizeVoices(listenerPosition);
        DevirtualizeVoices(listenerPosition);
    }
}

bool VoicePool::HasAvailableVoice(VoicePriority priority) const
{
    // Check for free voices
    for (const auto& voice : m_voices)
    {
        if (voice.state == VoiceState::Free)
        {
            return true;
        }
    }

    // Check if we can steal a lower priority voice
    if (m_config.stealMode != VoiceStealMode::None)
    {
        for (const auto& voice : m_voices)
        {
            if (voice.priority < priority)
            {
                return true;
            }
        }
    }

    return false;
}

int32 VoicePool::FindFreeVoice(VoicePriority priority)
{
    // First, look for a completely free voice
    for (size_t i = 0; i < m_voices.size(); ++i)
    {
        if (m_voices[i].state == VoiceState::Free)
        {
            return static_cast<int32>(i);
        }
    }

    // No free voice, try to steal one based on steal mode
    if (m_config.stealMode == VoiceStealMode::None)
    {
        return -1;
    }

    int32 candidateIndex = -1;
    float candidateScore = 0.0f;

    for (size_t i = 0; i < m_voices.size(); ++i)
    {
        const auto& voice = m_voices[i];

        // Can only steal lower priority voices
        if (voice.priority >= priority)
        {
            continue;
        }

        float score = 0.0f;

        switch (m_config.stealMode)
        {
            case VoiceStealMode::LowestPriority:
                score = 255.0f - static_cast<float>(voice.priority);
                break;

            case VoiceStealMode::QuietestFirst:
                score = 1.0f - voice.audibility;
                break;

            case VoiceStealMode::OldestFirst:
                // Use handle ID as a proxy for age (lower = older)
                score = 1.0f / static_cast<float>(voice.handleId);
                break;

            default:
                break;
        }

        if (candidateIndex < 0 || score > candidateScore)
        {
            candidateIndex = static_cast<int32>(i);
            candidateScore = score;
        }
    }

    if (candidateIndex >= 0)
    {
        // Stop the voice we're stealing
        m_voices[candidateIndex].state = VoiceState::Free;
        m_voices[candidateIndex].handleId = 0;
        m_voices[candidateIndex].clip = nullptr;
        m_activeCount--;
        
        RVX_CORE_DEBUG("Stole voice {} for new sound", candidateIndex);
    }

    return candidateIndex;
}

float VoicePool::CalculateAudibility(const VoiceInfo& voice, const Vec3& listenerPosition) const
{
    if (!voice.is3D)
    {
        // 2D sounds are always fully audible (based on volume)
        return voice.volume;
    }

    // Calculate distance-based audibility
    Vec3 diff = voice.position - listenerPosition;
    float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

    // Simple inverse distance attenuation
    // Real implementation would use the voice's attenuation settings
    constexpr float minDist = 1.0f;
    constexpr float maxDist = 100.0f;

    if (distance <= minDist)
    {
        return voice.volume;
    }
    else if (distance >= maxDist)
    {
        return 0.0f;
    }

    float attenuation = minDist / distance;
    return voice.volume * attenuation;
}

void VoicePool::VirtualizeVoices(const Vec3& listenerPosition)
{
    if (!m_config.enableVirtualization) return;

    for (auto& voice : m_voices)
    {
        if (voice.state != VoiceState::Playing) continue;

        if (voice.audibility < m_config.virtualizationThreshold)
        {
            // Find a free virtual slot
            for (auto& virtualVoice : m_virtualVoices)
            {
                if (virtualVoice.state == VoiceState::Free)
                {
                    // Move to virtual
                    virtualVoice = voice;
                    virtualVoice.state = VoiceState::Virtual;
                    
                    voice.state = VoiceState::Free;
                    voice.handleId = 0;
                    voice.clip = nullptr;
                    
                    m_activeCount--;
                    m_virtualCount++;

                    RVX_CORE_DEBUG("Virtualized voice (audibility: {:.3f})", virtualVoice.audibility);
                    break;
                }
            }
        }
    }
}

void VoicePool::DevirtualizeVoices(const Vec3& listenerPosition)
{
    if (!m_config.enableVirtualization) return;

    for (auto& virtualVoice : m_virtualVoices)
    {
        if (virtualVoice.state != VoiceState::Virtual) continue;

        // Recalculate audibility
        virtualVoice.audibility = CalculateAudibility(virtualVoice, listenerPosition);

        if (virtualVoice.audibility >= m_config.virtualizationThreshold * 2.0f)
        {
            // Try to find a real voice slot
            int32 slot = FindFreeVoice(virtualVoice.priority);
            if (slot >= 0)
            {
                m_voices[slot] = virtualVoice;
                m_voices[slot].state = VoiceState::Playing;

                virtualVoice.state = VoiceState::Free;
                virtualVoice.handleId = 0;
                virtualVoice.clip = nullptr;

                m_activeCount++;
                m_virtualCount--;

                RVX_CORE_DEBUG("Devirtualized voice (audibility: {:.3f})", m_voices[slot].audibility);
            }
        }
    }
}

} // namespace RVX::Audio
