/**
 * @file NativeViewport.cpp
 * @brief Native editor viewport panel implementation
 */

#include "Editor/Panels/NativeViewport.h"
#include "Editor/EditorSelectionService.h"
#include "Editor/EditorViewportToolService.h"
#include "Editor/UI/EditorIconButton.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorVectorIcon.h"
#include "UI/UIRenderer.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Image.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace RVX::Editor
{
namespace
{
    UI::Button::Ptr CreateViewportButton(const std::string& name,
                                         const std::string& text,
                                         const std::string& iconName,
                                         const UI::UITheme& theme,
                                         bool active)
    {
        UI::Button::Ptr button;
        if (!iconName.empty())
        {
            EditorIconButton::Ptr iconButton =
                EditorIconButton::Create(text, iconName);
            EditorIconButtonActionStyleDesc styleDesc;
            styleDesc.active = active;
            styleDesc.iconScale = 0.52f;
            styleDesc.pressedAlpha = 0.65f;
            iconButton->ApplyActionStyle(theme, styleDesc);
            button = iconButton;
        }
        else
        {
            button = UI::Button::Create(text);
            button->SetNormalColor(active ? theme.colors.surfaceActive : theme.colors.surface);
            button->SetHoverColor(theme.colors.surfaceHover);
            button->SetPressedColor(theme.colors.accent.WithAlpha(0.65f));
            button->SetDisabledColor(theme.colors.surface.WithAlpha(0.35f));
        }
        button->SetName(name);
        button->SetTooltipText(text);
        EditorTypography::ApplyToButton(*button,
                                        theme,
                                        EditorTypographyRole::Control);
        button->GetStyle().textColor = active ? theme.colors.text : theme.colors.textMuted;
        return button;
    }

    float EstimateViewportButtonWidth(const std::string& text,
                                      const UI::UITheme& theme,
                                      float minWidth,
                                      float maxWidth = 132.0f)
    {
        const float fontSize = EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::Control);
        const float textWidth = EditorTypography::MeasureTextWidth(
            text,
            theme,
            EditorTypographyRole::Control);
        const float horizontalPadding = std::max(16.0f, fontSize);
        return std::clamp(textWidth + horizontalPadding, minWidth, maxWidth);
    }

    UI::Label::Ptr CreateViewportLabel(const std::string& name,
                                       const std::string& text,
                                       const UI::UITheme& theme,
                                       const UI::UIColor& color)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        EditorTypography::ApplyToLabel(*label,
                                       theme,
                                       EditorTypographyRole::ListItem);
        label->SetTextColor(color);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetInteractive(false);
        return label;
    }

    std::string ShortenStatsText(const std::string& text, size_t maxChars)
    {
        if (text.size() <= maxChars)
        {
            return text;
        }
        if (maxChars <= 3u)
        {
            return text.substr(0u, maxChars);
        }
        return text.substr(0u, maxChars - 3u) + "...";
    }

    UI::Panel::Ptr CreateViewportLine(const std::string& name,
                                      float x,
                                      float y,
                                      float width,
                                      float height,
                                      const UI::UIColor& color)
    {
        UI::Panel::Ptr line = UI::Panel::Create();
        line->SetName(name);
        line->SetPosition(x, y);
        line->SetSize(width, height);
        line->SetBackgroundColor(color);
        line->SetBorderWidth(0.0f);
        line->SetInteractive(false);
        return line;
    }

    UI::Panel::Ptr CreateViewportDot(const std::string& name,
                                     const Vec2& center,
                                     float size,
                                     const UI::UIColor& color)
    {
        UI::Panel::Ptr dot = UI::Panel::Create();
        dot->SetName(name);
        dot->SetPosition(center.x - size * 0.5f,
                         center.y - size * 0.5f);
        dot->SetSize(size, size);
        dot->SetBackgroundColor(color);
        dot->SetBorderWidth(0.0f);
        dot->SetInteractive(false);
        return dot;
    }

    UI::Panel::Ptr CreateToolbarSeparator(const std::string& name,
                                          const UI::UITheme& theme)
    {
        UI::Panel::Ptr separator = UI::Panel::Create();
        separator->SetName(name);
        separator->SetBackgroundColor(theme.colors.border.WithAlpha(0.80f));
        separator->SetBorderWidth(0.0f);
        separator->SetInteractive(false);
        return separator;
    }

    void AddNavigationAxisVisual(UI::Panel& hud,
                                 const std::string& widgetName,
                                 const char* axisLabel,
                                 const Vec2& center,
                                 const Vec2& direction,
                                 float axisLength,
                                 float dotSize,
                                 const UI::UIColor& color,
                                 const UI::UITheme& theme)
    {
        const float directionLength = length(direction);
        if (directionLength <= 0.0001f || axisLength <= 0.0f)
        {
            return;
        }

        const Vec2 normalizedDirection = direction / directionLength;
        const uint32 sampleCount =
            static_cast<uint32>(std::clamp(std::ceil(axisLength / std::max(4.0f, dotSize * 1.35f)),
                                           4.0f,
                                           14.0f));
        for (uint32 sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            const float t = sampleCount <= 1u
                                ? 0.0f
                                : static_cast<float>(sampleIndex + 1u) /
                                      static_cast<float>(sampleCount);
            const Vec2 position = center + normalizedDirection * axisLength * t;
            hud.AddChild(CreateViewportDot(widgetName + ".Dot." +
                                               std::to_string(sampleIndex),
                                           position,
                                           dotSize,
                                           color));
        }

        const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
        const float labelSize =
            std::max(EditorTypography::GetLineHeight(
                         theme,
                         EditorTypographyRole::ListItem),
                     shellMetrics.formRowHeight * 0.48f);
        const Vec2 labelCenter =
            center + normalizedDirection * (axisLength + labelSize * 0.35f);
        UI::Label::Ptr label = CreateViewportLabel(widgetName + ".Label",
                                                   axisLabel,
                                                   theme,
                                                   color);
        label->SetTextAlign(UI::TextAlign::Center);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetPosition(labelCenter.x - labelSize * 0.5f,
                           labelCenter.y - labelSize * 0.5f);
        label->SetSize(labelSize, labelSize);
        hud.AddChild(label);
    }

    std::string NavigationAxisWidgetName(EditorViewportNavigationAxis axis)
    {
        return std::string("NativeViewport.NavigationHUD.Axis") +
               EditorViewportNavigationModel::GetAxisName(axis);
    }

    UI::Panel::Ptr CreateNavigationHitTarget(const std::string& name,
                                             const UI::Rect& rect,
                                             const std::string& tooltip,
                                             UI::EventCallback onClick)
    {
        UI::Panel::Ptr hitTarget = UI::Panel::Create();
        hitTarget->SetName(name);
        hitTarget->SetPosition(rect.x, rect.y);
        hitTarget->SetSize(rect.width, rect.height);
        hitTarget->SetBackgroundColor(UI::UIColor(0.0f, 0.0f, 0.0f, 0.0f));
        hitTarget->SetBorderWidth(0.0f);
        hitTarget->SetInteractive(true);
        hitTarget->SetTooltipText(tooltip);
        hitTarget->SetOnClick(std::move(onClick));
        return hitTarget;
    }

    const char* CameraModeLabel(ViewportCameraMode mode)
    {
        switch (mode)
        {
            case ViewportCameraMode::Orbit:
                return "Orbit";
            case ViewportCameraMode::Fly:
                return "Fly";
            case ViewportCameraMode::TopDown:
                return "Top";
        }
        return "Orbit";
    }

    EditorUICursorMode ToUICursorMode(EditorViewportCursorMode mode)
    {
        switch (mode)
        {
            case EditorViewportCursorMode::Normal:
                return EditorUICursorMode::Normal;
            case EditorViewportCursorMode::Hidden:
                return EditorUICursorMode::Hidden;
            case EditorViewportCursorMode::Locked:
                return EditorUICursorMode::Locked;
        }
        return EditorUICursorMode::Normal;
    }

    const char* ShadingModeLabel(ViewportShadingMode mode)
    {
        switch (mode)
        {
            case ViewportShadingMode::Lit:
                return "Lit";
            case ViewportShadingMode::Unlit:
                return "Unlit";
            case ViewportShadingMode::Wireframe:
                return "Wire";
            case ViewportShadingMode::Normals:
                return "Normals";
            case ViewportShadingMode::Depth:
                return "Depth";
            case ViewportShadingMode::Albedo:
                return "Albedo";
        }
        return "Lit";
    }

    const char* SnapLabel(bool enabled)
    {
        return enabled ? "Snap On" : "Snap Off";
    }

    std::string FormatViewportToolStatusText(
        const EditorViewportToolState& toolState,
        ViewportCameraMode cameraMode,
        ViewportShadingMode shadingMode,
        const char* modeLabel,
        const char* spaceLabel)
    {
        return std::string(modeLabel ? modeLabel : "Move") + " | " +
               (spaceLabel ? spaceLabel : "World") + " | " +
               SnapLabel(toolState.snapEnabled) + " | " +
               CameraModeLabel(cameraMode) + " | " +
               ShadingModeLabel(shadingMode);
    }

    const char* ManipulatorHandleName(EditorViewportManipulatorHandle handle)
    {
        switch (handle)
        {
            case EditorViewportManipulatorHandle::Center:
                return "Center";
            case EditorViewportManipulatorHandle::AxisX:
                return "AxisX";
            case EditorViewportManipulatorHandle::AxisY:
                return "AxisY";
            case EditorViewportManipulatorHandle::AxisZ:
                return "AxisZ";
            case EditorViewportManipulatorHandle::PlaneXY:
                return "PlaneXY";
            case EditorViewportManipulatorHandle::PlaneXZ:
                return "PlaneXZ";
            case EditorViewportManipulatorHandle::PlaneYZ:
                return "PlaneYZ";
            case EditorViewportManipulatorHandle::None:
                return "None";
        }
        return "None";
    }

    UI::UIColor ManipulatorHandleColor(EditorViewportManipulatorHandle handle,
                                       const UI::UITheme& theme,
                                       bool active)
    {
        if (active)
        {
            return theme.colors.accent;
        }

        switch (handle)
        {
            case EditorViewportManipulatorHandle::AxisX:
                return UI::UIColor(0.92f, 0.22f, 0.24f, 0.92f);
            case EditorViewportManipulatorHandle::AxisY:
                return UI::UIColor(0.24f, 0.78f, 0.36f, 0.92f);
            case EditorViewportManipulatorHandle::AxisZ:
                return UI::UIColor(0.24f, 0.42f, 0.94f, 0.92f);
            case EditorViewportManipulatorHandle::PlaneXY:
                return UI::UIColor(0.88f, 0.78f, 0.20f, 0.46f);
            case EditorViewportManipulatorHandle::PlaneXZ:
                return UI::UIColor(0.72f, 0.36f, 0.88f, 0.44f);
            case EditorViewportManipulatorHandle::PlaneYZ:
                return UI::UIColor(0.20f, 0.72f, 0.82f, 0.44f);
            case EditorViewportManipulatorHandle::Center:
                return UI::UIColor(0.92f, 0.82f, 0.24f, 0.95f);
            case EditorViewportManipulatorHandle::None:
                break;
        }

        return theme.colors.accent;
    }

    void AddManipulatorSegmentVisual(UI::Panel& handleWidget,
                                     const std::string& widgetName,
                                     const EditorViewportManipulatorHandleInfo& handle,
                                     const UI::UIColor& color,
                                     bool active)
    {
        const Vec2 localStart = handle.screenStart - handle.bounds.Position();
        const Vec2 localEnd = handle.screenEnd - handle.bounds.Position();
        const Vec2 segment = localEnd - localStart;
        const float segmentLength = length(segment);
        if (segmentLength <= 0.0001f)
        {
            return;
        }

        const uint32 sampleCount =
            static_cast<uint32>(std::clamp(std::ceil(segmentLength / 5.0f),
                                           2.0f,
                                           18.0f));
        const float dotSize = active ? 5.0f : 4.0f;
        for (uint32 sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            const float t = sampleCount <= 1
                                ? 0.0f
                                : static_cast<float>(sampleIndex) /
                                      static_cast<float>(sampleCount - 1);
            const Vec2 position = localStart + segment * t;
            UI::Panel::Ptr segmentDot = UI::Panel::Create();
            segmentDot->SetName(widgetName + ".Segment." + std::to_string(sampleIndex));
            segmentDot->SetPosition(position.x - dotSize * 0.5f,
                                    position.y - dotSize * 0.5f);
            segmentDot->SetSize(dotSize, dotSize);
            segmentDot->SetBackgroundColor(color);
            segmentDot->SetBorderWidth(0.0f);
            segmentDot->SetInteractive(false);
            handleWidget.AddChild(segmentDot);
        }
    }

    void AddManipulatorPlaneVisual(UI::Panel& handleWidget,
                                   const std::string& widgetName,
                                   const EditorViewportManipulatorHandleInfo& handle,
                                   const UI::UIColor& color,
                                   bool active)
    {
        std::array<Vec2, 9> samples{};
        Vec2 center(0.0f);
        for (const Vec2& corner : handle.screenCorners)
        {
            center += corner;
        }
        center *= 0.25f;

        samples[0] = center;
        for (uint32 index = 0; index < 4; ++index)
        {
            samples[1 + index] = handle.screenCorners[index];
            samples[5 + index] =
                (handle.screenCorners[index] + handle.screenCorners[(index + 1) % 4]) * 0.5f;
        }

        const float dotSize = active ? 6.0f : 5.0f;
        for (uint32 sampleIndex = 0; sampleIndex < samples.size(); ++sampleIndex)
        {
            const Vec2 localPosition = samples[sampleIndex] - handle.bounds.Position();
            UI::Panel::Ptr planeDot = UI::Panel::Create();
            planeDot->SetName(widgetName + ".Plane." + std::to_string(sampleIndex));
            planeDot->SetPosition(localPosition.x - dotSize * 0.5f,
                                  localPosition.y - dotSize * 0.5f);
            planeDot->SetSize(dotSize, dotSize);
            planeDot->SetBackgroundColor(color);
            planeDot->SetBorderWidth(0.0f);
            planeDot->SetInteractive(false);
            handleWidget.AddChild(planeDot);
        }
    }

    void AddManipulatorRingVisual(UI::Panel& handleWidget,
                                  const std::string& widgetName,
                                  const EditorViewportManipulatorHandleInfo& handle,
                                  const UI::UIColor& color,
                                  bool active)
    {
        const float dotSize = active ? 6.0f : 5.0f;
        for (uint32 sampleIndex = 0;
             sampleIndex < handle.screenRingSampleCount;
             ++sampleIndex)
        {
            const Vec2 localPosition =
                handle.screenRingSamples[sampleIndex] - handle.bounds.Position();
            UI::Panel::Ptr ringDot = UI::Panel::Create();
            ringDot->SetName(widgetName + ".Ring." + std::to_string(sampleIndex));
            ringDot->SetPosition(localPosition.x - dotSize * 0.5f,
                                 localPosition.y - dotSize * 0.5f);
            ringDot->SetSize(dotSize, dotSize);
            ringDot->SetBackgroundColor(color);
            ringDot->SetBorderWidth(0.0f);
            ringDot->SetInteractive(false);
            handleWidget.AddChild(ringDot);
        }
    }

    void AddSelectionOutlineVisual(UI::Panel& selectionWidget,
                                   const std::string& widgetName,
                                   const EditorViewportSelectionOverlayInfo& overlay,
                                   const UI::UIColor& color)
    {
        constexpr float dotSize = 4.0f;
        for (uint32 sampleIndex = 0;
             sampleIndex < overlay.edgeSampleCount;
             ++sampleIndex)
        {
            const Vec2 localPosition =
                overlay.edgeSamples[sampleIndex] - overlay.bounds.Position();
            UI::Panel::Ptr outlineDot = UI::Panel::Create();
            outlineDot->SetName(widgetName + ".Outline." + std::to_string(sampleIndex));
            outlineDot->SetPosition(localPosition.x - dotSize * 0.5f,
                                    localPosition.y - dotSize * 0.5f);
            outlineDot->SetSize(dotSize, dotSize);
            outlineDot->SetBackgroundColor(color);
            outlineDot->SetBorderWidth(0.0f);
            outlineDot->SetInteractive(false);
            selectionWidget.AddChild(outlineDot);
        }

        constexpr float anchorSize = 7.0f;
        const Vec2 localAnchor = overlay.anchor - overlay.bounds.Position();
        UI::Panel::Ptr anchor = UI::Panel::Create();
        anchor->SetName(widgetName + ".Anchor");
        anchor->SetPosition(localAnchor.x - anchorSize * 0.5f,
                            localAnchor.y - anchorSize * 0.5f);
        anchor->SetSize(anchorSize, anchorSize);
        anchor->SetBackgroundColor(color.WithAlpha(0.95f));
        anchor->SetBorderColor(UI::UIColor(0.04f, 0.05f, 0.06f, 0.90f));
        anchor->SetBorderWidth(1.0f);
        anchor->SetInteractive(false);
        selectionWidget.AddChild(anchor);
    }
}

NativeViewportPanel::NativeViewportPanel()
{
    m_desc.id = PanelId();
    m_desc.title = "Viewport";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Center;
    m_desc.visibleByDefault = true;
    m_desc.closable = true;
}

void NativeViewportPanel::OnUpdate(float deltaTime)
{
    m_viewport.OnUpdate(deltaTime);
}

void NativeViewportPanel::BuildUI(EditorUIPanelFrameContext& context)
{
    m_lastBuildStats = {};
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    m_lastBuildStats.built = true;

    AddToolbar(context);
    AddViewportContent(context);
    m_lastBuildStats.renderTargetState = m_viewport.GetRenderTargetState();
}

EditorUICursorRequest NativeViewportPanel::GetCursorRequest() const
{
    const EditorViewportInteractionStats& interactionStats =
        m_lastBuildStats.interactionStats;

    EditorUICursorRequest request;
    request.requestsMouseCapture = interactionStats.requestsMouseCapture;
    request.requestsCursorHidden = interactionStats.requestsCursorHidden;
    request.requestsCursorLock = interactionStats.requestsCursorLock;
    request.cursorMode = ToUICursorMode(interactionStats.cursorMode);
    if (request.IsActive())
    {
        request.ownerPanelId = PanelId();
    }
    return request;
}

void NativeViewportPanel::SetRenderDevice(IRHIDevice* device)
{
    m_viewport.SetRenderDevice(device);
}

EditorViewportToolState NativeViewportPanel::CaptureToolState() const
{
    return m_toolService ? m_toolService->CaptureState()
                         : m_toolModel.CaptureState();
}

const EditorViewportToolModelStats& NativeViewportPanel::GetToolStats() const
{
    return m_toolService ? m_toolService->GetLastStats()
                         : m_toolModel.GetLastStats();
}

void NativeViewportPanel::SetToolMode(EditorContext::GizmoMode mode)
{
    if (m_toolService)
    {
        m_toolService->SetMode(mode);
        return;
    }

    m_toolModel.SetMode(mode);
}

void NativeViewportPanel::SetToolSpace(EditorContext::GizmoSpace space)
{
    if (m_toolService)
    {
        m_toolService->SetSpace(space);
        return;
    }

    m_toolModel.SetSpace(space);
}

void NativeViewportPanel::ToggleToolSnap()
{
    if (m_toolService)
    {
        m_toolService->ToggleSnap();
        return;
    }

    m_toolModel.ToggleSnap();
}

void NativeViewportPanel::ActivateNavigationAxis(EditorViewportNavigationAxis axis)
{
    const EditorViewportNavigationViewTarget target =
        m_navigationModel.ActivateAxis(axis,
                                       m_viewport.GetCameraPosition(),
                                       m_viewport.GetCameraTarget());
    if (!target.valid)
    {
        return;
    }

    m_viewport.SetCameraTarget(target.target);
    m_viewport.AlignCameraToViewDirection(target.directionFromTarget,
                                          target.cameraMode);
    m_lastBuildStats.navigationStats = m_navigationModel.GetLastStats();
}

const char* NativeViewportPanel::GetToolModeLabel(EditorContext::GizmoMode mode) const
{
    return m_toolService ? m_toolService->GetModeLabel(mode)
                         : m_toolModel.GetModeLabel(mode);
}

const char* NativeViewportPanel::GetToolSpaceLabel(EditorContext::GizmoSpace space) const
{
    return m_toolService ? m_toolService->GetSpaceLabel(space)
                         : m_toolModel.GetSpaceLabel(space);
}

void NativeViewportPanel::AddToolbar(EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float buttonHeight = shellMetrics.panelControlHeight;
    const float contentWidth = std::max(0.0f, context.contentContainer->GetWidth());
    float x = padding;
    const float y = padding;
    const float buttonGap = std::max(4.0f, padding * 0.5f);

    auto addButton = [&](const std::string& name,
                         const std::string& text,
                         const std::string& iconName,
                         float minWidth,
                         bool active,
                         UI::EventCallback callback) -> bool {
        const bool iconOnly = !iconName.empty();
        const float width = iconOnly
                                ? buttonHeight
                                : EstimateViewportButtonWidth(text,
                                                              theme,
                                                              minWidth);
        if (x + width > contentWidth - padding)
        {
            return false;
        }

        UI::Button::Ptr button =
            CreateViewportButton(name, text, iconName, theme, active);
        button->SetPosition(x, y);
        button->SetSize(width, buttonHeight);
        button->SetOnClick(std::move(callback));
        context.contentContainer->AddChild(button);
        x += width + buttonGap;
        ++m_lastBuildStats.toolbarButtonCount;
        if (iconOnly)
        {
            ++m_lastBuildStats.toolbarIconButtonCount;
        }
        else
        {
            ++m_lastBuildStats.toolbarTextButtonCount;
        }
        return true;
    };

    auto addSeparator = [&](const std::string& name) -> bool {
        const float preGap = std::max(3.0f, padding * 0.42f);
        const float postGap = std::max(6.0f, padding * 0.78f);
        const float separatorWidth = std::max(1.0f, theme.metrics.borderWidth);
        const float separatorHeight = std::max(12.0f, buttonHeight - 10.0f);
        const float separatorX = x + preGap;
        if (separatorX + separatorWidth + postGap > contentWidth - padding)
        {
            return false;
        }

        UI::Panel::Ptr separator = CreateToolbarSeparator(name, theme);
        separator->SetPosition(separatorX,
                               y + (buttonHeight - separatorHeight) * 0.5f);
        separator->SetSize(separatorWidth, separatorHeight);
        context.contentContainer->AddChild(separator);
        x = separatorX + separatorWidth + postGap;
        ++m_lastBuildStats.toolbarGroupSeparatorCount;
        return true;
    };

    const EditorViewportToolState toolState = CaptureToolState();
    addButton("NativeViewport.Gizmo.Translate",
              GetToolModeLabel(EditorContext::GizmoMode::Translate),
              "move",
              54.0f,
              toolState.mode == EditorContext::GizmoMode::Translate,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  SetToolMode(EditorContext::GizmoMode::Translate);
              });
    addButton("NativeViewport.Gizmo.Rotate",
              GetToolModeLabel(EditorContext::GizmoMode::Rotate),
              "rotate-cw",
              60.0f,
              toolState.mode == EditorContext::GizmoMode::Rotate,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  SetToolMode(EditorContext::GizmoMode::Rotate);
              });
    addButton("NativeViewport.Gizmo.Scale",
              GetToolModeLabel(EditorContext::GizmoMode::Scale),
              "scale",
              56.0f,
              toolState.mode == EditorContext::GizmoMode::Scale,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  SetToolMode(EditorContext::GizmoMode::Scale);
              });

    addSeparator("NativeViewport.Toolbar.Separator.TransformSpace");
    addButton("NativeViewport.Space.World",
              GetToolSpaceLabel(EditorContext::GizmoSpace::World),
              "globe",
              58.0f,
              toolState.space == EditorContext::GizmoSpace::World,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  SetToolSpace(EditorContext::GizmoSpace::World);
              });
    addButton("NativeViewport.Space.Local",
              GetToolSpaceLabel(EditorContext::GizmoSpace::Local),
              "local-axis",
              54.0f,
              toolState.space == EditorContext::GizmoSpace::Local,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  SetToolSpace(EditorContext::GizmoSpace::Local);
              });

    addSeparator("NativeViewport.Toolbar.Separator.SpaceToggles");
    addButton("NativeViewport.Toggle.Snap",
              "Snap",
              "snap",
              54.0f,
              toolState.snapEnabled,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  ToggleToolSnap();
              });
    addButton("NativeViewport.Toggle.Grid",
              "Grid",
              "grid",
              52.0f,
              m_viewport.GetShowGrid(),
              [this](const UI::UIEvent& event) {
                  (void)event;
                  m_viewport.SetShowGrid(!m_viewport.GetShowGrid());
              });
    addButton("NativeViewport.Toggle.Stats",
              "Stats",
              "stats",
              56.0f,
              m_viewport.GetShowStats(),
              [this](const UI::UIEvent& event) {
                  (void)event;
                  m_viewport.SetShowStats(!m_viewport.GetShowStats());
              });

    addSeparator("NativeViewport.Toolbar.Separator.TogglesCamera");
    addButton("NativeViewport.Camera.Orbit",
              "Orbit",
              "orbit",
              58.0f,
              m_viewport.GetCameraMode() == ViewportCameraMode::Orbit,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  m_viewport.SetCameraMode(ViewportCameraMode::Orbit);
              });
    addButton("NativeViewport.Camera.Fly",
              "Fly",
              "fly",
              44.0f,
              m_viewport.GetCameraMode() == ViewportCameraMode::Fly,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  m_viewport.SetCameraMode(ViewportCameraMode::Fly);
              });
    addButton("NativeViewport.Camera.Top",
              "Top",
              "top",
              44.0f,
              m_viewport.GetCameraMode() == ViewportCameraMode::TopDown,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  m_viewport.SetCameraMode(ViewportCameraMode::TopDown);
              });

    addButton("NativeViewport.Camera.Focus",
              "Focus",
              "focus",
              56.0f,
              false,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  m_viewport.FocusOnSelection();
              });
    addButton("NativeViewport.Camera.Reset",
              "Reset",
              "reset",
              58.0f,
              false,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  m_viewport.ResetCamera();
              });

    addSeparator("NativeViewport.Toolbar.Separator.CameraShading");
    addButton("NativeViewport.Shading.Lit",
              "Lit",
              "sun",
              42.0f,
              m_viewport.GetShadingMode() == ViewportShadingMode::Lit,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  m_viewport.SetShadingMode(ViewportShadingMode::Lit);
              });
    addButton("NativeViewport.Shading.Wire",
              "Wire",
              "wireframe",
              50.0f,
              m_viewport.GetShadingMode() == ViewportShadingMode::Wireframe,
              [this](const UI::UIEvent& event) {
                  (void)event;
                  m_viewport.SetShadingMode(ViewportShadingMode::Wireframe);
              });
    m_lastBuildStats.toolStats = GetToolStats();
    AddToolStatusStrip(context, toolState, x);
}

void NativeViewportPanel::AddToolStatusStrip(
    EditorUIPanelFrameContext& context,
    const EditorViewportToolState& toolState,
    float reservedLeft)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float buttonHeight = shellMetrics.panelControlHeight;
    const float contentWidth = std::max(0.0f, context.contentContainer->GetWidth());
    const std::string statusText = FormatViewportToolStatusText(
        toolState,
        m_viewport.GetCameraMode(),
        m_viewport.GetShadingMode(),
        GetToolModeLabel(toolState.mode),
        GetToolSpaceLabel(toolState.space));

    m_lastBuildStats.toolbarStatusText = statusText;

    const float fontSize = EditorTypography::GetFontSize(
        theme,
        EditorTypographyRole::ListItem);
    const float measuredWidth = EditorTypography::MeasureTextWidth(
        statusText,
        theme,
        EditorTypographyRole::ListItem);
    const float minStatusWidth = std::max(220.0f, fontSize * 14.0f);
    const float desiredStatusWidth =
        std::clamp(measuredWidth + padding * 2.0f,
                   minStatusWidth,
                   std::max(minStatusWidth, contentWidth * 0.34f));
    const float statusX = contentWidth - padding - desiredStatusWidth;
    if (statusX < reservedLeft + padding || statusX < padding)
    {
        return;
    }

    UI::Panel::Ptr status = UI::Panel::Create();
    status->SetName("NativeViewport.ToolStatus");
    status->SetPosition(statusX, padding);
    status->SetSize(desiredStatusWidth, buttonHeight);
    status->SetBackgroundColor(theme.colors.surface.WithAlpha(0.70f));
    status->SetBorderColor(theme.colors.border);
    status->SetBorderWidth(std::max(1.0f, theme.metrics.borderWidth));
    status->SetInteractive(false);
    status->SetTooltipText("Viewport tool state: " + statusText);

    UI::Label::Ptr label = CreateViewportLabel("NativeViewport.ToolStatus.Text",
                                               statusText,
                                               theme,
                                               theme.colors.textMuted);
    label->SetTextAlign(UI::TextAlign::Right);
    label->SetTooltipText(status->GetTooltipText());
    label->SetPosition(padding, 0.0f);
    label->SetSize(std::max(0.0f, desiredStatusWidth - padding * 2.0f),
                   buttonHeight);
    status->AddChild(label);

    context.contentContainer->AddChild(status);
    m_lastBuildStats.toolbarStatusBuilt = true;
    m_lastBuildStats.toolbarStatusBounds =
        UI::Rect(context.contentBounds.x + statusX,
                 context.contentBounds.y + padding,
                 desiredStatusWidth,
                 buttonHeight);
}

void NativeViewportPanel::AddViewportContent(EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float viewportY = shellMetrics.singleRowPanelToolbarHeight;
    const UI::Rect localBounds(padding,
                               viewportY,
                               std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f),
                               std::max(0.0f, context.contentContainer->GetHeight() -
                                                  viewportY -
                                                  padding));
    const UI::Rect globalBounds(context.contentBounds.x + localBounds.x,
                                context.contentBounds.y + localBounds.y,
                                localBounds.width,
                                localBounds.height);
    m_lastBuildStats.contentBounds = globalBounds;

    UI::Panel::Ptr viewportRoot = UI::Panel::Create();
    viewportRoot->SetName("NativeViewport.Content");
    viewportRoot->SetPosition(localBounds.x, localBounds.y);
    viewportRoot->SetSize(localBounds.width, localBounds.height);
    viewportRoot->SetBackgroundColor(UI::UIColor(0.035f, 0.042f, 0.052f, 1.0f));
    viewportRoot->SetBorderColor(theme.colors.border);
    viewportRoot->SetBorderWidth(std::max(1.0f, theme.metrics.borderWidth));
    viewportRoot->SetClipChildren(true);
    viewportRoot->SetInteractive(true);

    const ViewportNativeContentState nativeContent = m_viewport.PrepareNativeContent(globalBounds);
    const EditorViewportCameraFrame cameraFrame = m_viewport.CreateCameraFrame();

    bool viewportNavigationCapturedInput = false;
    if (context.input)
    {
        EditorViewportInteractionDesc interactionDesc;
        interactionDesc.viewportBounds = globalBounds;
        const EditorViewportInteractionState& interactionState =
            m_interactionModel.Update(*context.input, interactionDesc);
        viewportNavigationCapturedInput = interactionState.blocksSceneTools;
        m_lastBuildStats.interactionStats = m_interactionModel.GetLastStats();
    }

    EditorViewportManipulatorBuildDesc manipulatorDesc;
    manipulatorDesc.viewportBounds = globalBounds;
    manipulatorDesc.toolState = CaptureToolState();
    manipulatorDesc.cameraFrame = cameraFrame;
    m_manipulatorModel.Build(EditorContext::Get(), manipulatorDesc);

    bool manipulatorConsumedInput = false;
    bool selectionConsumedInput = false;
    if (context.input)
    {
        if (!viewportNavigationCapturedInput)
        {
            manipulatorConsumedInput =
                m_manipulatorModel.HandleInput(EditorContext::Get(), *context.input);
            EditorViewportSelectionPickDesc selectionDesc;
            selectionDesc.sceneManager =
                m_selectionService ? m_selectionService->GetActiveSceneManager()
                                   : EditorContext::Get().GetActiveSceneManager();
            selectionDesc.cameraFrame = cameraFrame;
            selectionConsumedInput = m_selectionService
                                         ? m_selectionModel.HandleInput(
                                               *m_selectionService,
                                               *context.input,
                                               selectionDesc,
                                               manipulatorConsumedInput)
                                         : m_selectionModel.HandleInput(
                                               EditorContext::Get(),
                                               *context.input,
                                               selectionDesc,
                                               manipulatorConsumedInput);
        }

        if (viewportNavigationCapturedInput ||
            (!manipulatorConsumedInput && !selectionConsumedInput))
        {
            m_viewport.OnNativeInput(*context.input);
        }
    }

    EditorViewportSelectionOverlayBuildDesc selectionOverlayDesc;
    selectionOverlayDesc.viewportBounds = globalBounds;
    selectionOverlayDesc.cameraFrame = cameraFrame;
    m_selectionOverlayModel.Build(EditorContext::Get(), selectionOverlayDesc);

    if (nativeContent.displayImageReady && nativeContent.textureView)
    {
        UI::Image::Ptr image = UI::Image::Create();
        image->SetName("NativeViewport.SceneImage");
        image->SetPosition(0.0f, 0.0f);
        image->SetSize(localBounds.width, localBounds.height);
        image->SetTextureView(nativeContent.textureView);
        image->SetUVRect(UI::Rect(0.0f, 1.0f, 1.0f, -1.0f));
        image->SetInteractive(false);
        viewportRoot->AddChild(image);
        m_lastBuildStats.imageBuilt = true;
    }
    else
    {
        UI::Label::Ptr fallback = CreateViewportLabel("NativeViewport.Fallback",
                                                      nativeContent.fallbackReason.empty()
                                                          ? "Viewport waiting for render target"
                                                          : nativeContent.fallbackReason,
                                                      theme,
                                                      theme.colors.textMuted);
        fallback->SetPosition(12.0f, 10.0f);
        fallback->SetSize(std::max(0.0f, localBounds.width - 24.0f), 20.0f);
        viewportRoot->AddChild(fallback);
        m_lastBuildStats.fallbackBuilt = true;
    }

    if (m_viewport.GetShowGrid())
    {
        AddGrid(*viewportRoot, localBounds, UI::UIColor(0.17f, 0.20f, 0.24f, 0.72f));
    }

    AddCrosshair(*viewportRoot, localBounds, UI::UIColor(0.46f, 0.50f, 0.56f, 0.95f));
    AddSelectionOverlay(*viewportRoot, globalBounds, theme);
    AddManipulatorOverlay(*viewportRoot, globalBounds, theme);
    if (m_viewport.GetShowStats())
    {
        AddStatsOverlay(*viewportRoot, localBounds, theme);
    }
    AddNavigationHUD(*viewportRoot, localBounds, globalBounds, theme, cameraFrame);

    m_lastBuildStats.toolStats = GetToolStats();
    m_lastBuildStats.manipulatorStats = m_manipulatorModel.GetLastStats();
    m_lastBuildStats.selectionStats = m_selectionModel.GetLastStats();
    m_lastBuildStats.selectionOverlayStats = m_selectionOverlayModel.GetLastStats();
    context.contentContainer->AddChild(viewportRoot);
}

void NativeViewportPanel::AddGrid(UI::Panel& viewportRoot,
                                  const UI::Rect& localBounds,
                                  const UI::UIColor& color)
{
    constexpr float gridSpacing = 50.0f;
    uint32 lineIndex = 0;
    for (float x = gridSpacing; x < localBounds.width; x += gridSpacing)
    {
        viewportRoot.AddChild(CreateViewportLine("NativeViewport.Grid.Vertical." +
                                                     std::to_string(lineIndex++),
                                                 x,
                                                 0.0f,
                                                 1.0f,
                                                 localBounds.height,
                                                 color));
        ++m_lastBuildStats.gridLineCount;
    }

    lineIndex = 0;
    for (float y = gridSpacing; y < localBounds.height; y += gridSpacing)
    {
        viewportRoot.AddChild(CreateViewportLine("NativeViewport.Grid.Horizontal." +
                                                     std::to_string(lineIndex++),
                                                 0.0f,
                                                 y,
                                                 localBounds.width,
                                                 1.0f,
                                                 color));
        ++m_lastBuildStats.gridLineCount;
    }
}

void NativeViewportPanel::AddCrosshair(UI::Panel& viewportRoot,
                                       const UI::Rect& localBounds,
                                       const UI::UIColor& color)
{
    const float centerX = localBounds.width * 0.5f;
    const float centerY = localBounds.height * 0.5f;
    viewportRoot.AddChild(CreateViewportLine("NativeViewport.Crosshair.Horizontal",
                                             std::max(0.0f, centerX - 20.0f),
                                             centerY,
                                             40.0f,
                                             1.0f,
                                             color));
    viewportRoot.AddChild(CreateViewportLine("NativeViewport.Crosshair.Vertical",
                                             centerX,
                                             std::max(0.0f, centerY - 20.0f),
                                             1.0f,
                                             40.0f,
                                             color));
}

    void NativeViewportPanel::AddStatsOverlay(UI::Panel& viewportRoot,
                                          const UI::Rect& localBounds,
                                          const UI::UITheme& theme)
{
    constexpr uint32 RVX_VIEWPORT_STATS_LINE_COUNT = 9;
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float paddingX = shellMetrics.contentPadding;
    const float paddingY = shellMetrics.compactGap;
    const float rowGap = std::max(2.0f, shellMetrics.compactGap * 0.35f);
    const float lineHeight =
        EditorTypography::GetLineHeight(theme, EditorTypographyRole::Caption);
    const float panelWidth =
        std::min(std::max(320.0f, shellMetrics.formRowHeight * 11.5f),
                 std::max(120.0f, localBounds.width - paddingX * 3.0f));
    const float panelHeight =
        paddingY * 2.0f +
        lineHeight * static_cast<float>(RVX_VIEWPORT_STATS_LINE_COUNT) +
        rowGap * static_cast<float>(RVX_VIEWPORT_STATS_LINE_COUNT - 1u);
    UI::Panel::Ptr statsPanel = UI::Panel::Create();
    statsPanel->SetName("NativeViewport.Stats");
    statsPanel->SetPosition(std::max(10.0f, shellMetrics.contentPadding),
                            std::max(10.0f, shellMetrics.contentPadding));
    statsPanel->SetSize(panelWidth, panelHeight);
    statsPanel->SetBackgroundColor(UI::UIColor(0.02f, 0.025f, 0.03f, 0.82f));
    statsPanel->SetBorderColor(theme.colors.border);
    statsPanel->SetBorderWidth(1.0f);
    statsPanel->SetInteractive(false);

    float rowY = paddingY;
    const float rowWidth = std::max(0.0f, panelWidth - paddingX * 2.0f);
    auto placeStatsLabel = [&](UI::Label& label) {
        label.SetPosition(paddingX, rowY);
        label.SetSize(rowWidth, lineHeight);
        rowY += lineHeight + rowGap;
    };

    const Vec3& camera = m_viewport.GetCameraPosition();
    const Vec3& target = m_viewport.GetCameraTarget();
    const ViewportRenderTargetState& state = m_viewport.GetRenderTargetState();
    char buffer[128];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "Camera: %.1f, %.1f, %.1f",
                  camera.x,
                  camera.y,
                  camera.z);
    UI::Label::Ptr cameraLabel = CreateViewportLabel("NativeViewport.Stats.Camera",
                                                     buffer,
                                                     theme,
                                                     theme.colors.text);
    placeStatsLabel(*cameraLabel);
    statsPanel->AddChild(cameraLabel);

    std::snprintf(buffer,
                  sizeof(buffer),
                  "Target: %.1f, %.1f, %.1f",
                  target.x,
                  target.y,
                  target.z);
    UI::Label::Ptr targetLabel = CreateViewportLabel("NativeViewport.Stats.Target",
                                                     buffer,
                                                     theme,
                                                     theme.colors.textMuted);
    placeStatsLabel(*targetLabel);
    statsPanel->AddChild(targetLabel);

    std::snprintf(buffer, sizeof(buffer), "Viewport: %ux%u", state.width, state.height);
    UI::Label::Ptr sizeLabel = CreateViewportLabel("NativeViewport.Stats.Size",
                                                   buffer,
                                                   theme,
                                                   theme.colors.textMuted);
    placeStatsLabel(*sizeLabel);
    statsPanel->AddChild(sizeLabel);

    const std::string mode = std::string(CameraModeLabel(m_viewport.GetCameraMode())) +
                             " / " +
                             ShadingModeLabel(m_viewport.GetShadingMode());
    UI::Label::Ptr modeLabel = CreateViewportLabel("NativeViewport.Stats.Mode",
                                                   "Mode: " + mode,
                                                   theme,
                                                   theme.colors.textMuted);
    placeStatsLabel(*modeLabel);
    statsPanel->AddChild(modeLabel);

    UI::Label::Ptr backendLabel = CreateViewportLabel(
        "NativeViewport.Stats.Backend",
        state.backend == ViewportRenderBackend::RHI ? "Backend: RHI target"
                                                    : "Backend: Placeholder target",
        theme,
        state.backend == ViewportRenderBackend::RHI ? theme.colors.accent
                                                    : theme.colors.textMuted);
    placeStatsLabel(*backendLabel);
    statsPanel->AddChild(backendLabel);

    const ViewportRenderGraphDiagnostics& graph =
        state.renderGraphDiagnostics;
    std::snprintf(buffer,
                  sizeof(buffer),
                  "Graph: %u pass / %u culled",
                  graph.totalPasses,
                  graph.culledPasses);
    UI::Label::Ptr graphLabel = CreateViewportLabel("NativeViewport.Stats.Graph",
                                                    buffer,
                                                    theme,
                                                    graph.graphCompiled
                                                        ? theme.colors.textMuted
                                                        : theme.colors.warning);
    placeStatsLabel(*graphLabel);
    statsPanel->AddChild(graphLabel);

    std::snprintf(buffer,
                  sizeof(buffer),
                  "Barriers: %u (%u tex, %u buf)",
                  graph.barrierCount,
                  graph.textureBarrierCount,
                  graph.bufferBarrierCount);
    UI::Label::Ptr barrierLabel = CreateViewportLabel(
        "NativeViewport.Stats.Barriers",
        buffer,
        theme,
        theme.colors.textMuted);
    placeStatsLabel(*barrierLabel);
    statsPanel->AddChild(barrierLabel);

    std::snprintf(buffer,
                  sizeof(buffer),
                  "Validation: %u warn / %u err",
                  graph.validationWarningCount,
                  graph.validationErrorCount);
    const UI::UIColor validationColor =
        graph.validationErrorCount > 0u
            ? theme.colors.error
            : (graph.validationWarningCount > 0u ? theme.colors.warning
                                                 : theme.colors.textMuted);
    UI::Label::Ptr validationLabel = CreateViewportLabel(
        "NativeViewport.Stats.Validation",
        buffer,
        theme,
        validationColor);
    placeStatsLabel(*validationLabel);
    statsPanel->AddChild(validationLabel);

    std::string reason = graph.skippedReason;
    if (reason.empty())
    {
        reason = graph.firstDiagnostic;
    }
    if (reason.empty())
    {
        reason = state.fallbackReason;
    }
    if (reason.empty())
    {
        reason = graph.graphCompileValid ? "OK" : "RenderGraph invalid";
    }
    const UI::UIColor reasonColor =
        (reason == "OK" && !graph.graphExecutionSkipped) ? theme.colors.textMuted
                                                          : theme.colors.warning;
    UI::Label::Ptr reasonLabel = CreateViewportLabel(
        "NativeViewport.Stats.Reason",
        "Reason: " + ShortenStatsText(reason, 44u),
        theme,
        reasonColor);
    placeStatsLabel(*reasonLabel);
    statsPanel->AddChild(reasonLabel);

    viewportRoot.AddChild(statsPanel);
}

void NativeViewportPanel::AddNavigationHUD(UI::Panel& viewportRoot,
                                           const UI::Rect& localBounds,
                                           const UI::Rect& globalBounds,
                                           const UI::UITheme& theme,
                                           const EditorViewportCameraFrame& cameraFrame)
{
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float minViewportExtent = std::min(localBounds.width, localBounds.height);
    const float minimumExtent =
        std::max(96.0f, shellMetrics.formRowHeight * 2.15f);
    if (minViewportExtent < minimumExtent)
    {
        return;
    }

    const float margin = std::max(10.0f, shellMetrics.contentPadding);
    const float preferredSize =
        std::max(76.0f, shellMetrics.formRowHeight * 2.20f);
    const float maxSize = std::max(64.0f, minViewportExtent * 0.28f);
    const float hudSize = std::min(preferredSize, maxSize);
    if (hudSize <= 0.0f || localBounds.width < hudSize + margin * 2.0f)
    {
        return;
    }

    UI::Panel::Ptr hud = UI::Panel::Create();
    hud->SetName("NativeViewport.NavigationHUD");
    hud->SetPosition(std::max(margin, localBounds.width - hudSize - margin),
                     margin);
    hud->SetSize(hudSize, hudSize);
    hud->SetBackgroundColor(UI::UIColor(0.02f, 0.025f, 0.03f, 0.70f));
    hud->SetBorderColor(theme.colors.border.WithAlpha(0.85f));
    hud->SetBorderWidth(std::max(1.0f, theme.metrics.borderWidth));
    hud->SetInteractive(false);

    const float axisLength = hudSize * 0.30f;
    const float dotSize = std::max(4.0f, theme.metrics.borderWidth * 3.0f);

    EditorViewportNavigationHUDDesc hudDesc;
    hudDesc.cameraFrame = cameraFrame;
    hudDesc.localBounds = UI::Rect(0.0f, 0.0f, hudSize, hudSize);
    hudDesc.axisLength = axisLength;
    hudDesc.hitSize = std::max(shellMetrics.formRowHeight * 0.72f,
                               dotSize * 4.0f);
    const EditorViewportNavigationHUDState& hudState =
        m_navigationModel.BuildHUD(hudDesc);
    if (!hudState.valid)
    {
        return;
    }

    UI::Panel::Ptr centerDot =
        CreateViewportDot("NativeViewport.NavigationHUD.Center",
                          hudState.center,
                          dotSize * 1.35f,
                          UI::UIColor(0.92f, 0.95f, 1.0f, 0.96f));
    centerDot->SetBorderColor(UI::UIColor(0.04f, 0.05f, 0.06f, 0.90f));
    centerDot->SetBorderWidth(std::max(1.0f, theme.metrics.borderWidth));
    hud->AddChild(centerDot);

    for (const EditorViewportNavigationAxisVisual& axis : hudState.axes)
    {
        if (!axis.valid)
        {
            continue;
        }

        UI::UIColor color;
        switch (axis.axis)
        {
            case EditorViewportNavigationAxis::X:
                color = UI::UIColor(0.94f, 0.24f, 0.26f, 0.96f);
                break;
            case EditorViewportNavigationAxis::Y:
                color = UI::UIColor(0.28f, 0.82f, 0.38f, 0.96f);
                break;
            case EditorViewportNavigationAxis::Z:
                color = UI::UIColor(0.30f, 0.48f, 0.96f, 0.96f);
                break;
        }

        const std::string widgetName = NavigationAxisWidgetName(axis.axis);
        AddNavigationAxisVisual(*hud,
                                widgetName,
                                axis.label,
                                hudState.center,
                                axis.direction,
                                axisLength,
                                dotSize,
                                color,
                                theme);

        const UI::Rect hitRect(hud->GetPosition().x + axis.hitRect.x,
                               hud->GetPosition().y + axis.hitRect.y,
                               axis.hitRect.width,
                               axis.hitRect.height);
        viewportRoot.AddChild(CreateNavigationHitTarget(
            widgetName + ".Hit",
            hitRect,
            std::string("Align viewport to +") + axis.label,
            [this, axis = axis.axis](const UI::UIEvent& event) {
                (void)event;
                ActivateNavigationAxis(axis);
            }));
    }

    m_lastBuildStats.navigationHUDBuilt = true;
    m_lastBuildStats.navigationHUDAxisCount = hudState.axisCount;
    m_lastBuildStats.navigationStats = m_navigationModel.GetLastStats();
    m_lastBuildStats.navigationHUDBounds =
        UI::Rect(globalBounds.x + hud->GetPosition().x,
                 globalBounds.y + hud->GetPosition().y,
                 hud->GetWidth(),
                 hud->GetHeight());
    viewportRoot.AddChild(hud);
}

void NativeViewportPanel::AddSelectionOverlay(UI::Panel& viewportRoot,
                                              const UI::Rect& globalBounds,
                                              const UI::UITheme& theme)
{
    const EditorViewportSelectionOverlayInfo& overlay =
        m_selectionOverlayModel.GetOverlay();
    if (!overlay.enabled)
    {
        return;
    }

    UI::Panel::Ptr selectionWidget = UI::Panel::Create();
    selectionWidget->SetName("NativeViewport.Selection");
    selectionWidget->SetPosition(overlay.bounds.x - globalBounds.x,
                                 overlay.bounds.y - globalBounds.y);
    selectionWidget->SetSize(overlay.bounds.width, overlay.bounds.height);
    selectionWidget->SetBackgroundColor(UI::UIColor::Transparent());
    selectionWidget->SetBorderWidth(0.0f);
    selectionWidget->SetInteractive(false);

    AddSelectionOutlineVisual(*selectionWidget,
                              "NativeViewport.Selection",
                              overlay,
                              theme.colors.accent);
    viewportRoot.AddChild(selectionWidget);
}

void NativeViewportPanel::AddManipulatorOverlay(UI::Panel& viewportRoot,
                                                const UI::Rect& globalBounds,
                                                const UI::UITheme& theme)
{
    const EditorViewportManipulatorStats& stats = m_manipulatorModel.GetLastStats();
    for (uint32 index = 0; index < m_manipulatorModel.GetHandleCount(); ++index)
    {
        const EditorViewportManipulatorHandleInfo* handle = m_manipulatorModel.GetHandle(index);
        if (!handle || !handle->enabled)
        {
            continue;
        }

        const std::string widgetName = std::string("NativeViewport.Manipulator.") +
                                       ManipulatorHandleName(handle->handle);
        const bool active = stats.activeHandle == handle->handle;
        const UI::UIColor color = ManipulatorHandleColor(handle->handle, theme, active);

        UI::Panel::Ptr handleWidget = UI::Panel::Create();
        handleWidget->SetName(widgetName);
        handleWidget->SetPosition(handle->bounds.x - globalBounds.x,
                                  handle->bounds.y - globalBounds.y);
        handleWidget->SetSize(handle->bounds.width, handle->bounds.height);
        if (handle->hitRing)
        {
            handleWidget->SetBackgroundColor(UI::UIColor::Transparent());
            handleWidget->SetBorderWidth(0.0f);
            AddManipulatorRingVisual(*handleWidget,
                                     widgetName,
                                     *handle,
                                     color,
                                     active);
        }
        else if (handle->hitPolygon)
        {
            handleWidget->SetBackgroundColor(UI::UIColor::Transparent());
            handleWidget->SetBorderWidth(0.0f);
            AddManipulatorPlaneVisual(*handleWidget,
                                      widgetName,
                                      *handle,
                                      color,
                                      active);
        }
        else if (handle->hitSegment)
        {
            handleWidget->SetBackgroundColor(UI::UIColor::Transparent());
            handleWidget->SetBorderWidth(0.0f);
            AddManipulatorSegmentVisual(*handleWidget,
                                        widgetName,
                                        *handle,
                                        color,
                                        active);
        }
        else
        {
            handleWidget->SetBackgroundColor(color);
            handleWidget->SetBorderColor(theme.colors.surface.WithAlpha(0.85f));
            handleWidget->SetBorderWidth(active ? 2.0f : 1.0f);
        }
        handleWidget->SetInteractive(false);
        viewportRoot.AddChild(handleWidget);
    }
}

} // namespace RVX::Editor
