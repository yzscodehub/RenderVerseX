#pragma once

/**
 * @file SkyboxComponent.h
 * @brief Skybox component for environment rendering
 * 
 * SkyboxComponent provides skybox/environment map rendering
 * for scene backgrounds and ambient lighting.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/TextureResource.h"

namespace RVX
{

/**
 * @brief Skybox type
 */
enum class SkyboxType : uint8_t
{
    Cubemap = 0,        // 6-face cubemap texture
    Equirectangular,    // Single HDR panorama
    Procedural,         // Procedural sky (gradient, atmosphere)
    Color               // Solid color
};

/**
 * @brief Skybox component for environment rendering
 * 
 * Features:
 * - Cubemap and equirectangular HDR support
 * - Procedural sky with sun direction
 * - Solid color fallback
 * - Exposure and rotation controls
 * - IBL (Image-Based Lighting) support
 * 
 * Usage:
 * @code
 * auto* sky = scene->GetRoot()->AddComponent<SkyboxComponent>();
 * sky->SetSkyboxType(SkyboxType::Cubemap);
 * sky->SetCubemap(hdriTexture);
 * sky->SetExposure(1.2f);
 * @endcode
 */
class SkyboxComponent : public Component
{
public:
    SkyboxComponent() = default;
    ~SkyboxComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "Skybox"; }

    void OnAttach() override;
    void OnDetach() override;

    // =========================================================================
    // Skybox Type
    // =========================================================================

    SkyboxType GetSkyboxType() const { return m_type; }
    void SetSkyboxType(SkyboxType type) { m_type = type; }

    // =========================================================================
    // Texture Settings
    // =========================================================================

    /// Set cubemap texture (6-face)
    void SetCubemap(Resource::ResourceHandle<Resource::TextureResource> texture);
    Resource::ResourceHandle<Resource::TextureResource> GetCubemap() const { return m_cubemap; }

    /// Set equirectangular HDR texture
    void SetEquirectangular(Resource::ResourceHandle<Resource::TextureResource> texture);
    Resource::ResourceHandle<Resource::TextureResource> GetEquirectangular() const { return m_equirectangular; }

    // =========================================================================
    // Rendering Settings
    // =========================================================================

    /// Exposure multiplier
    float GetExposure() const { return m_exposure; }
    void SetExposure(float exposure) { m_exposure = exposure; }

    /// Rotation around Y axis (radians)
    float GetRotation() const { return m_rotation; }
    void SetRotation(float rotation) { m_rotation = rotation; }

    /// Blur level (for glossy reflections)
    float GetBlurLevel() const { return m_blurLevel; }
    void SetBlurLevel(float level) { m_blurLevel = level; }

    // =========================================================================
    // Solid Color Mode
    // =========================================================================

    const Vec3& GetSolidColor() const { return m_solidColor; }
    void SetSolidColor(const Vec3& color) { m_solidColor = color; }

    // =========================================================================
    // Procedural Sky Settings
    // =========================================================================

    /// Sun direction (normalized)
    const Vec3& GetSunDirection() const { return m_sunDirection; }
    void SetSunDirection(const Vec3& direction) { m_sunDirection = normalize(direction); }

    /// Sun color and intensity
    const Vec3& GetSunColor() const { return m_sunColor; }
    void SetSunColor(const Vec3& color) { m_sunColor = color; }

    /// Atmosphere settings
    const Vec3& GetZenithColor() const { return m_zenithColor; }
    void SetZenithColor(const Vec3& color) { m_zenithColor = color; }

    const Vec3& GetHorizonColor() const { return m_horizonColor; }
    void SetHorizonColor(const Vec3& color) { m_horizonColor = color; }

    const Vec3& GetGroundColor() const { return m_groundColor; }
    void SetGroundColor(const Vec3& color) { m_groundColor = color; }

    /// Atmospheric scattering intensity
    float GetScatteringIntensity() const { return m_scatteringIntensity; }
    void SetScatteringIntensity(float intensity) { m_scatteringIntensity = intensity; }

    // =========================================================================
    // IBL (Image-Based Lighting)
    // =========================================================================

    /// Whether this skybox contributes to ambient lighting
    bool ContributesToLighting() const { return m_contributesToLighting; }
    void SetContributesToLighting(bool contributes) { m_contributesToLighting = contributes; }

    /// Get prefiltered environment map for reflections
    Resource::ResourceHandle<Resource::TextureResource> GetPrefilteredMap() const { return m_prefilteredMap; }
    void SetPrefilteredMap(Resource::ResourceHandle<Resource::TextureResource> texture);

    /// Get irradiance map for diffuse lighting
    Resource::ResourceHandle<Resource::TextureResource> GetIrradianceMap() const { return m_irradianceMap; }
    void SetIrradianceMap(Resource::ResourceHandle<Resource::TextureResource> texture);

    /// Get BRDF LUT texture
    Resource::ResourceHandle<Resource::TextureResource> GetBRDFLUT() const { return m_brdfLUT; }
    void SetBRDFLUT(Resource::ResourceHandle<Resource::TextureResource> texture);

private:
    SkyboxType m_type = SkyboxType::Color;

    // Textures
    Resource::ResourceHandle<Resource::TextureResource> m_cubemap;
    Resource::ResourceHandle<Resource::TextureResource> m_equirectangular;

    // IBL textures
    Resource::ResourceHandle<Resource::TextureResource> m_prefilteredMap;
    Resource::ResourceHandle<Resource::TextureResource> m_irradianceMap;
    Resource::ResourceHandle<Resource::TextureResource> m_brdfLUT;

    // Rendering settings
    float m_exposure = 1.0f;
    float m_rotation = 0.0f;
    float m_blurLevel = 0.0f;

    // Solid color
    Vec3 m_solidColor{0.1f, 0.1f, 0.15f};

    // Procedural sky
    Vec3 m_sunDirection{0.0f, 1.0f, 0.0f};
    Vec3 m_sunColor{1.0f, 0.95f, 0.9f};
    Vec3 m_zenithColor{0.2f, 0.4f, 0.8f};
    Vec3 m_horizonColor{0.7f, 0.8f, 0.9f};
    Vec3 m_groundColor{0.3f, 0.25f, 0.2f};
    float m_scatteringIntensity = 1.0f;

    // Lighting
    bool m_contributesToLighting = true;
};

} // namespace RVX
