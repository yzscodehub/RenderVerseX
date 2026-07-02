/**
 * @file Viewport.h
 * @brief Scene viewport panel
 */

#pragma once

#include "Core/MathTypes.h"
#include "Editor/Panels/IEditorPanel.h"
#include "Editor/UI/EditorViewportCameraControllerModel.h"
#include "Editor/UI/EditorViewportProjection.h"
#include "RHI/RHITexture.h"

#include <string>

namespace RVX
{
    class Camera;
    class IRHIDevice;
    class RHICommandContext;
    class RHIDescriptorSetLayout;
    class RHIPipeline;
    class RHITexture;
    class RHITextureView;
    class SceneEntity;

    namespace UI
    {
        class UIRenderer;
    }
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
 * @brief Viewport render backend currently feeding the panel
 */
enum class ViewportRenderBackend : uint8
{
    Placeholder,
    RHI
};

/**
 * @brief Latest RenderGraph diagnostics surfaced to editor viewport UI
 */
struct ViewportRenderGraphDiagnostics
{
    bool graphCompiled = false;
    bool graphCompileValid = true;
    bool graphExecutionSkipped = false;
    uint32 totalPasses = 0;
    uint32 culledPasses = 0;
    uint32 barrierCount = 0;
    uint32 textureBarrierCount = 0;
    uint32 bufferBarrierCount = 0;
    uint32 validationWarningCount = 0;
    uint32 validationErrorCount = 0;
    std::string skippedReason;
    std::string firstDiagnostic;
};

/**
 * @brief Viewport render target allocation state
 */
struct ViewportRenderTargetState
{
    ViewportRenderBackend backend = ViewportRenderBackend::Placeholder;
    bool hasDevice = false;
    bool colorTargetReady = false;
    bool depthTargetReady = false;
    bool lastRenderRecorded = false;
    bool runtimeUIOverlayRecorded = false;
    bool displayImageReady = false;
    uint32 width = 0;
    uint32 height = 0;
    uint32 runtimeUIOverlayDrawCallCount = 0;
    uint64 frameCount = 0;
    uint64 runtimeUIOverlayFrameCount = 0;
    uint64 displayTextureId = 0;
    RHIFormat colorFormat = RHIFormat::Unknown;
    RHIFormat depthFormat = RHIFormat::Unknown;
    RHIResourceState colorState = RHIResourceState::Undefined;
    RHIResourceState depthState = RHIResourceState::Undefined;
    std::string fallbackReason = "RHI device not bound";
    std::string runtimeUIOverlayFallbackReason;
    std::string displayFallbackReason = "RHI device not bound";
    ViewportRenderGraphDiagnostics renderGraphDiagnostics;
};

/**
 * @brief Native UI viewport content state prepared from the reusable viewport core.
 */
struct ViewportNativeContentState
{
    UI::Rect bounds;
    bool sizeValid = false;
    bool rhiReady = false;
    bool renderReadyForDisplay = false;
    bool displayImageReady = false;
    RHITextureView* textureView = nullptr;
    std::string fallbackReason;
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
    void OnNativeInput(const UI::UIInputState& input) override;

    // =========================================================================
    // Camera Control
    // =========================================================================

    void SetCameraMode(ViewportCameraMode mode) { m_cameraMode = mode; }
    ViewportCameraMode GetCameraMode() const { return m_cameraMode; }

    void FocusOnSelection();
    void ResetCamera();
    void AlignCameraToViewDirection(const Vec3& directionFromTarget,
                                    ViewportCameraMode mode);

    void SetCameraPosition(const Vec3& position) { m_cameraPosition = position; }
    const Vec3& GetCameraPosition() const { return m_cameraPosition; }

    void SetCameraTarget(const Vec3& target) { m_cameraTarget = target; }
    const Vec3& GetCameraTarget() const { return m_cameraTarget; }
    const EditorViewportCameraControllerStats& GetCameraControllerStats() const
    {
        return m_cameraController.GetLastStats();
    }

    Camera BuildRenderCamera() const;
    EditorViewportCameraFrame CreateCameraFrame() const;

    // =========================================================================
    // Display Options
    // =========================================================================

    void SetShowGrid(bool show) { m_showGrid = show; }
    bool GetShowGrid() const { return m_showGrid; }

    void SetShowGizmo(bool show) { m_showGizmo = show; }
    bool GetShowGizmo() const { return m_showGizmo; }

    void SetShowStats(bool show) { m_showStats = show; }
    bool GetShowStats() const { return m_showStats; }

    void SetRuntimeUIOverlayEnabled(bool enabled) { m_runtimeUIOverlayEnabled = enabled; }
    bool IsRuntimeUIOverlayEnabled() const { return m_runtimeUIOverlayEnabled; }

    void SetShadingMode(ViewportShadingMode mode) { m_shadingMode = mode; }
    ViewportShadingMode GetShadingMode() const { return m_shadingMode; }

    // =========================================================================
    // RHI Render Target
    // =========================================================================

    void SetRenderDevice(IRHIDevice* device);
    bool EnsureRenderTargetSize(uint32 width, uint32 height);
    bool RenderRHI(RHICommandContext& commandContext);
    bool RenderRuntimeUIOverlay(RHICommandContext& commandContext,
                                UI::UIRenderer& renderer,
                                RHIPipeline* pipeline,
                                RHIDescriptorSetLayout* textureSetLayout);
    void MarkSceneRenderResult(bool recorded,
                               RHIResourceState colorState,
                               RHIResourceState depthState,
                               const std::string& fallbackReason);
    void SetRenderGraphDiagnostics(const ViewportRenderGraphDiagnostics& diagnostics);
    void ClearRenderGraphDiagnostics();
    void ReleaseRenderTargets();

    bool IsRHIRenderTargetReady() const;
    uint64 GetNativeDisplayTextureId() const;
    ViewportNativeContentState PrepareNativeContent(const UI::Rect& bounds);
    const ViewportRenderTargetState& GetRenderTargetState() const { return m_renderTargetState; }

    RHITexture* GetColorTarget() const { return m_renderTarget.Get(); }
    RHITextureView* GetColorTargetView() const { return m_renderTargetView.Get(); }
    RHITextureView* GetColorShaderResourceView() const { return m_renderShaderResourceView.Get(); }
    RHITexture* GetDepthTarget() const { return m_depthTarget.Get(); }
    RHITextureView* GetDepthTargetView() const { return m_depthTargetView.Get(); }

private:
    void UpdateCamera(float deltaTime);
    void UpdateOrbitCamera(float deltaTime);
    void UpdateFlyCamera(float deltaTime);
    EditorViewportCameraControllerState CaptureCameraControllerState() const;
    void ApplyCameraControllerState(const EditorViewportCameraControllerState& state);

    void DrawToolbar();
    void DrawGizmo();
    void BeginGizmoUndo(SceneEntity* entity);
    void CommitGizmoUndo();
    void DrawViewportContent();
    void DrawOverlays();

    void HandleMousePick();
    void UpdateNativePointerScope(const UI::UIInputState& input);

    Mat4 GetViewMatrix() const;
    Mat4 GetProjectionMatrix() const;

    // Render target
    IRHIDevice* m_renderDevice = nullptr;
    RHITextureRef m_renderTarget;
    RHITextureViewRef m_renderTargetView;
    RHITextureViewRef m_renderShaderResourceView;
    RHITextureRef m_depthTarget;
    RHITextureViewRef m_depthTargetView;
    ViewportRenderTargetState m_renderTargetState;
    uint32 m_viewportWidth = 1280;
    uint32 m_viewportHeight = 720;
    Vec2 m_viewportPos{0.0f, 0.0f};
    UI::Rect m_viewportBounds;
    bool m_runtimeUIOverlayEnabled = false;

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
    EditorViewportCameraControllerModel m_cameraController;

    ViewportCameraMode m_cameraMode = ViewportCameraMode::Orbit;
    ViewportShadingMode m_shadingMode = ViewportShadingMode::Lit;

    bool m_isNavigating = false;
    bool m_isHovered = false;
    bool m_isFocused = false;

    // Input state
    UI::UIInputState m_nativeInputState;
    bool m_rightMouseDown = false;
    bool m_middleMouseDown = false;

    // Display options
    bool m_showGrid = true;
    bool m_showStats = true;
    bool m_showGizmo = true;
    bool m_showBounds = false;
    bool m_showDebugDraw = true;

    // Gizmo undo coalescing
    bool m_gizmoUndoActive = false;
    uint32 m_gizmoUndoEntityHandle = ~0u;
    Vec3 m_gizmoUndoStartPosition{0.0f};
    Quat m_gizmoUndoStartRotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 m_gizmoUndoStartScale{1.0f};
};

} // namespace RVX::Editor
