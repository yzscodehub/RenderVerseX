/**
 * @file Caustics.cpp
 * @brief Implementation of underwater caustics state.
 */

#include "Water/Caustics.h"
#include "Water/WaterSimulation.h"
#include "Core/Log.h"

namespace RVX
{

bool Caustics::Initialize(const CausticsDesc& desc)
{
    m_quality = desc.quality;
    m_textureSize = desc.textureSize;
    m_intensity = desc.intensity;
    m_scale = desc.scale;
    m_speed = desc.speed;
    m_maxDepth = desc.maxDepth;
    m_focusFalloff = desc.focusFalloff;
    m_time = 0.0f;

    RVX_CORE_INFO("Caustics: Initialized feature state at {} quality, {}x{} target texture",
                  static_cast<int>(m_quality), m_textureSize, m_textureSize);
    return true;
}

void Caustics::Update(float deltaTime, const WaterSimulation* simulation)
{
    (void)simulation;

    if (m_quality == CausticsQuality::Off)
        return;

    m_time += deltaTime * m_speed;
}

} // namespace RVX
