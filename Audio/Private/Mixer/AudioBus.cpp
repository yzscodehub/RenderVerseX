/**
 * @file AudioBus.cpp
 * @brief AudioBusNode implementation
 */

#include "Audio/Mixer/AudioBus.h"
#include <algorithm>

namespace RVX::Audio
{

AudioBusNode::AudioBusNode(uint32 id, const std::string& name)
    : m_id(id)
    , m_name(name)
{
}

void AudioBusNode::SetVolume(float volume)
{
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

float AudioBusNode::GetEffectiveVolume() const
{
    float effective = m_muted ? 0.0f : m_volume;
    
    if (m_parent)
    {
        effective *= m_parent->GetEffectiveVolume();
    }

    return effective;
}

void AudioBusNode::SetMuted(bool muted)
{
    m_muted = muted;
}

void AudioBusNode::SetSolo(bool solo)
{
    m_solo = solo;
    
    // When solo is enabled, mute all siblings
    if (solo && m_parent)
    {
        for (auto& sibling : m_parent->GetChildren())
        {
            if (sibling.get() != this)
            {
                sibling->SetMuted(true);
            }
        }
    }
}

void AudioBusNode::AddChild(Ptr child)
{
    child->SetParent(this);
    m_children.push_back(std::move(child));
}

void AudioBusNode::RemoveChild(uint32 childId)
{
    m_children.erase(
        std::remove_if(m_children.begin(), m_children.end(),
            [childId](const Ptr& child) { return child->GetId() == childId; }),
        m_children.end()
    );
}

void AudioBusNode::AddEffect(std::shared_ptr<IAudioEffect> effect)
{
    m_effects.push_back(std::move(effect));
}

void AudioBusNode::RemoveEffect(size_t index)
{
    if (index < m_effects.size())
    {
        m_effects.erase(m_effects.begin() + static_cast<ptrdiff_t>(index));
    }
}

void AudioBusNode::SetSend(uint32 targetBusId, float amount)
{
    m_sends[targetBusId] = std::clamp(amount, 0.0f, 1.0f);
}

float AudioBusNode::GetSend(uint32 targetBusId) const
{
    auto it = m_sends.find(targetBusId);
    return (it != m_sends.end()) ? it->second : 0.0f;
}

} // namespace RVX::Audio
