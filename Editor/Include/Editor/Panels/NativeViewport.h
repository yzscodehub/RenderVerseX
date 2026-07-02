/**
 * @file NativeViewport.h
 * @brief Native editor viewport panel
 */

#pragma once

#include "Editor/Panels/Viewport.h"
#include "Editor/UI/EditorUIPanel.h"
#include "Editor/UI/EditorViewportInteractionModel.h"
#include "Editor/UI/EditorViewportManipulatorModel.h"
#include "Editor/UI/EditorViewportNavigationModel.h"
#include "Editor/UI/EditorViewportSelectionOverlayModel.h"
#include "Editor/UI/EditorViewportSelectionModel.h"
#include "Editor/UI/EditorViewportToolModel.h"

#include <string>

namespace RVX
{
    class IRHIDevice;
}

namespace RVX::Editor
{

class EditorSelectionService;
class EditorViewportToolService;

struct NativeViewportPanelBuildStats
{
    bool built = false;
    bool imageBuilt = false;
    bool fallbackBuilt = false;
    bool navigationHUDBuilt = false;
    uint32 toolbarButtonCount = 0;
    uint32 toolbarIconButtonCount = 0;
    uint32 toolbarTextButtonCount = 0;
    uint32 toolbarGroupSeparatorCount = 0;
    bool toolbarStatusBuilt = false;
    std::string toolbarStatusText;
    uint32 gridLineCount = 0;
    uint32 navigationHUDAxisCount = 0;
    UI::Rect contentBounds;
    UI::Rect toolbarStatusBounds;
    UI::Rect navigationHUDBounds;
    EditorViewportToolModelStats toolStats;
    EditorViewportInteractionStats interactionStats;
    EditorViewportNavigationStats navigationStats;
    EditorViewportManipulatorStats manipulatorStats;
    EditorViewportSelectionStats selectionStats;
    EditorViewportSelectionOverlayStats selectionOverlayStats;
    ViewportRenderTargetState renderTargetState;
};

/**
 * @brief Native UI shell for the editor viewport.
 */
class NativeViewportPanel final : public IEditorUIPanel
{
public:
    NativeViewportPanel();

    static const char* PanelId() { return "native.viewport"; }

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void OnUpdate(float deltaTime) override;
    void BuildUI(EditorUIPanelFrameContext& context) override;
    EditorUICursorRequest GetCursorRequest() const override;

    void SetRenderDevice(IRHIDevice* device);
    void SetSelectionService(EditorSelectionService* service)
    {
        m_selectionService = service;
    }
    void SetToolService(EditorViewportToolService* service)
    {
        m_toolService = service;
    }
    ViewportPanel& GetViewportCore() { return m_viewport; }
    const ViewportPanel& GetViewportCore() const { return m_viewport; }
    const NativeViewportPanelBuildStats& GetLastBuildStats() const { return m_lastBuildStats; }

private:
    void AddToolbar(EditorUIPanelFrameContext& context);
    void AddToolStatusStrip(EditorUIPanelFrameContext& context,
                            const EditorViewportToolState& toolState,
                            float reservedLeft);
    void AddViewportContent(EditorUIPanelFrameContext& context);
    void AddGrid(UI::Panel& viewportRoot,
                 const UI::Rect& localBounds,
                 const UI::UIColor& color);
    void AddCrosshair(UI::Panel& viewportRoot,
                      const UI::Rect& localBounds,
                      const UI::UIColor& color);
    void AddStatsOverlay(UI::Panel& viewportRoot,
                         const UI::Rect& localBounds,
                         const UI::UITheme& theme);
    void AddNavigationHUD(UI::Panel& viewportRoot,
                          const UI::Rect& localBounds,
                          const UI::Rect& globalBounds,
                          const UI::UITheme& theme,
                          const EditorViewportCameraFrame& cameraFrame);
    void AddSelectionOverlay(UI::Panel& viewportRoot,
                             const UI::Rect& globalBounds,
                             const UI::UITheme& theme);
    void AddManipulatorOverlay(UI::Panel& viewportRoot,
                               const UI::Rect& globalBounds,
                               const UI::UITheme& theme);
    EditorViewportToolState CaptureToolState() const;
    const EditorViewportToolModelStats& GetToolStats() const;
    void SetToolMode(EditorContext::GizmoMode mode);
    void SetToolSpace(EditorContext::GizmoSpace space);
    void ToggleToolSnap();
    void ActivateNavigationAxis(EditorViewportNavigationAxis axis);
    const char* GetToolModeLabel(EditorContext::GizmoMode mode) const;
    const char* GetToolSpaceLabel(EditorContext::GizmoSpace space) const;

    EditorUIPanelDesc m_desc;
    ViewportPanel m_viewport;
    EditorViewportToolModel m_toolModel;
    EditorViewportInteractionModel m_interactionModel;
    EditorViewportNavigationModel m_navigationModel;
    EditorSelectionService* m_selectionService = nullptr;
    EditorViewportToolService* m_toolService = nullptr;
    EditorViewportManipulatorModel m_manipulatorModel;
    EditorViewportSelectionModel m_selectionModel;
    EditorViewportSelectionOverlayModel m_selectionOverlayModel;
    NativeViewportPanelBuildStats m_lastBuildStats;
};

} // namespace RVX::Editor
