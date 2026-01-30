#pragma once

/**
 * @file DecalComponent.h
 * @brief Decal projection component
 * 
 * DecalComponent projects textures onto scene geometry
 * for effects like bullet holes, blood splatter, graffiti, etc.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/MaterialResource.h"

namespace RVX
{

/**
 * @brief Decal blend mode
 */
enum class DecalBlendMode : uint8_t
{
    Default = 0,    // Normal blending (albedo + normal)
    Stain,          // Multiply blend (stains, dirt)
    Emissive,       // Additive blend (lights, glow)
    Normal          // Normal map only (surface detail)
};

/**
 * @brief Decal component for projecting textures onto geometry
 * 
 * Features:
 * - Box projection onto scene geometry
 * - PBR material support (albedo, normal, roughness)
 * - Fade based on angle and distance
 * - Layer masking
 * - Draw order control
 * 
 * Usage:
 * @code
 * auto* entity = scene->CreateEntity("BulletHole");
 * auto* decal = entity->AddComponent<DecalComponent>();
 * decal->SetMaterial(bulletHoleMaterial);
 * decal->SetSize(Vec3(0.2f, 0.2f, 0.1f));
 * @endcode
 */
class DecalComponent : public Component
{
public:
    DecalComponent() = default;
    ~DecalComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "Decal"; }

    void OnAttach() override;
    void OnDetach() override;

    // =========================================================================
    // Spatial Bounds
    // =========================================================================

    bool ProvidesBounds() const override { return true; }
    AABB GetLocalBounds() const override;

    // =========================================================================
    // Material
    // =========================================================================

    /// Set decal material
    void SetMaterial(Resource::ResourceHandle<Resource::MaterialResource> material);
    Resource::ResourceHandle<Resource::MaterialResource> GetMaterial() const { return m_material; }

    // =========================================================================
    // Projection Size
    // =========================================================================

    /// Size of projection box (half extents)
    const Vec3& GetSize() const { return m_size; }
    void SetSize(const Vec3& size);

    /// Depth of projection (Z component of size)
    float GetProjectionDepth() const { return m_size.z; }
    void SetProjectionDepth(float depth);

    // =========================================================================
    // Rendering Settings
    // =========================================================================

    /// Blend mode
    DecalBlendMode GetBlendMode() const { return m_blendMode; }
    void SetBlendMode(DecalBlendMode mode) { m_blendMode = mode; }

    /// Opacity
    float GetOpacity() const { return m_opacity; }
    void SetOpacity(float opacity) { m_opacity = clamp(opacity, 0.0f, 1.0f); }

    /// Albedo tint color
    const Vec4& GetColor() const { return m_color; }
    void SetColor(const Vec4& color) { m_color = color; }

    /// Normal map strength
    float GetNormalStrength() const { return m_normalStrength; }
    void SetNormalStrength(float strength) { m_normalStrength = strength; }

    // =========================================================================
    // Fade Settings
    // =========================================================================

    /// Fade based on projection angle
    float GetAngleFade() const { return m_angleFade; }
    void SetAngleFade(float fade) { m_angleFade = clamp(fade, 0.0f, 1.0f); }

    /// Distance at which decal starts fading
    float GetFadeDistance() const { return m_fadeDistance; }
    void SetFadeDistance(float distance) { m_fadeDistance = distance; }

    /// Fade width (distance over which fade occurs)
    float GetFadeWidth() const { return m_fadeWidth; }
    void SetFadeWidth(float width) { m_fadeWidth = width; }

    // =========================================================================
    // Layer Masking
    // =========================================================================

    /// Layers this decal projects onto
    uint32_t GetDecalMask() const { return m_decalMask; }
    void SetDecalMask(uint32_t mask) { m_decalMask = mask; }

    /// Sort order (higher = rendered later)
    int GetSortOrder() const { return m_sortOrder; }
    void SetSortOrder(int order) { m_sortOrder = order; }

    // =========================================================================
    // Projection Matrix
    // =========================================================================

    /// Get the decal projection matrix (for shader)
    Mat4 GetProjectionMatrix() const;

    /// Get the inverse projection matrix
    Mat4 GetInverseProjectionMatrix() const;

private:
    Resource::ResourceHandle<Resource::MaterialResource> m_material;

    // Size
    Vec3 m_size{0.5f, 0.5f, 0.25f};

    // Rendering
    DecalBlendMode m_blendMode = DecalBlendMode::Default;
    float m_opacity = 1.0f;
    Vec4 m_color{1.0f, 1.0f, 1.0f, 1.0f};
    float m_normalStrength = 1.0f;

    // Fade
    float m_angleFade = 0.5f;
    float m_fadeDistance = 10.0f;
    float m_fadeWidth = 2.0f;

    // Masking
    uint32_t m_decalMask = ~0u;
    int m_sortOrder = 0;
};

} // namespace RVX
