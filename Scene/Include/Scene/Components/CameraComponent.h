#pragma once

/**
 * @file CameraComponent.h
 * @brief Camera component for scene entities
 * 
 * CameraComponent provides camera functionality including projection,
 * view matrix computation, and camera controls.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"

namespace RVX
{

/**
 * @brief Camera projection type
 */
enum class ProjectionType : uint8_t
{
    Perspective = 0,
    Orthographic
};

/**
 * @brief Camera clear mode
 */
enum class CameraClearMode : uint8_t
{
    Skybox = 0,     // Clear with skybox
    SolidColor,     // Clear with solid color
    DepthOnly,      // Clear depth only
    Nothing         // Don't clear
};

/**
 * @brief Camera component for scene entities
 * 
 * Features:
 * - Perspective and orthographic projection
 * - View/projection matrix computation
 * - Clear mode and background color
 * - Layer mask for selective rendering
 * - Priority for multi-camera rendering
 * 
 * Usage:
 * @code
 * auto* entity = scene->CreateEntity("MainCamera");
 * auto* camera = entity->AddComponent<CameraComponent>();
 * camera->SetProjectionType(ProjectionType::Perspective);
 * camera->SetFieldOfView(glm::radians(60.0f));
 * camera->SetNearPlane(0.1f);
 * camera->SetFarPlane(1000.0f);
 * @endcode
 */
class CameraComponent : public Component
{
public:
    CameraComponent() = default;
    ~CameraComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "Camera"; }

    void OnAttach() override;
    void OnDetach() override;

    // =========================================================================
    // Projection Settings
    // =========================================================================

    /// Get projection type
    ProjectionType GetProjectionType() const { return m_projectionType; }
    void SetProjectionType(ProjectionType type);

    /// Field of view in radians (perspective only)
    float GetFieldOfView() const { return m_fieldOfView; }
    void SetFieldOfView(float fov) { m_fieldOfView = fov; m_projectionDirty = true; }

    /// Orthographic size (half-height, orthographic only)
    float GetOrthographicSize() const { return m_orthoSize; }
    void SetOrthographicSize(float size) { m_orthoSize = size; m_projectionDirty = true; }

    /// Near clipping plane
    float GetNearPlane() const { return m_nearPlane; }
    void SetNearPlane(float near) { m_nearPlane = near; m_projectionDirty = true; }

    /// Far clipping plane
    float GetFarPlane() const { return m_farPlane; }
    void SetFarPlane(float far) { m_farPlane = far; m_projectionDirty = true; }

    /// Aspect ratio (width / height)
    float GetAspectRatio() const { return m_aspectRatio; }
    void SetAspectRatio(float aspect) { m_aspectRatio = aspect; m_projectionDirty = true; }

    // =========================================================================
    // Clear Settings
    // =========================================================================

    CameraClearMode GetClearMode() const { return m_clearMode; }
    void SetClearMode(CameraClearMode mode) { m_clearMode = mode; }

    const Vec4& GetBackgroundColor() const { return m_backgroundColor; }
    void SetBackgroundColor(const Vec4& color) { m_backgroundColor = color; }

    // =========================================================================
    // Rendering Settings
    // =========================================================================

    /// Camera priority (higher = rendered later)
    int32_t GetPriority() const { return m_priority; }
    void SetPriority(int32_t priority) { m_priority = priority; }

    /// Layer mask for selective rendering
    uint32_t GetCullingMask() const { return m_cullingMask; }
    void SetCullingMask(uint32_t mask) { m_cullingMask = mask; }

    /// Viewport rectangle (normalized 0-1)
    const Vec4& GetViewport() const { return m_viewport; }
    void SetViewport(const Vec4& viewport) { m_viewport = viewport; }
    void SetViewport(float x, float y, float width, float height) 
    { 
        m_viewport = Vec4(x, y, width, height); 
    }

    // =========================================================================
    // Matrix Computation
    // =========================================================================

    /// Get view matrix (computed from owner's transform)
    Mat4 GetViewMatrix() const;

    /// Get projection matrix
    Mat4 GetProjectionMatrix() const;

    /// Get view-projection matrix
    Mat4 GetViewProjectionMatrix() const;

    /// Get inverse view matrix
    Mat4 GetInverseViewMatrix() const;

    /// Mark projection as needing recalculation
    void MarkProjectionDirty() { m_projectionDirty = true; }

    // =========================================================================
    // Utility Functions
    // =========================================================================

    /// Convert screen position to world ray
    /// @param screenPos Normalized screen coordinates (0-1)
    /// @return Ray origin and direction
    void ScreenToWorldRay(const Vec2& screenPos, Vec3& outOrigin, Vec3& outDirection) const;

    /// Convert world position to screen position
    /// @param worldPos World position
    /// @return Normalized screen coordinates (0-1), z = depth
    Vec3 WorldToScreenPoint(const Vec3& worldPos) const;

    /// Get camera forward direction (world space)
    Vec3 GetForward() const;

    /// Get camera right direction (world space)
    Vec3 GetRight() const;

    /// Get camera up direction (world space)
    Vec3 GetUp() const;

private:
    void UpdateProjectionMatrix() const;

    // Projection settings
    ProjectionType m_projectionType = ProjectionType::Perspective;
    float m_fieldOfView = 1.0472f;  // 60 degrees
    float m_orthoSize = 5.0f;
    float m_nearPlane = 0.1f;
    float m_farPlane = 1000.0f;
    float m_aspectRatio = 16.0f / 9.0f;

    // Clear settings
    CameraClearMode m_clearMode = CameraClearMode::Skybox;
    Vec4 m_backgroundColor{0.0f, 0.0f, 0.0f, 1.0f};

    // Rendering settings
    int32_t m_priority = 0;
    uint32_t m_cullingMask = ~0u;  // Render all layers by default
    Vec4 m_viewport{0.0f, 0.0f, 1.0f, 1.0f};  // Full screen

    // Cached matrices
    mutable Mat4 m_projectionMatrix{1.0f};
    mutable bool m_projectionDirty = true;
};

} // namespace RVX
