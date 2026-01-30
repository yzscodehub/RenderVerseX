/**
 * @file EffectChain.cpp
 * @brief Effect chain implementation
 */

#include "Audio/DSP/EffectChain.h"
#include <algorithm>

namespace RVX::Audio
{

void EffectChain::AddEffect(std::shared_ptr<IAudioEffect> effect)
{
    if (effect)
    {
        m_effects.push_back(std::move(effect));
    }
}

void EffectChain::InsertEffect(size_t index, std::shared_ptr<IAudioEffect> effect)
{
    if (effect)
    {
        if (index >= m_effects.size())
        {
            m_effects.push_back(std::move(effect));
        }
        else
        {
            m_effects.insert(m_effects.begin() + static_cast<ptrdiff_t>(index), std::move(effect));
        }
    }
}

void EffectChain::RemoveEffect(size_t index)
{
    if (index < m_effects.size())
    {
        m_effects.erase(m_effects.begin() + static_cast<ptrdiff_t>(index));
    }
}

void EffectChain::RemoveEffect(IAudioEffect* effect)
{
    m_effects.erase(
        std::remove_if(m_effects.begin(), m_effects.end(),
            [effect](const std::shared_ptr<IAudioEffect>& e) { return e.get() == effect; }),
        m_effects.end()
    );
}

void EffectChain::Clear()
{
    m_effects.clear();
}

IAudioEffect* EffectChain::GetEffect(size_t index)
{
    return (index < m_effects.size()) ? m_effects[index].get() : nullptr;
}

const IAudioEffect* EffectChain::GetEffect(size_t index) const
{
    return (index < m_effects.size()) ? m_effects[index].get() : nullptr;
}

void EffectChain::Process(float* buffer, uint32 frameCount, uint32 channels)
{
    if (!m_enabled || m_bypassed || m_effects.empty())
    {
        return;
    }

    for (auto& effect : m_effects)
    {
        if (effect && effect->IsEnabled())
        {
            effect->Process(buffer, frameCount, channels);
        }
    }
}

void EffectChain::Reset()
{
    for (auto& effect : m_effects)
    {
        if (effect)
        {
            effect->Reset();
        }
    }
}

} // namespace RVX::Audio
