#pragma once

/**
 * @file LightProbeComponent.h
 * @brief Light probe component for indirect lighting
 * 
 * LightProbeComponent captures spherical harmonics lighting
 * for diffuse global illumination on dynamic objects.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"
#include <array>

namespace RVX
{

/**
 * @brief Spherical Harmonics coefficients (L2, 9 coefficients per channel)
 */
struct SphericalHarmonicsL2
{
    // 9 coefficients for RGB (3 channels x 9 = 27 floats)
    std::array<Vec3, 9> coefficients;

    SphericalHarmonicsL2()
    {
        for (auto& c : coefficients)
        {
            c = Vec3(0.0f);
        }
    }

    /// Get dominant light direction from SH
    Vec3 GetDominantDirection() const;

    /// Add ambient light
    void AddAmbientLight(const Vec3& color);

    /// Add directional light
    void AddDirectionalLight(const Vec3& direction, const Vec3& color);

    /// Sample SH in a direction
    Vec3 Sample(const Vec3& direction) const;

    /// Evaluate for rendering (returns coefficients packed for shader)
    std::array<Vec4, 7> GetShaderData() const;

    /// Clear all coefficients
    void Clear();

    /// Blend with another SH
    void Blend(const SphericalHarmonicsL2& other, float t);
};

/**
 * @brief Light probe mode
 */
enum class LightProbeMode : uint8_t
{
    Baked = 0,      // Pre-baked (static)
    Realtime,       // Runtime capture
    Custom          // User-provided SH
};

/**
 * @brief Light probe component for diffuse global illumination
 * 
 * Features:
 * - Spherical Harmonics L2 lighting
 * - Baked and realtime modes
 * - Probe interpolation support
 * - Light probe group integration
 * 
 * Usage:
 * @code
 * auto* entity = scene->CreateEntity("Probe");
 * auto* probe = entity->AddComponent<LightProbeComponent>();
 * probe->SetMode(LightProbeMode::Baked);
 * probe->Bake();
 * 
 * // Query lighting at a position
 * SphericalHarmonicsL2 sh = probeSystem->SampleAt(position);
 * @endcode
 */
class LightProbeComponent : public Component
{
public:
    LightProbeComponent() = default;
    ~LightProbeComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "LightProbe"; }

    void OnAttach() override;
    void OnDetach() override;

    // =========================================================================
    // Probe Mode
    // =========================================================================

    LightProbeMode GetMode() const { return m_mode; }
    void SetMode(LightProbeMode mode) { m_mode = mode; }

    // =========================================================================
    // Spherical Harmonics Data
    // =========================================================================

    /// Get the SH coefficients
    const SphericalHarmonicsL2& GetSH() const { return m_sh; }
    SphericalHarmonicsL2& GetSH() { return m_sh; }

    /// Set SH coefficients (for custom mode)
    void SetSH(const SphericalHarmonicsL2& sh) { m_sh = sh; }

    /// Sample lighting in a direction
    Vec3 SampleDirection(const Vec3& direction) const;

    /// Get average ambient color
    Vec3 GetAverageColor() const;

    // =========================================================================
    // Capture Settings
    // =========================================================================

    /// Layer mask for capture
    uint32_t GetCullingMask() const { return m_cullingMask; }
    void SetCullingMask(uint32_t mask) { m_cullingMask = mask; }

    /// Near/far planes for capture
    float GetNearClip() const { return m_nearClip; }
    void SetNearClip(float near) { m_nearClip = near; }

    float GetFarClip() const { return m_farClip; }
    void SetFarClip(float far) { m_farClip = far; }

    // =========================================================================
    // Probe Groups
    // =========================================================================

    /// Group ID for interpolation (probes in same group blend together)
    int GetGroupID() const { return m_groupID; }
    void SetGroupID(int id) { m_groupID = id; }

    // =========================================================================
    // Capture API
    // =========================================================================

    /// Bake the probe
    void Bake();

    /// Is baking in progress?
    bool IsBaking() const { return m_isBaking; }

    /// Has valid data
    bool HasValidData() const { return m_hasValidData; }

private:
    LightProbeMode m_mode = LightProbeMode::Baked;
    SphericalHarmonicsL2 m_sh;

    // Capture settings
    uint32_t m_cullingMask = ~0u;
    float m_nearClip = 0.1f;
    float m_farClip = 100.0f;

    // Group
    int m_groupID = 0;

    // State
    bool m_isBaking = false;
    bool m_hasValidData = false;
};

} // namespace RVX
