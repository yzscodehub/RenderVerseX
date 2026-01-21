#pragma once

/**
 * @file LightComponent.h
 * @brief Component for lights (directional, point, spot)
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"

namespace RVX
{
    /**
     * @brief Light type enumeration
     */
    enum class LightType : uint8_t
    {
        Directional = 0,
        Point,
        Spot
    };

    /**
     * @brief Component for scene lighting
     * 
     * Features:
     * - Directional, Point, and Spot light support
     * - Color, intensity, and range configuration
     * - Shadow casting support
     * - Provides bounds for point/spot lights
     * 
     * Usage:
     * @code
     * auto* entity = scene->CreateEntity("Sun");
     * auto* light = entity->AddComponent<LightComponent>();
     * light->SetLightType(LightType::Directional);
     * light->SetColor(Vec3(1.0f, 0.9f, 0.8f));
     * light->SetIntensity(1.5f);
     * @endcode
     */
    class LightComponent : public Component
    {
    public:
        LightComponent() = default;
        ~LightComponent() override = default;

        // =====================================================================
        // Component Interface
        // =====================================================================

        const char* GetTypeName() const override { return "Light"; }

        void OnAttach() override;
        void OnDetach() override;

        // =====================================================================
        // Spatial Bounds (only for point/spot lights)
        // =====================================================================

        bool ProvidesBounds() const override;
        AABB GetLocalBounds() const override;

        // =====================================================================
        // Light Type
        // =====================================================================

        LightType GetLightType() const { return m_type; }
        void SetLightType(LightType type);

        // =====================================================================
        // Light Properties
        // =====================================================================

        const Vec3& GetColor() const { return m_color; }
        void SetColor(const Vec3& color) { m_color = color; }

        float GetIntensity() const { return m_intensity; }
        void SetIntensity(float intensity) { m_intensity = intensity; }

        /// Range for point/spot lights
        float GetRange() const { return m_range; }
        void SetRange(float range);

        // =====================================================================
        // Spot Light Properties
        // =====================================================================

        /// Inner cone angle in radians
        float GetInnerConeAngle() const { return m_innerConeAngle; }
        void SetInnerConeAngle(float angle) { m_innerConeAngle = angle; }

        /// Outer cone angle in radians
        float GetOuterConeAngle() const { return m_outerConeAngle; }
        void SetOuterConeAngle(float angle) { m_outerConeAngle = angle; }

        // =====================================================================
        // Shadow Properties
        // =====================================================================

        bool CastsShadow() const { return m_castsShadow; }
        void SetCastsShadow(bool casts) { m_castsShadow = casts; }

        float GetShadowBias() const { return m_shadowBias; }
        void SetShadowBias(float bias) { m_shadowBias = bias; }

    private:
        LightType m_type = LightType::Directional;
        Vec3 m_color{1.0f, 1.0f, 1.0f};
        float m_intensity = 1.0f;
        float m_range = 10.0f;
        float m_innerConeAngle = 0.0f;
        float m_outerConeAngle = 0.7854f;  // ~45 degrees
        bool m_castsShadow = false;
        float m_shadowBias = 0.001f;
    };

} // namespace RVX
