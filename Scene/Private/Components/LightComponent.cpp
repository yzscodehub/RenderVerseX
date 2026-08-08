#include "Scene/Components/LightComponent.h"
#include "Scene/SceneEntity.h"

namespace RVX
{

void LightComponent::OnAttach()
{
    NotifyBoundsChanged();
}

void LightComponent::OnDetach()
{
    // Nothing special needed
}

bool LightComponent::ProvidesBounds() const
{
    // Only point and spot lights have spatial bounds
    // Directional lights are infinite
    return m_type != LightType::Directional;
}

AABB LightComponent::GetLocalBounds() const
{
    if (m_type == LightType::Directional)
    {
        // Directional lights have no bounds
        return AABB();
    }

    // Point light: sphere with radius = range
    // Spot light: approximated as sphere (could be more precise with cone)
    Vec3 extent(m_range, m_range, m_range);
    return AABB(-extent, extent);
}

void LightComponent::SetLightType(LightType type)
{
    if (m_type != type)
    {
        m_type = type;
        NotifyBoundsChanged();
    }
}

void LightComponent::SetRange(float range)
{
    if (m_range != range)
    {
        m_range = range;
        NotifyBoundsChanged();
    }
}

void LightComponent::SetColor(const Vec3& color)
{
    if (m_color == color)
        return;
    m_color = color;
    NotifySceneStateChanged();
}

void LightComponent::SetIntensity(float intensity)
{
    if (m_intensity == intensity)
        return;
    m_intensity = intensity;
    NotifySceneStateChanged();
}

void LightComponent::SetInnerConeAngle(float angle)
{
    if (m_innerConeAngle == angle)
        return;
    m_innerConeAngle = angle;
    NotifySceneStateChanged();
}

void LightComponent::SetOuterConeAngle(float angle)
{
    if (m_outerConeAngle == angle)
        return;
    m_outerConeAngle = angle;
    NotifySceneStateChanged();
}

void LightComponent::SetCastsShadow(bool casts)
{
    if (m_castsShadow == casts)
        return;
    m_castsShadow = casts;
    NotifySceneStateChanged();
}

void LightComponent::SetShadowBias(float bias)
{
    if (m_shadowBias == bias)
        return;
    m_shadowBias = bias;
    NotifySceneStateChanged();
}

} // namespace RVX
