/**
 * @file Underwater.cpp
 * @brief Implementation of underwater post-processing state.
 */

#include "Water/Underwater.h"
#include "Core/Log.h"

namespace RVX
{

bool Underwater::Initialize(const UnderwaterDesc& desc)
{
    m_quality = desc.quality;
    m_properties = desc.properties;
    m_time = 0.0f;
    m_isUnderwater = false;

    RVX_CORE_INFO("Underwater: Initialized feature state at {} quality",
                  static_cast<int>(m_quality));
    return true;
}

void Underwater::Update(float deltaTime)
{
    if (m_quality == UnderwaterQuality::Off)
        return;

    m_time += deltaTime * m_properties.distortionSpeed;
}

void Underwater::SetProperties(const UnderwaterProperties& props)
{
    m_properties = props;
}

void Underwater::SetQuality(UnderwaterQuality quality)
{
    m_quality = quality;
}

} // namespace RVX
