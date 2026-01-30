#pragma once

/**
 * @file ReflectionProbeComponent.h
 * @brief Reflection probe component for local reflections
 * 
 * ReflectionProbeComponent captures environment reflections
 * for accurate local reflections in PBR rendering.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/TextureResource.h"

namespace RVX
{

/**
 * @brief Reflection probe mode
 */
enum class ReflectionProbeMode : uint8_t
{
    Baked = 0,      // Pre-baked cubemap (static)
    Realtime,       // Runtime capture (expensive)
    Custom          // User-provided cubemap
};

/**
 * @brief Reflection probe shape for influence
 */
enum class ReflectionProbeShape : uint8_t
{
    Box = 0,
    Sphere
};

/**
 * @brief Reflection probe refresh mode
 */
enum class ReflectionProbeRefresh : uint8_t
{
    OnAwake = 0,        // Capture once on start
    EveryFrame,         // Capture every frame
    ViaScripting        // Manual refresh via API
};

/**
 * @brief Reflection probe component for local environment reflections
 * 
 * Features:
 * - Box and sphere influence volumes
 * - Box projection correction
 * - Baked and realtime modes
 * - Priority-based blending
 * - HDR capture support
 * 
 * Usage:
 * @code
 * auto* entity = scene->CreateEntity("RoomProbe");
 * auto* probe = entity->AddComponent<ReflectionProbeComponent>();
 * probe->SetShape(ReflectionProbeShape::Box);
 * probe->SetSize(Vec3(10.0f, 5.0f, 10.0f));
 * probe->SetMode(ReflectionProbeMode::Baked);
 * probe->Bake();
 * @endcode
 */
class ReflectionProbeComponent : public Component
{
public:
    ReflectionProbeComponent() = default;
    ~ReflectionProbeComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "ReflectionProbe"; }

    void OnAttach() override;
    void OnDetach() override;

    // =========================================================================
    // Spatial Bounds
    // =========================================================================

    bool ProvidesBounds() const override { return true; }
    AABB GetLocalBounds() const override;

    // =========================================================================
    // Probe Mode
    // =========================================================================

    ReflectionProbeMode GetMode() const { return m_mode; }
    void SetMode(ReflectionProbeMode mode) { m_mode = mode; }

    // =========================================================================
    // Influence Volume
    // =========================================================================

    ReflectionProbeShape GetShape() const { return m_shape; }
    void SetShape(ReflectionProbeShape shape);

    /// Size for box shape (half extents) or radius for sphere (in x component)
    const Vec3& GetSize() const { return m_size; }
    void SetSize(const Vec3& size);

    /// Blend distance at volume edges
    float GetBlendDistance() const { return m_blendDistance; }
    void SetBlendDistance(float distance) { m_blendDistance = distance; }

    // =========================================================================
    // Box Projection
    // =========================================================================

    /// Enable box projection correction
    bool UseBoxProjection() const { return m_useBoxProjection; }
    void SetUseBoxProjection(bool use) { m_useBoxProjection = use; }

    /// Box projection bounds (usually same as influence size)
    const Vec3& GetBoxProjectionSize() const { return m_boxProjectionSize; }
    void SetBoxProjectionSize(const Vec3& size) { m_boxProjectionSize = size; }

    /// Box projection center offset
    const Vec3& GetBoxProjectionOffset() const { return m_boxProjectionOffset; }
    void SetBoxProjectionOffset(const Vec3& offset) { m_boxProjectionOffset = offset; }

    // =========================================================================
    // Capture Settings
    // =========================================================================

    /// Resolution of captured cubemap
    int GetResolution() const { return m_resolution; }
    void SetResolution(int resolution) { m_resolution = resolution; }

    /// Use HDR capture
    bool UseHDR() const { return m_useHDR; }
    void SetUseHDR(bool hdr) { m_useHDR = hdr; }

    /// Clipping planes
    float GetNearClip() const { return m_nearClip; }
    void SetNearClip(float near) { m_nearClip = near; }

    float GetFarClip() const { return m_farClip; }
    void SetFarClip(float far) { m_farClip = far; }

    /// Layer mask for capture
    uint32_t GetCullingMask() const { return m_cullingMask; }
    void SetCullingMask(uint32_t mask) { m_cullingMask = mask; }

    /// Background mode
    bool ClearBackground() const { return m_clearBackground; }
    void SetClearBackground(bool clear) { m_clearBackground = clear; }

    const Vec4& GetBackgroundColor() const { return m_backgroundColor; }
    void SetBackgroundColor(const Vec4& color) { m_backgroundColor = color; }

    // =========================================================================
    // Realtime Settings
    // =========================================================================

    ReflectionProbeRefresh GetRefreshMode() const { return m_refreshMode; }
    void SetRefreshMode(ReflectionProbeRefresh mode) { m_refreshMode = mode; }

    /// Time slicing for realtime (frames per face)
    int GetTimeSlicing() const { return m_timeSlicing; }
    void SetTimeSlicing(int frames) { m_timeSlicing = frames; }

    // =========================================================================
    // Priority
    // =========================================================================

    /// Priority for blending (higher = more important)
    int GetImportance() const { return m_importance; }
    void SetImportance(int importance) { m_importance = importance; }

    // =========================================================================
    // Cubemap Access
    // =========================================================================

    /// Get the captured/assigned cubemap
    Resource::ResourceHandle<Resource::TextureResource> GetCubemap() const { return m_cubemap; }
    void SetCubemap(Resource::ResourceHandle<Resource::TextureResource> cubemap);

    /// Check if probe has valid data
    bool HasValidCubemap() const;

    // =========================================================================
    // Capture API
    // =========================================================================

    /// Bake the probe (for Baked/Custom modes)
    void Bake();

    /// Request render for realtime mode
    void RequestRender();

    /// Is baking in progress?
    bool IsBaking() const { return m_isBaking; }

private:
    ReflectionProbeMode m_mode = ReflectionProbeMode::Baked;
    ReflectionProbeShape m_shape = ReflectionProbeShape::Box;
    ReflectionProbeRefresh m_refreshMode = ReflectionProbeRefresh::OnAwake;

    // Influence volume
    Vec3 m_size{5.0f};
    float m_blendDistance = 1.0f;

    // Box projection
    bool m_useBoxProjection = true;
    Vec3 m_boxProjectionSize{5.0f};
    Vec3 m_boxProjectionOffset{0.0f};

    // Capture settings
    int m_resolution = 256;
    bool m_useHDR = true;
    float m_nearClip = 0.1f;
    float m_farClip = 100.0f;
    uint32_t m_cullingMask = ~0u;
    bool m_clearBackground = true;
    Vec4 m_backgroundColor{0.0f, 0.0f, 0.0f, 1.0f};

    // Realtime settings
    int m_timeSlicing = 0;  // 0 = no slicing

    // Priority
    int m_importance = 0;

    // Cubemap
    Resource::ResourceHandle<Resource::TextureResource> m_cubemap;
    bool m_isBaking = false;
};

} // namespace RVX
