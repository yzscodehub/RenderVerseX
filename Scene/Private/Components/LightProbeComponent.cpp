#include "Scene/Components/LightProbeComponent.h"
#include "Scene/SceneEntity.h"
#include <cmath>

namespace RVX
{

// =========================================================================
// SphericalHarmonicsL2 Implementation
// =========================================================================

namespace
{
    // SH basis functions (L0, L1, L2)
    constexpr float SH_C0 = 0.282095f;  // 1 / (2 * sqrt(pi))
    constexpr float SH_C1 = 0.488603f;  // sqrt(3) / (2 * sqrt(pi))
    constexpr float SH_C2_0 = 1.092548f;  // sqrt(15) / (2 * sqrt(pi))
    constexpr float SH_C2_1 = 0.315392f;  // sqrt(5) / (4 * sqrt(pi))
    constexpr float SH_C2_2 = 0.546274f;  // sqrt(15) / (4 * sqrt(pi))
}

Vec3 SphericalHarmonicsL2::GetDominantDirection() const
{
    // L1 band encodes directional information
    // coefficients[1] = Y, coefficients[2] = Z, coefficients[3] = X
    return normalize(Vec3(
        coefficients[3].x + coefficients[3].y + coefficients[3].z,
        coefficients[1].x + coefficients[1].y + coefficients[1].z,
        coefficients[2].x + coefficients[2].y + coefficients[2].z
    ));
}

void SphericalHarmonicsL2::AddAmbientLight(const Vec3& color)
{
    // L0 band (constant term)
    coefficients[0] += color * SH_C0;
}

void SphericalHarmonicsL2::AddDirectionalLight(const Vec3& direction, const Vec3& color)
{
    // Normalize direction
    Vec3 d = normalize(direction);

    // L0
    coefficients[0] += color * SH_C0;

    // L1
    coefficients[1] += color * (SH_C1 * d.y);
    coefficients[2] += color * (SH_C1 * d.z);
    coefficients[3] += color * (SH_C1 * d.x);

    // L2
    coefficients[4] += color * (SH_C2_0 * d.x * d.y);
    coefficients[5] += color * (SH_C2_0 * d.y * d.z);
    coefficients[6] += color * (SH_C2_1 * (3.0f * d.z * d.z - 1.0f));
    coefficients[7] += color * (SH_C2_0 * d.x * d.z);
    coefficients[8] += color * (SH_C2_2 * (d.x * d.x - d.y * d.y));
}

Vec3 SphericalHarmonicsL2::Sample(const Vec3& direction) const
{
    Vec3 d = normalize(direction);

    Vec3 result(0.0f);

    // L0
    result += coefficients[0] * SH_C0;

    // L1
    result += coefficients[1] * (SH_C1 * d.y);
    result += coefficients[2] * (SH_C1 * d.z);
    result += coefficients[3] * (SH_C1 * d.x);

    // L2
    result += coefficients[4] * (SH_C2_0 * d.x * d.y);
    result += coefficients[5] * (SH_C2_0 * d.y * d.z);
    result += coefficients[6] * (SH_C2_1 * (3.0f * d.z * d.z - 1.0f));
    result += coefficients[7] * (SH_C2_0 * d.x * d.z);
    result += coefficients[8] * (SH_C2_2 * (d.x * d.x - d.y * d.y));

    return max(result, Vec3(0.0f));
}

std::array<Vec4, 7> SphericalHarmonicsL2::GetShaderData() const
{
    // Pack SH coefficients for shader use
    // Format compatible with Unity's SH representation
    std::array<Vec4, 7> data;

    // RGB linear coefficients (for each band)
    data[0] = Vec4(coefficients[0].x, coefficients[1].x, coefficients[2].x, coefficients[3].x);
    data[1] = Vec4(coefficients[0].y, coefficients[1].y, coefficients[2].y, coefficients[3].y);
    data[2] = Vec4(coefficients[0].z, coefficients[1].z, coefficients[2].z, coefficients[3].z);

    // L2 coefficients
    data[3] = Vec4(coefficients[4].x, coefficients[5].x, coefficients[6].x, coefficients[7].x);
    data[4] = Vec4(coefficients[4].y, coefficients[5].y, coefficients[6].y, coefficients[7].y);
    data[5] = Vec4(coefficients[4].z, coefficients[5].z, coefficients[6].z, coefficients[7].z);
    data[6] = Vec4(coefficients[8].x, coefficients[8].y, coefficients[8].z, 0.0f);

    return data;
}

void SphericalHarmonicsL2::Clear()
{
    for (auto& c : coefficients)
    {
        c = Vec3(0.0f);
    }
}

void SphericalHarmonicsL2::Blend(const SphericalHarmonicsL2& other, float t)
{
    for (size_t i = 0; i < coefficients.size(); ++i)
    {
        coefficients[i] = mix(coefficients[i], other.coefficients[i], t);
    }
}

// =========================================================================
// LightProbeComponent Implementation
// =========================================================================

void LightProbeComponent::OnAttach()
{
    // Initialize with default ambient
    if (!m_hasValidData)
    {
        m_sh.AddAmbientLight(Vec3(0.2f));
    }
}

void LightProbeComponent::OnDetach()
{
    // Nothing special needed
}

Vec3 LightProbeComponent::SampleDirection(const Vec3& direction) const
{
    return m_sh.Sample(direction);
}

Vec3 LightProbeComponent::GetAverageColor() const
{
    // L0 band represents average light
    return m_sh.coefficients[0] / SH_C0;
}

void LightProbeComponent::Bake()
{
    if (m_isBaking)
    {
        return;
    }

    m_isBaking = true;

    // TODO: Trigger SH capture through render system
    // This would typically:
    // 1. Render a cubemap at probe position
    // 2. Integrate cubemap faces into SH coefficients
    // 3. Store result in m_sh

    // For now, create a simple default lighting setup
    m_sh.Clear();
    m_sh.AddAmbientLight(Vec3(0.1f, 0.1f, 0.15f));
    m_sh.AddDirectionalLight(Vec3(0.0f, 1.0f, 0.5f), Vec3(0.5f, 0.45f, 0.4f));

    m_hasValidData = true;
    m_isBaking = false;
}

} // namespace RVX
