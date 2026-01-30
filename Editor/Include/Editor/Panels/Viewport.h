/**
 * @file Viewport.h
 * @brief Scene viewport panel
 */

#pragma once

#include "Editor/Panels/IEditorPanel.h"
#include "Core/MathTypes.h"
#include <memory>

namespace RVX
{
    class RHITexture;
}

namespace RVX::Editor
{

/**
 * @brief Camera control mode
 */
enum class ViewportCameraMode : uint8
{
    Orbit,      ///< Orbit around focus point
    Fly,        ///< WASD + mouse fly camera
    TopDown     ///< Top-down orthographic
};

/**
 * @brief Viewport shading mode
 */
enum class ViewportShadingMode : uint8
{
    Lit,
    Unlit,
    Wireframe,
    Normals,
    Depth,
    Albedo
};

/**
 * @brief Viewport panel for 3D scene viewing
 */
class ViewportPanel : public IEditorPanel
{
public:
    ViewportPanel();
    ~ViewportPanel() override = default;

    // =========================================================================
    // IEditorPanel Interface
    // =========================================================================

    const char* GetName() const override { return "Viewport"; }
    const char* GetIcon() const override { return "viewport"; }
    void OnInit() override;
    void OnUpdate(float deltaTime) override;
    void OnGUI() override;

    // =========================================================================
    // Camera Control
    // =========================================================================

    void SetCameraMode(ViewportCameraMode mode) { m_cameraMode = mode; }
    ViewportCameraMode GetCameraMode() const { return m_cameraMode; }

    void FocusOnSelection();
    void ResetCamera();

    void SetCameraPosition(const Vec3& position) { m_cameraPosition = position; }
    const Vec3& GetCameraPosition() const { return m_cameraPosition; }

    void SetCameraTarget(const Vec3& target) { m_cameraTarget = target; }
    const Vec3& GetCameraTarget() const { return m_cameraTarget; }

    // =========================================================================
    // Display Options
    // =========================================================================

    void SetShowGrid(bool show) { m_showGrid = show; }
    bool GetShowGrid() const { return m_showGrid; }

    void SetShowGizmo(bool show) { m_showGizmo = show; }
    bool GetShowGizmo() const { return m_showGizmo; }

    void SetShadingMode(ViewportShadingMode mode) { m_shadingMode = mode; }
    ViewportShadingMode GetShadingMode() const { return m_shadingMode; }

private:
    void UpdateCamera(float deltaTime);
    void UpdateOrbitCamera(float deltaTime);
    void UpdateFlyCamera(float deltaTime);
    
    void DrawToolbar();
    void DrawGizmo();
    void DrawViewportContent();
    void DrawOverlays();
    
    void HandleInput();
    void HandleMousePick();

    Mat4 GetViewMatrix() const;
    Mat4 GetProjectionMatrix() const;

    // Render target
    std::shared_ptr<RHITexture> m_renderTarget;
    uint32 m_viewportWidth = 1280;
    uint32 m_viewportHeight = 720;
    Vec2 m_viewportPos{0.0f, 0.0f};

    // Camera
    Vec3 m_cameraPosition{0.0f, 5.0f, 10.0f};
    Vec3 m_cameraTarget{0.0f, 0.0f, 0.0f};
    Vec3 m_cameraUp{0.0f, 1.0f, 0.0f};
    float m_cameraYaw = -90.0f;
    float m_cameraPitch = -15.0f;
    float m_cameraDistance = 10.0f;
    float m_cameraMoveSpeed = 10.0f;
    float m_cameraRotateSpeed = 0.3f;
    float m_cameraFOV = 60.0f;
    float m_cameraNear = 0.1f;
    float m_cameraFar = 1000.0f;

    ViewportCameraMode m_cameraMode = ViewportCameraMode::Orbit;
    ViewportShadingMode m_shadingMode = ViewportShadingMode::Lit;
    
    bool m_isNavigating = false;
    bool m_isHovered = false;
    bool m_isFocused = false;

    // Input state
    Vec2 m_lastMousePos{0.0f, 0.0f};
    bool m_rightMouseDown = false;
    bool m_middleMouseDown = false;

    // Display options
    bool m_showGrid = true;
    bool m_showStats = true;
    bool m_showGizmo = true;
    bool m_showBounds = false;
    bool m_showDebugDraw = true;
};

} // namespace RVX::Editor
