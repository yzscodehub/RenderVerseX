/**
 * @file AudioEvent.cpp
 * @brief AudioEvent implementation
 */

#include "Audio/Events/AudioEvent.h"
#include <algorithm>
#include <chrono>

namespace RVX::Audio
{

AudioEvent::AudioEvent(const AudioEventDesc& desc)
    : m_desc(desc)
{
    // Seed random generator
    auto seed = static_cast<unsigned int>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    m_rng.seed(seed);

    // Initialize shuffle order if needed
    if (desc.selectionMode == ClipSelectionMode::Shuffle)
    {
        InitShuffleOrder();
    }
}

bool AudioEvent::CanPlay() const
{
    // Check cooldown
    if (m_cooldownRemaining > 0.0f)
    {
        return false;
    }

    // Check instance limit
    if (m_desc.maxInstances > 0 && 
        static_cast<int>(m_activeHandles.size()) >= m_desc.maxInstances)
    {
        return false;
    }

    // Check if we have clips
    if (m_desc.clips.empty())
    {
        return false;
    }

    return true;
}

AudioClip::Ptr AudioEvent::GetNextClip()
{
    if (m_desc.clips.empty())
    {
        return nullptr;
    }

    if (m_desc.clips.size() == 1)
    {
        return m_desc.clips[0].clip;
    }

    int selectedIndex = 0;

    switch (m_desc.selectionMode)
    {
        case ClipSelectionMode::Random:
        {
            std::uniform_int_distribution<int> dist(0, static_cast<int>(m_desc.clips.size()) - 1);
            selectedIndex = dist(m_rng);
            break;
        }

        case ClipSelectionMode::Sequential:
        {
            selectedIndex = m_sequentialIndex;
            m_sequentialIndex = (m_sequentialIndex + 1) % static_cast<int>(m_desc.clips.size());
            break;
        }

        case ClipSelectionMode::Shuffle:
        {
            if (m_shuffleOrder.empty())
            {
                InitShuffleOrder();
            }
            selectedIndex = m_shuffleOrder[m_shuffleIndex];
            m_shuffleIndex++;
            if (m_shuffleIndex >= static_cast<int>(m_shuffleOrder.size()))
            {
                m_shuffleIndex = 0;
                InitShuffleOrder();  // Reshuffle for next cycle
            }
            break;
        }

        case ClipSelectionMode::Weighted:
        {
            // Calculate total weight
            float totalWeight = 0.0f;
            for (const auto& entry : m_desc.clips)
            {
                totalWeight += entry.weight;
            }

            // Pick random value
            std::uniform_real_distribution<float> dist(0.0f, totalWeight);
            float pick = dist(m_rng);

            // Find the clip
            float accumulated = 0.0f;
            for (size_t i = 0; i < m_desc.clips.size(); ++i)
            {
                accumulated += m_desc.clips[i].weight;
                if (pick <= accumulated)
                {
                    selectedIndex = static_cast<int>(i);
                    break;
                }
            }
            break;
        }
    }

    return m_desc.clips[selectedIndex].clip;
}

AudioPlaySettings AudioEvent::GenerateSettings() const
{
    AudioPlaySettings settings;

    // Randomize volume
    if (m_desc.volumeMin != m_desc.volumeMax)
    {
        std::uniform_real_distribution<float> dist(m_desc.volumeMin, m_desc.volumeMax);
        settings.volume = dist(m_rng);
    }
    else
    {
        settings.volume = m_desc.volumeMin;
    }

    // Randomize pitch
    if (m_desc.pitchMin != m_desc.pitchMax)
    {
        std::uniform_real_distribution<float> dist(m_desc.pitchMin, m_desc.pitchMax);
        settings.pitch = dist(m_rng);
    }
    else
    {
        settings.pitch = m_desc.pitchMin;
    }

    settings.loop = m_desc.loop;
    settings.fadeInTime = m_desc.fadeInTime;

    return settings;
}

void AudioEvent::RecordPlay(AudioHandle handle)
{
    if (handle.IsValid())
    {
        m_activeHandles.push_back(handle);
        m_cooldownRemaining = m_desc.cooldown;
    }
}

void AudioEvent::RemoveInstance(AudioHandle handle)
{
    m_activeHandles.erase(
        std::remove(m_activeHandles.begin(), m_activeHandles.end(), handle),
        m_activeHandles.end()
    );
}

void AudioEvent::Update(float deltaTime)
{
    // Update cooldown
    if (m_cooldownRemaining > 0.0f)
    {
        m_cooldownRemaining -= deltaTime;
        if (m_cooldownRemaining < 0.0f)
        {
            m_cooldownRemaining = 0.0f;
        }
    }
}

void AudioEvent::InitShuffleOrder()
{
    m_shuffleOrder.resize(m_desc.clips.size());
    for (size_t i = 0; i < m_desc.clips.size(); ++i)
    {
        m_shuffleOrder[i] = static_cast<int>(i);
    }
    
    // Fisher-Yates shuffle
    for (size_t i = m_shuffleOrder.size() - 1; i > 0; --i)
    {
        std::uniform_int_distribution<size_t> dist(0, i);
        size_t j = dist(m_rng);
        std::swap(m_shuffleOrder[i], m_shuffleOrder[j]);
    }
    
    m_shuffleIndex = 0;
}

} // namespace RVX::Audio
