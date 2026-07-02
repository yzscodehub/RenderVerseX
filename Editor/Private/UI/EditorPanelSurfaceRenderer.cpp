/**
 * @file EditorPanelSurfaceRenderer.cpp
 * @brief Native editor panel chrome and dock area layout renderer implementation
 */

#include "Editor/UI/EditorPanelSurfaceRenderer.h"

#include "Editor/UI/EditorIconButton.h"
#include "Editor/UI/EditorPanelChrome.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "UI/UIContext.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_PANEL_SURFACE_ROOT = "Editor.PanelSurface.Root";

    class EditorPanelSurfaceRoot final : public UI::Panel
    {
    public:
        UI::Widget* HitTest(const Vec2& point) override
        {
            if (m_visibility != UI::Visibility::Visible || !m_interactive ||
                !GetGlobalRect().Contains(point))
            {
                return nullptr;
            }

            for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
            {
                UI::Widget* hit = (*it)->HitTest(point);
                if (hit)
                {
                    return hit;
                }
            }

            return nullptr;
        }
    };

    std::string PanelWidgetName(const std::string& panelId, const char* suffix)
    {
        return std::string("Editor.PanelSurface.Panel.") + panelId + "." + suffix;
    }

    std::string DockTabWidgetName(const std::string& stackId,
                                  const std::string& panelId)
    {
        return std::string("Editor.PanelSurface.DockTab.") + stackId + "." + panelId;
    }

    float SafePositive(float value)
    {
        return std::max(0.0f, value);
    }

    float ClampSize(float value, float minimum, float maximum)
    {
        if (maximum <= 0.0f)
        {
            return 0.0f;
        }
        return std::clamp(value, std::min(minimum, maximum), maximum);
    }

    float ClampPanelWeight(float value)
    {
        if (!std::isfinite(value))
        {
            return 1.0f;
        }

        return std::clamp(value, 0.001f, 1.0f);
    }

    const char* DockDropAreaSuffix(EditorUIPanelDockArea area)
    {
        switch (area)
        {
            case EditorUIPanelDockArea::Left:
                return "Left";
            case EditorUIPanelDockArea::Right:
                return "Right";
            case EditorUIPanelDockArea::Bottom:
                return "Bottom";
            case EditorUIPanelDockArea::Center:
                return "Center";
            case EditorUIPanelDockArea::Floating:
                return "Floating";
        }

        return "Unknown";
    }

    bool ComparePlacementOrder(const EditorDockPanelPlacement* lhs,
                               const EditorDockPanelPlacement* rhs)
    {
        if (!lhs || !rhs)
        {
            return lhs != nullptr;
        }
        if (lhs->order != rhs->order)
        {
            return lhs->order < rhs->order;
        }

        return lhs->panelId < rhs->panelId;
    }

    struct LinearLayoutItem
    {
        const EditorDockPanelPlacement* placement = nullptr;
        float weight = 1.0f;
    };

    std::vector<const EditorDockPanelPlacement*> CollectTabStackPlacements(
        const EditorDockingModel& model,
        const std::string& stackId,
        EditorUIPanelDockArea area)
    {
        std::vector<const EditorDockPanelPlacement*> placements;
        if (stackId.empty())
        {
            return placements;
        }

        for (const EditorDockPanelPlacement& placement : model.GetPlacements())
        {
            if (placement.visible && placement.area == area &&
                placement.tabStackId == stackId)
            {
                placements.push_back(&placement);
            }
        }
        std::stable_sort(placements.begin(), placements.end(), ComparePlacementOrder);
        return placements;
    }

    std::string ResolvePanelTitle(const EditorPanelSurfaceRenderDesc& desc,
                                  const std::string& panelId,
                                  const std::string& fallbackTitle)
    {
        if (desc.panelTitles)
        {
            const auto titleIt = desc.panelTitles->find(panelId);
            if (titleIt != desc.panelTitles->end() && !titleIt->second.empty())
            {
                return titleIt->second;
            }
        }

        return fallbackTitle.empty() ? panelId : fallbackTitle;
    }

    struct FloatingResizeHandleDesc
    {
        const char* suffix = "";
        bool resizeLeft = false;
        bool resizeRight = false;
        bool resizeTop = false;
        bool resizeBottom = false;
    };

    constexpr std::array<FloatingResizeHandleDesc, 8> RVX_EDITOR_FLOATING_RESIZE_HANDLES = {
        FloatingResizeHandleDesc{"Resize.TopLeft", true, false, true, false},
        FloatingResizeHandleDesc{"Resize.Top", false, false, true, false},
        FloatingResizeHandleDesc{"Resize.TopRight", false, true, true, false},
        FloatingResizeHandleDesc{"Resize.Left", true, false, false, false},
        FloatingResizeHandleDesc{"Resize.Right", false, true, false, false},
        FloatingResizeHandleDesc{"Resize.BottomLeft", true, false, false, true},
        FloatingResizeHandleDesc{"Resize.Bottom", false, false, false, true},
        FloatingResizeHandleDesc{"Resize.BottomRight", false, true, false, true},
    };

    EditorDockFloatingBounds MakeNormalizedFloatingBounds(const UI::Rect& bounds,
                                                          const UI::Rect& workArea)
    {
        EditorDockFloatingBounds normalized;
        if (workArea.width <= 0.0f || workArea.height <= 0.0f)
        {
            return normalized;
        }

        normalized.x = (bounds.x - workArea.x) / workArea.width;
        normalized.y = (bounds.y - workArea.y) / workArea.height;
        normalized.width = bounds.width / workArea.width;
        normalized.height = bounds.height / workArea.height;
        return normalized;
    }

    EditorDockFloatingBounds ResizeFloatingBounds(
        EditorDockFloatingBounds bounds,
        const UI::UIEvent& event,
        const UI::Rect& workArea,
        const FloatingResizeHandleDesc& handle)
    {
        if (workArea.width <= 0.0f || workArea.height <= 0.0f)
        {
            return bounds;
        }

        const float deltaX = event.delta.x / workArea.width;
        const float deltaY = event.delta.y / workArea.height;
        if (handle.resizeLeft)
        {
            bounds.x += deltaX;
            bounds.width -= deltaX;
        }
        if (handle.resizeRight)
        {
            bounds.width += deltaX;
        }
        if (handle.resizeTop)
        {
            bounds.y += deltaY;
            bounds.height -= deltaY;
        }
        if (handle.resizeBottom)
        {
            bounds.height += deltaY;
        }
        return bounds;
    }

    UI::Rect MakeFloatingPanelRect(const EditorDockFloatingBounds& bounds,
                                   const UI::Rect& workArea)
    {
        if (workArea.width <= 0.0f || workArea.height <= 0.0f)
        {
            return {};
        }

        return UI::Rect(workArea.x + bounds.x * workArea.width,
                        workArea.y + bounds.y * workArea.height,
                        bounds.width * workArea.width,
                        bounds.height * workArea.height);
    }

    EditorDockFloatingBounds MakeDetachedDockTabFloatingBounds(
        const Vec2& position,
        const UI::Rect& sourcePanelBounds,
        const UI::Rect& workArea)
    {
        if (workArea.width <= 0.0f || workArea.height <= 0.0f)
        {
            return {};
        }

        const float maxWidth = std::min(640.0f, workArea.width * 0.72f);
        const float maxHeight = std::min(520.0f, workArea.height * 0.72f);
        const float width = ClampSize(sourcePanelBounds.width * 0.55f,
                                      320.0f,
                                      maxWidth);
        const float height = ClampSize(sourcePanelBounds.height * 0.55f,
                                       220.0f,
                                       maxHeight);
        const float titleAnchor = std::min(32.0f, height * 0.18f);
        UI::Rect detachedBounds(position.x - width * 0.5f,
                                position.y - titleAnchor,
                                width,
                                height);
        detachedBounds.x = std::clamp(
            detachedBounds.x,
            workArea.x,
            std::max(workArea.x, workArea.Right() - detachedBounds.width));
        detachedBounds.y = std::clamp(
            detachedBounds.y,
            workArea.y,
            std::max(workArea.y, workArea.Bottom() - detachedBounds.height));
        return MakeNormalizedFloatingBounds(detachedBounds, workArea);
    }

    float GetDockDropHorizontalEdgeWidth(const UI::Rect& workArea)
    {
        return std::min(std::max(96.0f, workArea.width * 0.14f),
                        workArea.width * 0.35f);
    }

    float GetDockDropBottomEdgeHeight(const UI::Rect& workArea)
    {
        return std::min(std::max(96.0f, workArea.height * 0.16f),
                        workArea.height * 0.35f);
    }

    UI::Rect MakeDockDropZoneRect(EditorUIPanelDockArea area,
                                  const UI::Rect& workArea)
    {
        if (workArea.width <= 0.0f || workArea.height <= 0.0f)
        {
            return {};
        }

        const float horizontalEdgeWidth = GetDockDropHorizontalEdgeWidth(workArea);
        const float bottomEdgeHeight = GetDockDropBottomEdgeHeight(workArea);
        switch (area)
        {
            case EditorUIPanelDockArea::Left:
                return UI::Rect(workArea.x,
                                workArea.y,
                                horizontalEdgeWidth,
                                workArea.height);
            case EditorUIPanelDockArea::Right:
                return UI::Rect(workArea.Right() - horizontalEdgeWidth,
                                workArea.y,
                                horizontalEdgeWidth,
                                workArea.height);
            case EditorUIPanelDockArea::Bottom:
                return UI::Rect(workArea.x,
                                workArea.Bottom() - bottomEdgeHeight,
                                workArea.width,
                                bottomEdgeHeight);
            case EditorUIPanelDockArea::Center:
            {
                const float centerWidth =
                    std::min(std::max(140.0f, workArea.width * 0.16f),
                             workArea.width * 0.30f);
                const float centerHeight =
                    std::min(std::max(100.0f, workArea.height * 0.16f),
                             workArea.height * 0.30f);
                return UI::Rect(workArea.x + (workArea.width - centerWidth) * 0.5f,
                                workArea.y + (workArea.height - centerHeight) * 0.5f,
                                centerWidth,
                                centerHeight);
            }
            case EditorUIPanelDockArea::Floating:
                return {};
        }

        return {};
    }

    bool TryResolveDockDropArea(const Vec2& position,
                                const UI::Rect& workArea,
                                EditorUIPanelDockArea& area)
    {
        if (workArea.width <= 0.0f || workArea.height <= 0.0f ||
            !workArea.Contains(position))
        {
            return false;
        }

        constexpr std::array<EditorUIPanelDockArea, 4> RVX_DOCK_DROP_RESOLVE_ORDER = {
            EditorUIPanelDockArea::Left,
            EditorUIPanelDockArea::Right,
            EditorUIPanelDockArea::Bottom,
            EditorUIPanelDockArea::Center,
        };
        for (EditorUIPanelDockArea candidate : RVX_DOCK_DROP_RESOLVE_ORDER)
        {
            if (MakeDockDropZoneRect(candidate, workArea).Contains(position))
            {
                area = candidate;
                return true;
            }
        }

        return false;
    }

    bool TryResolveTabStackDropTarget(
        const Vec2& position,
        const std::vector<EditorPanelSurfaceTabStackDropTarget>& targets,
        EditorUIPanelDockArea& area,
        std::string& stackId)
    {
        for (auto it = targets.rbegin(); it != targets.rend(); ++it)
        {
            if (it->stackId.empty() || it->bounds.width <= 0.0f ||
                it->bounds.height <= 0.0f || !it->bounds.Contains(position))
            {
                continue;
            }

            area = it->area;
            stackId = it->stackId;
            return true;
        }

        return false;
    }

    bool TryResolveDockTabReorderTarget(
        const Vec2& position,
        const std::vector<EditorPanelSurfaceDockTabTarget>& targets,
        const std::string& draggedPanelId,
        const std::string& stackId,
        std::string& targetPanelId,
        bool& afterTarget)
    {
        if (draggedPanelId.empty() || stackId.empty())
        {
            return false;
        }

        for (auto it = targets.rbegin(); it != targets.rend(); ++it)
        {
            if (it->stackId != stackId || it->panelId.empty() ||
                it->panelId == draggedPanelId || it->bounds.width <= 0.0f ||
                it->bounds.height <= 0.0f || !it->bounds.Contains(position))
            {
                continue;
            }

            targetPanelId = it->panelId;
            afterTarget = position.x >= it->bounds.x + it->bounds.width * 0.5f;
            return true;
        }

        return false;
    }

    class EditorDockDragWidget final : public UI::Panel
    {
    public:
        using DragCallback = std::function<void(const UI::UIEvent&)>;

        void SetDragCallback(DragCallback callback)
        {
            m_dragCallback = std::move(callback);
        }

        void SetDragStartCallback(DragCallback callback)
        {
            m_dragStartCallback = std::move(callback);
        }

        void SetDragEndCallback(DragCallback callback)
        {
            m_dragEndCallback = std::move(callback);
        }

        void SetDragStopCallback(DragCallback callback)
        {
            m_dragStopCallback = std::move(callback);
        }

        bool HandleEvent(const UI::UIEvent& event) override
        {
            switch (event.type)
            {
                case UI::UIEventType::MouseDown:
                    if (event.button == static_cast<int>(UI::UIMouseButton::Left))
                    {
                        m_dragging = true;
                        m_dragDistance = 0.0f;
                        if (m_dragStartCallback)
                        {
                            m_dragStartCallback(event);
                        }
                        return true;
                    }
                    break;
                case UI::UIEventType::MouseMove:
                    if (m_dragging)
                    {
                        m_dragDistance +=
                            std::sqrt(event.delta.x * event.delta.x +
                                      event.delta.y * event.delta.y);
                        if (m_dragCallback)
                        {
                            m_dragCallback(event);
                        }
                        return true;
                    }
                    break;
                case UI::UIEventType::MouseUp:
                    if (m_dragging)
                    {
                        if (m_dragEndCallback && m_dragDistance >= 4.0f)
                        {
                            m_dragEndCallback(event);
                        }
                        if (m_dragStopCallback)
                        {
                            m_dragStopCallback(event);
                        }
                        m_dragging = false;
                        return true;
                    }
                    break;
                case UI::UIEventType::MouseLeave:
                    if (m_dragging)
                    {
                        return true;
                    }
                    break;
                default:
                    break;
            }

            return UI::Panel::HandleEvent(event);
        }

    private:
        DragCallback m_dragStartCallback;
        DragCallback m_dragCallback;
        DragCallback m_dragEndCallback;
        DragCallback m_dragStopCallback;
        float m_dragDistance = 0.0f;
        bool m_dragging = false;
    };

    UI::Panel::Ptr CreatePanel(const std::string& name,
                               const UI::Rect& bounds,
                               const UI::UIColor& background,
                               const UI::UIColor& border,
                               float borderWidth)
    {
        UI::Panel::Ptr panel = UI::Panel::Create();
        panel->SetName(name);
        panel->SetPosition(bounds.x, bounds.y);
        panel->SetSize(bounds.width, bounds.height);
        panel->SetBackgroundColor(background);
        panel->SetBorderColor(border);
        panel->SetBorderWidth(borderWidth);
        return panel;
    }

    EditorIconButton::Ptr CreatePanelChromeIconButton(
        const std::string& name,
        const std::string& text,
        const std::string& iconName,
        const UI::Rect& bounds,
        const UI::UITheme& theme,
        const EditorPanelChromeStyle& chrome,
        const std::string& tooltip)
    {
        EditorIconButton::Ptr button = EditorIconButton::Create(text, iconName);
        button->SetName(name);
        button->SetPosition(bounds.x, bounds.y);
        button->SetSize(bounds.width, bounds.height);
        button->SetTooltipText(tooltip.empty() ? text : tooltip);
        button->SetShowText(false);
        button->SetIconSize(std::max(12.0f, chrome.buttonSize * 0.55f));
        button->SetSurfaceColors(chrome.buttonBackground,
                                 chrome.buttonHoverBackground,
                                 chrome.buttonPressedBackground,
                                 chrome.buttonDisabledBackground);
        button->SetIconColors(chrome.textSecondary,
                              chrome.textPrimary,
                              chrome.textPrimary,
                              chrome.textSecondary.WithAlpha(0.55f));
        button->SetFocusIndicator(chrome.accent, std::max(1.0f, theme.metrics.borderWidth));
        button->GetStyle().fontSize =
            EditorTypography::GetFontSize(theme, EditorTypographyRole::PanelTitle);
        button->GetStyle().textColor = chrome.textSecondary;
        return button;
    }

    std::shared_ptr<EditorDockDragWidget> CreateDragPanel(
        const std::string& name,
        const UI::Rect& bounds,
        const UI::UIColor& color,
        EditorDockDragWidget::DragCallback callback,
        EditorDockDragWidget::DragCallback dragStartCallback = {},
        EditorDockDragWidget::DragCallback dragEndCallback = {},
        EditorDockDragWidget::DragCallback dragStopCallback = {})
    {
        auto panel = std::make_shared<EditorDockDragWidget>();
        panel->SetName(name);
        panel->SetPosition(bounds.x, bounds.y);
        panel->SetSize(bounds.width, bounds.height);
        panel->SetBackgroundColor(color);
        panel->SetBorderColor(UI::UIColor::Transparent());
        panel->SetBorderWidth(0.0f);
        panel->SetInteractive(true);
        panel->SetDragCallback(std::move(callback));
        panel->SetDragStartCallback(std::move(dragStartCallback));
        panel->SetDragEndCallback(std::move(dragEndCallback));
        panel->SetDragStopCallback(std::move(dragStopCallback));
        return panel;
    }

    UI::Rect MakeFloatingResizeHandleRect(const UI::Rect& panelBounds,
                                          const FloatingResizeHandleDesc& handle,
                                          float thickness,
                                          float cornerSize)
    {
        const float width = std::max(0.0f, panelBounds.width);
        const float height = std::max(0.0f, panelBounds.height);
        const float edgeWidth = std::max(0.0f, width - cornerSize * 2.0f);
        const float edgeHeight = std::max(0.0f, height - cornerSize * 2.0f);

        if (handle.resizeTop && handle.resizeLeft)
        {
            return UI::Rect(0.0f, 0.0f, cornerSize, cornerSize);
        }
        if (handle.resizeTop && handle.resizeRight)
        {
            return UI::Rect(width - cornerSize, 0.0f, cornerSize, cornerSize);
        }
        if (handle.resizeBottom && handle.resizeLeft)
        {
            return UI::Rect(0.0f, height - cornerSize, cornerSize, cornerSize);
        }
        if (handle.resizeBottom && handle.resizeRight)
        {
            return UI::Rect(width - cornerSize,
                            height - cornerSize,
                            cornerSize,
                            cornerSize);
        }
        if (handle.resizeTop)
        {
            return UI::Rect(cornerSize, 0.0f, edgeWidth, thickness);
        }
        if (handle.resizeBottom)
        {
            return UI::Rect(cornerSize, height - thickness, edgeWidth, thickness);
        }
        if (handle.resizeLeft)
        {
            return UI::Rect(0.0f, cornerSize, thickness, edgeHeight);
        }
        if (handle.resizeRight)
        {
            return UI::Rect(width - thickness, cornerSize, thickness, edgeHeight);
        }

        return {};
    }

    void AddFloatingResizeHandles(UI::Panel& panel,
                                  const std::string& panelId,
                                  const UI::Rect& panelBounds,
                                  const UI::Rect& workArea,
                                  EditorUIHost* host,
                                  const UI::UIColor& color,
                                  float thickness,
                                  float cornerSize,
                                  uint32& handleCount)
    {
        if (!host || workArea.width <= 0.0f || workArea.height <= 0.0f ||
            panelBounds.width <= cornerSize * 2.0f ||
            panelBounds.height <= cornerSize * 2.0f)
        {
            return;
        }

        const EditorDockFloatingBounds initialFloatingBounds =
            MakeNormalizedFloatingBounds(panelBounds, workArea);
        for (const FloatingResizeHandleDesc& handle : RVX_EDITOR_FLOATING_RESIZE_HANDLES)
        {
            const UI::Rect handleBounds =
                MakeFloatingResizeHandleRect(panelBounds, handle, thickness, cornerSize);
            if (handleBounds.width <= 0.0f || handleBounds.height <= 0.0f)
            {
                continue;
            }

            std::shared_ptr<EditorDockDragWidget> dragHandle = CreateDragPanel(
                PanelWidgetName(panelId, handle.suffix),
                handleBounds,
                color,
                [host,
                 panelId,
                 workArea,
                 initialFloatingBounds,
                 handle](const UI::UIEvent& event) {
                    if (!host)
                    {
                        return;
                    }

                    EditorDockingModel& model = host->GetDockingModel();
                    EditorDockFloatingBounds bounds = initialFloatingBounds;
                    if (const EditorDockPanelPlacement* placement =
                            model.FindPlacement(panelId))
                    {
                        if (placement->hasFloatingBounds)
                        {
                            bounds = placement->floatingBounds;
                        }
                    }

                    bounds = ResizeFloatingBounds(bounds, event, workArea, handle);
                    model.SetFloatingPanelBounds(panelId, bounds);
                },
                [host, panelId](const UI::UIEvent& event) {
                    (void)event;
                    if (host)
                    {
                        host->GetDockingModel().BringFloatingPanelToFront(panelId);
                    }
                });
            panel.AddChild(std::move(dragHandle));
            ++handleCount;
        }
    }
}

void EditorPanelSurfaceRenderer::Begin(const EditorPanelSurfaceRenderDesc& desc)
{
    m_desc = desc;
    m_lastBuildStats = {};
    m_rootPanel = nullptr;
    m_leftArea = {};
    m_rightArea = {};
    m_bottomArea = {};
    m_centerArea = {};
    m_floatingArea = {};
    m_panelRects.clear();
    m_tabStackDropTargets.clear();
    m_dockTabTargets.clear();

    if (!m_desc.ui)
    {
        return;
    }

    Clear(*m_desc.ui);

    const float width = static_cast<float>(m_desc.ui->GetWidth());
    const float height = static_cast<float>(m_desc.ui->GetHeight());
    const float top = SafePositive(m_desc.topReservedHeight);
    const float bottom = SafePositive(m_desc.bottomReservedHeight);
    m_lastBuildStats.workArea =
        UI::Rect(0.0f, top, width, std::max(0.0f, height - top - bottom));
    const EditorPanelChromeStyle chrome =
        BuildEditorPanelChromeStyle(m_desc.ui->GetTheme());
    m_lastBuildStats.titleBarHeight = chrome.titleBarHeight;
    m_lastBuildStats.chromeTabHeight = chrome.tabHeight;
    m_lastBuildStats.chromeFrameBorderWidth = chrome.frameBorderWidth;

    CountVisiblePlacements();
    ConfigureAreaLayouts();
    BuildPanelRects();
}

EditorPanelSurfaceFrame EditorPanelSurfaceRenderer::BuildPanelFrame(
    const EditorUIPanelDesc& panelDesc)
{
    EditorPanelSurfaceFrame frame;
    if (!m_desc.ui || !m_desc.dockingModel || panelDesc.id.empty())
    {
        return frame;
    }

    const EditorDockPanelPlacement* placement =
        m_desc.dockingModel->FindPlacement(panelDesc.id);
    if (!placement || !placement->visible)
    {
        return frame;
    }
    const bool isDockTabStack =
        placement->area != EditorUIPanelDockArea::Floating &&
        !placement->tabStackId.empty();
    if (isDockTabStack &&
        m_desc.dockingModel->ResolveActiveDockTab(placement->tabStackId) != panelDesc.id)
    {
        return frame;
    }

    const auto rectIt = m_panelRects.find(panelDesc.id);
    if (rectIt == m_panelRects.end())
    {
        return frame;
    }

    const UI::Rect panelBounds = rectIt->second;
    if (panelBounds.width <= 1.0f || panelBounds.height <= 1.0f)
    {
        return frame;
    }

    const UI::UITheme& theme = m_desc.ui->GetTheme();
    const EditorPanelChromeStyle chrome = BuildEditorPanelChromeStyle(theme);
    if (!m_rootPanel)
    {
        UI::Panel::Ptr root = std::make_shared<EditorPanelSurfaceRoot>();
        root->SetName(RVX_EDITOR_PANEL_SURFACE_ROOT);
        root->SetPosition(0.0f, 0.0f);
        root->SetSize(static_cast<float>(m_desc.ui->GetWidth()),
                      static_cast<float>(m_desc.ui->GetHeight()));
        root->SetBackgroundColor(UI::UIColor::Transparent());
        root->SetBorderWidth(0.0f);
        m_rootPanel = root.get();
        m_desc.ui->GetCanvas().AddWidget(std::move(root));
    }

    const float titleHeight = chrome.titleBarHeight;
    const float borderWidth = chrome.frameBorderWidth;
    const float padding = chrome.panelPadding;
    UI::Panel::Ptr panel = CreatePanel(PanelWidgetName(panelDesc.id, "Frame"),
                                       panelBounds,
                                       chrome.frameBackground,
                                       chrome.borderStrong,
                                       borderWidth);
    panel->SetClipChildren(true);

    const UI::Rect titleBarBounds(0.0f, 0.0f, panelBounds.width, titleHeight);
    UI::Panel::Ptr titleBar;
    if (placement->area == EditorUIPanelDockArea::Floating && m_desc.host)
    {
        const UI::Rect workArea = m_lastBuildStats.workArea;
        const std::vector<EditorPanelSurfaceTabStackDropTarget> tabStackDropTargets =
            m_tabStackDropTargets;
        const EditorDockFloatingBounds initialFloatingBounds =
            MakeNormalizedFloatingBounds(panelBounds, workArea);
        titleBar = CreateDragPanel(
            PanelWidgetName(panelDesc.id, "TitleBar"),
            titleBarBounds,
            chrome.floatingTitleBackground,
            [host = m_desc.host,
             panelId = panelDesc.id,
             workArea,
             tabStackDropTargets,
             initialFloatingBounds](const UI::UIEvent& event) {
                if (!host || workArea.width <= 0.0f || workArea.height <= 0.0f)
                {
                    return;
                }

                EditorDockingModel& model = host->GetDockingModel();
                EditorDockFloatingBounds bounds = initialFloatingBounds;
                if (const EditorDockPanelPlacement* placement =
                        model.FindPlacement(panelId))
                {
                    if (placement->hasFloatingBounds)
                    {
                        bounds = placement->floatingBounds;
                    }
                }

                bounds.x += event.delta.x / workArea.width;
                bounds.y += event.delta.y / workArea.height;
                model.SetFloatingPanelBounds(panelId, bounds);

                EditorUIPanelDockArea previewArea = EditorUIPanelDockArea::Floating;
                std::string previewStackId;
                if (TryResolveTabStackDropTarget(event.position,
                                                 tabStackDropTargets,
                                                 previewArea,
                                                 previewStackId))
                {
                    model.SetDockDropPreview(panelId, previewArea, previewStackId);
                }
                else if (TryResolveDockDropArea(event.position, workArea, previewArea))
                {
                    model.SetDockDropPreview(panelId, previewArea);
                }
                else
                {
                    model.ClearDockDropPreview(panelId);
                }
            },
            [host = m_desc.host, panelId = panelDesc.id](const UI::UIEvent& event) {
                (void)event;
                if (host)
                {
                    EditorDockingModel& model = host->GetDockingModel();
                    model.BringFloatingPanelToFront(panelId);
                    model.ClearDockDropPreview(panelId);
                }
            },
            [host = m_desc.host,
             panelId = panelDesc.id,
             workArea,
             tabStackDropTargets](const UI::UIEvent& event) {
                if (!host)
                {
                    return;
                }

                EditorUIPanelDockArea dockArea = EditorUIPanelDockArea::Floating;
                std::string stackId;
                if (TryResolveTabStackDropTarget(event.position,
                                                 tabStackDropTargets,
                                                 dockArea,
                                                 stackId))
                {
                    host->GetDockingModel().SetPanelDockTabStack(panelId,
                                                                 dockArea,
                                                                 stackId);
                }
                else if (TryResolveDockDropArea(event.position, workArea, dockArea))
                {
                    host->GetDockingModel().SetPanelDockArea(panelId, dockArea);
                }
            },
            [host = m_desc.host, panelId = panelDesc.id](const UI::UIEvent& event) {
                (void)event;
                if (host)
                {
                    host->GetDockingModel().ClearDockDropPreview(panelId);
                }
            });
    }
    else if (isDockTabStack)
    {
        titleBar = CreatePanel(PanelWidgetName(panelDesc.id, "TitleBar"),
                               titleBarBounds,
                               chrome.titleBackground,
                               chrome.borderSubtle,
                               0.0f);
        m_tabStackDropTargets.push_back(
            {placement->tabStackId,
             placement->area,
             UI::Rect(panelBounds.x, panelBounds.y, panelBounds.width, titleHeight)});

        const std::vector<const EditorDockPanelPlacement*> tabPlacements =
            CollectTabStackPlacements(*m_desc.dockingModel,
                                      placement->tabStackId,
                                      placement->area);
        const float closeWidth =
            panelDesc.closable ? chrome.buttonSize : 0.0f;
        const bool isBottomDrawer = placement->area == EditorUIPanelDockArea::Bottom;
        const bool bottomDrawerCollapsed =
            isBottomDrawer &&
            m_desc.dockingModel->IsDockAreaCollapsed(EditorUIPanelDockArea::Bottom);
        const float drawerToggleWidth =
            isBottomDrawer ? chrome.buttonSize : 0.0f;
        const float availableWidth =
            std::max(0.0f,
                     panelBounds.width - closeWidth - drawerToggleWidth - 6.0f);
        const float tabHeight = chrome.tabHeight;
        float cursorX = 3.0f;
        ++m_lastBuildStats.dockTabStackCount;
        m_lastBuildStats.dockTabCount +=
            static_cast<uint32>(tabPlacements.size());
        if (!tabPlacements.empty())
        {
            m_lastBuildStats.inactiveDockTabCount +=
                static_cast<uint32>(tabPlacements.size() - 1u);
        }

        struct DockTabRenderItem
        {
            const EditorDockPanelPlacement* placement = nullptr;
            std::string title;
            UI::Rect localBounds;
            UI::Rect globalBounds;
            bool active = false;
        };

        std::vector<DockTabRenderItem> tabItems;
        std::vector<EditorPanelSurfaceDockTabTarget> reorderTargets;
        tabItems.reserve(tabPlacements.size());
        reorderTargets.reserve(tabPlacements.size());
        for (const EditorDockPanelPlacement* tabPlacement : tabPlacements)
        {
            if (!tabPlacement)
            {
                continue;
            }

            const bool activeTab = tabPlacement->panelId == panelDesc.id;
            const std::string title =
                ResolvePanelTitle(m_desc, tabPlacement->panelId, tabPlacement->panelId);
            const float desiredWidth =
                EditorTypography::EstimatePaddedTextWidth(title,
                                                          theme,
                                                          EditorTypographyRole::PanelTab,
                                                          chrome.tabMinWidth,
                                                          chrome.tabMaxWidth);
            const float remainingWidth = std::max(0.0f, availableWidth - cursorX);
            const float tabWidth = std::min(desiredWidth, remainingWidth);
            if (tabWidth <= 12.0f)
            {
                break;
            }

            const UI::Rect localTabBounds(cursorX, 2.0f, tabWidth, tabHeight);
            const UI::Rect globalTabBounds(panelBounds.x + localTabBounds.x,
                                           panelBounds.y + localTabBounds.y,
                                           localTabBounds.width,
                                           localTabBounds.height);
            tabItems.push_back({tabPlacement,
                                title,
                                localTabBounds,
                                globalTabBounds,
                                activeTab});
            reorderTargets.push_back({placement->tabStackId,
                                      tabPlacement->panelId,
                                      globalTabBounds});
            m_dockTabTargets.push_back(reorderTargets.back());
            cursorX += tabWidth + chrome.tabGap;
        }

        for (const DockTabRenderItem& tabItem : tabItems)
        {
            if (!tabItem.placement)
            {
                continue;
            }

            std::shared_ptr<EditorDockDragWidget> tab = CreateDragPanel(
                DockTabWidgetName(placement->tabStackId, tabItem.placement->panelId),
                tabItem.localBounds,
                tabItem.active ? chrome.tabActiveBackground
                               : chrome.tabInactiveBackground,
                [host = m_desc.host,
                 renderer = this,
                 panelId = tabItem.placement->panelId,
                 stackId = placement->tabStackId](const UI::UIEvent& event) {
                    if (!host || !renderer)
                    {
                        return;
                    }

                    EditorDockingModel& model = host->GetDockingModel();
                    EditorUIPanelDockArea previewArea = EditorUIPanelDockArea::Floating;
                    std::string previewStackId;
                    if (TryResolveTabStackDropTarget(event.position,
                                                     renderer->m_tabStackDropTargets,
                                                     previewArea,
                                                     previewStackId))
                    {
                        if (previewStackId != stackId)
                        {
                            model.SetDockDropPreview(panelId,
                                                     previewArea,
                                                     previewStackId);
                        }
                        else
                        {
                            model.ClearDockDropPreview(panelId);
                        }
                        return;
                    }

                    if (TryResolveDockDropArea(event.position,
                                               renderer->m_lastBuildStats.workArea,
                                               previewArea))
                    {
                        model.SetDockDropPreview(panelId, previewArea);
                        return;
                    }

                    model.ClearDockDropPreview(panelId);
                },
                {},
                [host = m_desc.host,
                 renderer = this,
                 panelId = tabItem.placement->panelId,
                 stackId = placement->tabStackId,
                 reorderTargets,
                 panelBounds](const UI::UIEvent& event) {
                    if (!host)
                    {
                        return;
                    }

                    EditorDockingModel& model = host->GetDockingModel();
                    std::string targetPanelId;
                    bool afterTarget = false;
                    if (TryResolveDockTabReorderTarget(event.position,
                                                       reorderTargets,
                                                       panelId,
                                                       stackId,
                                                       targetPanelId,
                                                       afterTarget))
                    {
                        model.MoveDockTab(panelId, targetPanelId, afterTarget);
                        model.ClearDockDropPreview(panelId);
                        return;
                    }

                    EditorUIPanelDockArea targetArea = EditorUIPanelDockArea::Floating;
                    std::string targetStackId;
                    if (renderer &&
                        TryResolveTabStackDropTarget(event.position,
                                                     renderer->m_tabStackDropTargets,
                                                     targetArea,
                                                     targetStackId))
                    {
                        if (targetStackId != stackId)
                        {
                            model.SetPanelDockTabStack(panelId, targetArea, targetStackId);
                        }
                        else
                        {
                            model.ClearDockDropPreview(panelId);
                        }
                        return;
                    }

                    const UI::Rect workArea =
                        renderer ? renderer->m_lastBuildStats.workArea : UI::Rect();
                    if (TryResolveDockDropArea(event.position, workArea, targetArea))
                    {
                        model.SetPanelDockArea(panelId, targetArea);
                        return;
                    }

                    if (workArea.Contains(event.position))
                    {
                        const EditorDockFloatingBounds floatingBounds =
                            MakeDetachedDockTabFloatingBounds(event.position,
                                                              panelBounds,
                                                              workArea);
                        model.DetachDockTabToFloating(panelId, floatingBounds);
                    }
                },
                [host = m_desc.host,
                 panelId = tabItem.placement->panelId](const UI::UIEvent& event) {
                    (void)event;
                    if (host)
                    {
                        EditorDockingModel& model = host->GetDockingModel();
                        model.ClearDockDropPreview(panelId);
                        model.SetActiveDockTab(panelId);
                    }
                });
            tab->SetBorderColor(tabItem.active ? chrome.tabActiveBorder
                                               : chrome.tabInactiveBorder);
            tab->SetBorderWidth(tabItem.active ? borderWidth : 0.0f);

            UI::Label::Ptr tabLabel = UI::Label::Create(tabItem.title);
            tabLabel->SetName(DockTabWidgetName(placement->tabStackId,
                                                tabItem.placement->panelId) +
                              ".Label");
            tabLabel->SetPosition(6.0f, 0.0f);
            tabLabel->SetSize(std::max(0.0f, tabItem.localBounds.width - 12.0f),
                              tabItem.localBounds.height);
            tabLabel->SetFontSize(
                EditorTypography::GetFontSize(theme,
                                              EditorTypographyRole::PanelTab));
            tabLabel->SetTextColor(tabItem.active ? chrome.textPrimary
                                                  : chrome.textSecondary);
            tabLabel->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
            tabLabel->SetTooltipText(tabItem.title);
            tabLabel->SetVerticalAlign(UI::VerticalAlign::Middle);
            tabLabel->SetInteractive(false);
            tab->AddChild(tabLabel);
            if (tabItem.active && chrome.tabAccentHeight > 0.0f)
            {
                UI::Panel::Ptr tabAccent = CreatePanel(
                    DockTabWidgetName(placement->tabStackId,
                                      tabItem.placement->panelId) +
                        ".Accent",
                    UI::Rect(0.0f,
                             0.0f,
                             tabItem.localBounds.width,
                             chrome.tabAccentHeight),
                    chrome.accent,
                    UI::UIColor::Transparent(),
                    0.0f);
                tabAccent->SetInteractive(false);
                tab->AddChild(tabAccent);
                ++m_lastBuildStats.dockTabAccentCount;
            }
            titleBar->AddChild(tab);
            ++m_lastBuildStats.dockTabReorderHandleCount;
        }

        if (panelDesc.closable)
        {
            EditorIconButton::Ptr close = CreatePanelChromeIconButton(
                PanelWidgetName(panelDesc.id, "Close"),
                "Close",
                "close",
                UI::Rect(panelBounds.width - closeWidth - 3.0f,
                         3.0f,
                         closeWidth,
                         chrome.buttonSize),
                theme,
                chrome,
                "Close panel");
            close->SetOnClick([host = m_desc.host,
                               panelId = panelDesc.id](const UI::UIEvent& event) {
                (void)event;
                if (host)
                {
                    host->SetPanelVisible(panelId, false);
                }
            });
            titleBar->AddChild(close);
            ++m_lastBuildStats.closablePanelCount;
            ++m_lastBuildStats.panelChromeIconButtonCount;
        }
        if (isBottomDrawer)
        {
            const float toggleRightInset = closeWidth + 6.0f;
            EditorIconButton::Ptr toggle = CreatePanelChromeIconButton(
                "Editor.PanelSurface.BottomDrawer.Toggle",
                bottomDrawerCollapsed ? "Expand bottom drawer"
                                      : "Collapse bottom drawer",
                bottomDrawerCollapsed ? "chevron-up" : "chevron-down",
                UI::Rect(panelBounds.width - toggleRightInset - drawerToggleWidth,
                         3.0f,
                         drawerToggleWidth,
                         chrome.buttonSize),
                theme,
                chrome,
                bottomDrawerCollapsed ? "Expand bottom drawer"
                                      : "Collapse bottom drawer");
            toggle->SetOnClick([host = m_desc.host](const UI::UIEvent& event) {
                (void)event;
                if (!host)
                {
                    return;
                }

                host->ToggleBottomDrawerCollapsed();
            });
            titleBar->AddChild(toggle);
            ++m_lastBuildStats.bottomDrawerToggleCount;
            ++m_lastBuildStats.panelChromeIconButtonCount;
        }
    }
    else
    {
        titleBar = CreatePanel(PanelWidgetName(panelDesc.id, "TitleBar"),
                               titleBarBounds,
                               chrome.titleBackground,
                               chrome.borderSubtle,
                               0.0f);
    }

    const float closeWidth = panelDesc.closable ? chrome.buttonSize : 0.0f;
    UI::Label::Ptr title = UI::Label::Create(panelDesc.title);
    title->SetName(PanelWidgetName(panelDesc.id, "Title"));
    title->SetPosition(padding, 0.0f);
    title->SetSize(std::max(0.0f, panelBounds.width - padding * 2.0f - closeWidth),
                   titleHeight);
    title->SetFontSize(EditorTypography::GetFontSize(
        theme,
        EditorTypographyRole::PanelTitle));
    title->SetTextColor(chrome.textPrimary);
    title->SetVerticalAlign(UI::VerticalAlign::Middle);
    title->SetInteractive(false);
    if (!isDockTabStack)
    {
        titleBar->AddChild(title);
    }

    if (panelDesc.closable && !isDockTabStack)
    {
        EditorIconButton::Ptr close = CreatePanelChromeIconButton(
            PanelWidgetName(panelDesc.id, "Close"),
            "Close",
            "close",
            UI::Rect(panelBounds.width - closeWidth - 3.0f,
                     3.0f,
                     closeWidth,
                     chrome.buttonSize),
            theme,
            chrome,
            "Close panel");
        close->SetOnClick([host = m_desc.host, panelId = panelDesc.id](const UI::UIEvent& event) {
            (void)event;
            if (host)
            {
                host->SetPanelVisible(panelId, false);
            }
        });
        titleBar->AddChild(close);
        ++m_lastBuildStats.closablePanelCount;
        ++m_lastBuildStats.panelChromeIconButtonCount;
    }

    const UI::Rect contentBounds(borderWidth,
                                 titleHeight,
                                 std::max(0.0f, panelBounds.width - borderWidth * 2.0f),
                                 std::max(0.0f, panelBounds.height - titleHeight - borderWidth));
    UI::Panel::Ptr content = CreatePanel(PanelWidgetName(panelDesc.id, "Content"),
                                         contentBounds,
                                         chrome.contentBackground,
                                         UI::UIColor::Transparent(),
                                         0.0f);
    content->SetClipChildren(true);
    content->SetInteractive(true);

    UI::Panel* panelRaw = panel.get();
    UI::Panel* contentRaw = content.get();
    panel->AddChild(titleBar);
    panel->AddChild(content);
    if (placement->area == EditorUIPanelDockArea::Floating)
    {
        const float handleThickness = std::max(7.0f, borderWidth * 6.0f);
        const float cornerSize = std::max(12.0f, handleThickness * 1.6f);
        AddFloatingResizeHandles(*panel,
                                 panelDesc.id,
                                 panelBounds,
                                 m_lastBuildStats.workArea,
                                 m_desc.host,
                                 chrome.borderSubtle,
                                 handleThickness,
                                 cornerSize,
                                 m_lastBuildStats.floatingResizeHandleCount);
    }
    m_rootPanel->AddChild(panel);

    frame.panelContainer = panelRaw;
    frame.contentContainer = contentRaw;
    frame.panelBounds = panelBounds;
    frame.contentBounds = UI::Rect(panelBounds.x + contentBounds.x,
                                   panelBounds.y + contentBounds.y,
                                   contentBounds.width,
                                   contentBounds.height);
    frame.built = true;

    ++m_lastBuildStats.panelFrameCount;
    ++m_lastBuildStats.visiblePanelCount;
    switch (placement->area)
    {
        case EditorUIPanelDockArea::Left:
            ++m_lastBuildStats.leftPanelCount;
            break;
        case EditorUIPanelDockArea::Right:
            ++m_lastBuildStats.rightPanelCount;
            break;
        case EditorUIPanelDockArea::Bottom:
            ++m_lastBuildStats.bottomPanelCount;
            break;
        case EditorUIPanelDockArea::Center:
            ++m_lastBuildStats.centerPanelCount;
            break;
        case EditorUIPanelDockArea::Floating:
            ++m_lastBuildStats.floatingPanelCount;
            break;
    }
    return frame;
}

void EditorPanelSurfaceRenderer::End()
{
    AddSplitterHandles();
    AddDockDropPreviewOverlay();
    m_desc = {};
    m_rootPanel = nullptr;
}

void EditorPanelSurfaceRenderer::Clear(UI::UIContext& ui)
{
    UI::Widget::Ptr root = ui.GetCanvas().FindWidget(RVX_EDITOR_PANEL_SURFACE_ROOT);
    if (root)
    {
        ui.GetCanvas().RemoveWidget(std::move(root));
    }
}

void EditorPanelSurfaceRenderer::CountVisiblePlacements()
{
    if (!m_desc.dockingModel)
    {
        return;
    }

    std::unordered_set<std::string> countedTabStacks;
    for (const EditorDockPanelPlacement& placement : m_desc.dockingModel->GetPlacements())
    {
        if (!placement.visible)
        {
            continue;
        }

        if (placement.area != EditorUIPanelDockArea::Floating &&
            !placement.tabStackId.empty())
        {
            const std::string tabStackKey =
                std::to_string(static_cast<uint32>(placement.area)) + ":" +
                placement.tabStackId;
            if (!countedTabStacks.insert(tabStackKey).second)
            {
                continue;
            }
        }

        switch (placement.area)
        {
            case EditorUIPanelDockArea::Left:
                ++m_leftArea.count;
                break;
            case EditorUIPanelDockArea::Right:
                ++m_rightArea.count;
                break;
            case EditorUIPanelDockArea::Bottom:
                ++m_bottomArea.count;
                break;
            case EditorUIPanelDockArea::Center:
                ++m_centerArea.count;
                break;
            case EditorUIPanelDockArea::Floating:
                ++m_floatingArea.count;
                break;
        }
    }
}

void EditorPanelSurfaceRenderer::ConfigureAreaLayouts()
{
    const UI::Rect work = m_lastBuildStats.workArea;
    if (work.width <= 0.0f || work.height <= 0.0f)
    {
        return;
    }

    const EditorDockAreaSizing defaultSizing;
    const float leftRatio = m_desc.dockingModel
                                ? m_desc.dockingModel->GetDockAreaSize(EditorUIPanelDockArea::Left)
                                : defaultSizing.leftWidthRatio;
    const float rightRatio = m_desc.dockingModel
                                 ? m_desc.dockingModel->GetDockAreaSize(EditorUIPanelDockArea::Right)
                                 : defaultSizing.rightWidthRatio;
    const float bottomRatio = m_desc.dockingModel
                                  ? m_desc.dockingModel->GetDockAreaSize(EditorUIPanelDockArea::Bottom)
                                  : defaultSizing.bottomHeightRatio;
    const bool compactWidth = work.width < 1100.0f;
    const float leftMinimumWidth = compactWidth ? 150.0f : 190.0f;
    const float rightMinimumWidth = compactWidth ? 180.0f : 230.0f;
    const float leftMaximumWidth = work.width * (compactWidth ? 0.28f : 0.32f);
    const float rightMaximumWidth = work.width * (compactWidth ? 0.32f : 0.34f);
    float leftWidth = m_leftArea.count > 0
                          ? ClampSize(work.width * leftRatio,
                                      leftMinimumWidth,
                                      leftMaximumWidth)
                          : 0.0f;
    float rightWidth = m_rightArea.count > 0
                           ? ClampSize(work.width * rightRatio,
                                       rightMinimumWidth,
                                       rightMaximumWidth)
                           : 0.0f;
    const float minimumCenterWidth =
        compactWidth
            ? std::min(work.width, std::max(280.0f, work.width * 0.42f))
            : std::min(work.width, std::max(520.0f, work.width * 0.46f));
    const float maximumCombinedSideWidth =
        std::max(0.0f, work.width - minimumCenterWidth);
    const float combinedSideWidth = leftWidth + rightWidth;
    if (combinedSideWidth > maximumCombinedSideWidth && combinedSideWidth > 0.0f)
    {
        const float sideScale = maximumCombinedSideWidth / combinedSideWidth;
        leftWidth = std::round(leftWidth * sideScale);
        rightWidth = std::round(rightWidth * sideScale);
    }
    const float centerWidth = std::max(0.0f, work.width - leftWidth - rightWidth);
    const bool bottomCollapsed =
        m_bottomArea.count > 0 && m_desc.dockingModel &&
        m_desc.dockingModel->IsDockAreaCollapsed(EditorUIPanelDockArea::Bottom);
    const float bottomCollapsedHeight =
        std::min(std::max(24.0f, m_lastBuildStats.titleBarHeight + 2.0f),
                 work.height * 0.18f);
    const float bottomHeight =
        m_bottomArea.count > 0
            ? (bottomCollapsed
                   ? bottomCollapsedHeight
                   : ClampSize(work.height * bottomRatio,
                               work.height < 720.0f ? 104.0f : 132.0f,
                               work.height * (work.height < 720.0f ? 0.34f : 0.40f)))
            : 0.0f;
    const float mainHeight = std::max(0.0f, work.height - bottomHeight);

    m_leftArea.bounds = UI::Rect(work.x, work.y, leftWidth, mainHeight);
    m_rightArea.bounds = UI::Rect(work.Right() - rightWidth, work.y, rightWidth, mainHeight);
    m_centerArea.bounds = UI::Rect(work.x + leftWidth, work.y, centerWidth, mainHeight);
    m_bottomArea.bounds =
        UI::Rect(work.x + leftWidth, work.y + mainHeight, centerWidth, bottomHeight);
    m_floatingArea.bounds = work;
    m_lastBuildStats.leftAreaWidth = leftWidth;
    m_lastBuildStats.rightAreaWidth = rightWidth;
    m_lastBuildStats.bottomAreaHeight = bottomHeight;
    m_lastBuildStats.bottomAreaCollapsed = bottomCollapsed;
}

void EditorPanelSurfaceRenderer::BuildPanelRects()
{
    BuildLinearAreaPanelRects(EditorUIPanelDockArea::Left, m_leftArea, false);
    BuildLinearAreaPanelRects(EditorUIPanelDockArea::Right, m_rightArea, false);
    BuildLinearAreaPanelRects(EditorUIPanelDockArea::Bottom, m_bottomArea, true);
    BuildLinearAreaPanelRects(EditorUIPanelDockArea::Center, m_centerArea, false);
    BuildFloatingPanelRects();
}

void EditorPanelSurfaceRenderer::BuildLinearAreaPanelRects(EditorUIPanelDockArea area,
                                                           const AreaLayout& layout,
                                                           bool horizontal)
{
    if (!m_desc.dockingModel || layout.count == 0 ||
        layout.bounds.width <= 0.0f || layout.bounds.height <= 0.0f)
    {
        return;
    }

    std::vector<const EditorDockPanelPlacement*> placements;
    placements.reserve(layout.count);
    for (const EditorDockPanelPlacement& placement : m_desc.dockingModel->GetPlacements())
    {
        if (placement.visible && placement.area == area)
        {
            placements.push_back(&placement);
        }
    }

    if (placements.empty())
    {
        return;
    }

    std::stable_sort(placements.begin(), placements.end(), ComparePlacementOrder);

    std::vector<LinearLayoutItem> layoutItems;
    layoutItems.reserve(placements.size());
    std::unordered_set<std::string> processedTabStacks;
    for (const EditorDockPanelPlacement* placement : placements)
    {
        if (!placement)
        {
            continue;
        }

        const bool tabStacked = !placement->tabStackId.empty();
        if (!tabStacked)
        {
            layoutItems.push_back({placement, ClampPanelWeight(placement->normalizedSize)});
            continue;
        }

        if (!processedTabStacks.insert(placement->tabStackId).second)
        {
            continue;
        }

        const std::string activePanelId =
            m_desc.dockingModel->ResolveActiveDockTab(placement->tabStackId);
        const EditorDockPanelPlacement* activePlacement =
            activePanelId.empty() ? nullptr : m_desc.dockingModel->FindPlacement(activePanelId);
        if (!activePlacement || activePlacement->area != area ||
            activePlacement->tabStackId != placement->tabStackId)
        {
            activePlacement = placement;
        }
        layoutItems.push_back(
            {activePlacement, ClampPanelWeight(activePlacement->normalizedSize)});
    }

    if (layoutItems.empty())
    {
        return;
    }

    float totalWeight = 0.0f;
    for (const LinearLayoutItem& item : layoutItems)
    {
        totalWeight += item.weight;
    }
    if (totalWeight <= 0.0f)
    {
        totalWeight = static_cast<float>(layoutItems.size());
    }

    const float totalLength = horizontal ? layout.bounds.width : layout.bounds.height;
    float cursor = horizontal ? layout.bounds.x : layout.bounds.y;
    float remaining = totalLength;
    for (size_t index = 0; index < layoutItems.size(); ++index)
    {
        const LinearLayoutItem& item = layoutItems[index];
        const EditorDockPanelPlacement* placement = item.placement;
        if (!placement)
        {
            continue;
        }

        const bool isLast = index + 1u == layoutItems.size();
        const float length = isLast
                                 ? remaining
                                 : std::max(0.0f, totalLength * (item.weight / totalWeight));

        if (horizontal)
        {
            m_panelRects[placement->panelId] =
                UI::Rect(cursor, layout.bounds.y, length, layout.bounds.height);
        }
        else
        {
            m_panelRects[placement->panelId] =
                UI::Rect(layout.bounds.x, cursor, layout.bounds.width, length);
        }

        cursor += length;
        remaining = std::max(0.0f, remaining - length);
    }
}

void EditorPanelSurfaceRenderer::BuildFloatingPanelRects()
{
    if (!m_desc.dockingModel || m_floatingArea.count == 0 ||
        m_floatingArea.bounds.width <= 0.0f || m_floatingArea.bounds.height <= 0.0f)
    {
        return;
    }

    std::vector<const EditorDockPanelPlacement*> placements;
    placements.reserve(m_floatingArea.count);
    for (const EditorDockPanelPlacement& placement : m_desc.dockingModel->GetPlacements())
    {
        if (placement.visible && placement.area == EditorUIPanelDockArea::Floating)
        {
            placements.push_back(&placement);
        }
    }

    std::stable_sort(placements.begin(), placements.end(), ComparePlacementOrder);

    for (size_t index = 0; index < placements.size(); ++index)
    {
        const EditorDockPanelPlacement* placement = placements[index];
        if (placement->hasFloatingBounds)
        {
            UI::Rect bounds = MakeFloatingPanelRect(placement->floatingBounds,
                                                    m_floatingArea.bounds);
            bounds.width = ClampSize(bounds.width, 320.0f, m_floatingArea.bounds.width);
            bounds.height = ClampSize(bounds.height, 220.0f, m_floatingArea.bounds.height);
            bounds.x = std::clamp(bounds.x,
                                  m_floatingArea.bounds.x,
                                  std::max(m_floatingArea.bounds.x,
                                           m_floatingArea.bounds.Right() - bounds.width));
            bounds.y = std::clamp(bounds.y,
                                  m_floatingArea.bounds.y,
                                  std::max(m_floatingArea.bounds.y,
                                           m_floatingArea.bounds.Bottom() - bounds.height));
            m_panelRects[placement->panelId] = bounds;
            continue;
        }

        const float sizeRatio = placement->normalizedSize > 0.0f &&
                                        placement->normalizedSize < 1.0f
                                    ? placement->normalizedSize
                                    : 0.32f;
        const float width = ClampSize(m_floatingArea.bounds.width * sizeRatio,
                                      320.0f,
                                      m_floatingArea.bounds.width);
        const float height = ClampSize(m_floatingArea.bounds.height * sizeRatio,
                                       220.0f,
                                       m_floatingArea.bounds.height);
        const float offset = 18.0f * static_cast<float>(index);
        const float centeredX =
            m_floatingArea.bounds.x +
            (m_floatingArea.bounds.width - width) * 0.5f + offset;
        const float preferredY =
            m_floatingArea.bounds.y +
            std::min(std::max(36.0f, m_floatingArea.bounds.height * 0.08f),
                     96.0f) +
            offset;
        const float x =
            std::clamp(centeredX,
                       m_floatingArea.bounds.x,
                       std::max(m_floatingArea.bounds.x,
                                m_floatingArea.bounds.Right() - width));
        const float y =
            std::clamp(preferredY,
                       m_floatingArea.bounds.y,
                       std::max(m_floatingArea.bounds.y,
                                m_floatingArea.bounds.Bottom() - height));
        m_panelRects[placement->panelId] =
            UI::Rect(x, y, width, height);
    }
}

void EditorPanelSurfaceRenderer::AddSplitterHandles()
{
    if (!m_rootPanel || !m_desc.host || !m_desc.dockingModel)
    {
        return;
    }

    const UI::Rect work = m_lastBuildStats.workArea;
    if (work.width <= 0.0f || work.height <= 0.0f)
    {
        return;
    }

    const UI::UITheme& theme = m_desc.ui->GetTheme();
    const EditorPanelChromeStyle chrome = BuildEditorPanelChromeStyle(theme);
    const UI::UIColor splitterColor = chrome.splitter;
    const float thickness = std::max(5.0f, theme.metrics.borderWidth * 5.0f);

    auto addSplitter = [this](std::shared_ptr<EditorDockDragWidget> splitter) {
        if (splitter)
        {
            m_rootPanel->AddChild(std::move(splitter));
            ++m_lastBuildStats.splitterHandleCount;
        }
    };

    if (m_leftArea.count > 0 && m_centerArea.bounds.width > 1.0f &&
        m_leftArea.bounds.width > 1.0f)
    {
        const UI::Rect bounds(m_leftArea.bounds.Right() - thickness * 0.5f,
                              m_leftArea.bounds.y,
                              thickness,
                              m_leftArea.bounds.height);
        addSplitter(CreateDragPanel(
            "Editor.PanelSurface.Splitter.Left",
            bounds,
            splitterColor,
            [host = m_desc.host, workWidth = work.width](const UI::UIEvent& event) {
                if (!host || workWidth <= 0.0f)
                {
                    return;
                }

                EditorDockingModel& model = host->GetDockingModel();
                model.SetDockAreaSize(
                    EditorUIPanelDockArea::Left,
                    model.GetDockAreaSize(EditorUIPanelDockArea::Left) +
                        event.delta.x / workWidth);
            }));
    }

    if (m_rightArea.count > 0 && m_centerArea.bounds.width > 1.0f &&
        m_rightArea.bounds.width > 1.0f)
    {
        const UI::Rect bounds(m_rightArea.bounds.x - thickness * 0.5f,
                              m_rightArea.bounds.y,
                              thickness,
                              m_rightArea.bounds.height);
        addSplitter(CreateDragPanel(
            "Editor.PanelSurface.Splitter.Right",
            bounds,
            splitterColor,
            [host = m_desc.host, workWidth = work.width](const UI::UIEvent& event) {
                if (!host || workWidth <= 0.0f)
                {
                    return;
                }

                EditorDockingModel& model = host->GetDockingModel();
                model.SetDockAreaSize(
                    EditorUIPanelDockArea::Right,
                    model.GetDockAreaSize(EditorUIPanelDockArea::Right) -
                        event.delta.x / workWidth);
            }));
    }

    if (m_bottomArea.count > 0 && m_centerArea.bounds.width > 1.0f &&
        m_bottomArea.bounds.height > 1.0f)
    {
        const UI::Rect bounds(m_bottomArea.bounds.x,
                              m_bottomArea.bounds.y - thickness * 0.5f,
                              m_bottomArea.bounds.width,
                              thickness);
        addSplitter(CreateDragPanel(
            "Editor.PanelSurface.Splitter.Bottom",
            bounds,
            splitterColor,
            [host = m_desc.host, workHeight = work.height](const UI::UIEvent& event) {
                if (!host || workHeight <= 0.0f)
                {
                    return;
                }

                EditorDockingModel& model = host->GetDockingModel();
                model.SetDockAreaSize(
                    EditorUIPanelDockArea::Bottom,
                    model.GetDockAreaSize(EditorUIPanelDockArea::Bottom) -
                        event.delta.y / workHeight);
            }));
    }
}

void EditorPanelSurfaceRenderer::AddDockDropPreviewOverlay()
{
    if (!m_rootPanel || !m_desc.ui || !m_desc.dockingModel)
    {
        return;
    }

    const EditorDockDropPreview& preview =
        m_desc.dockingModel->GetDockDropPreview();
    if (!preview.active)
    {
        return;
    }

    UI::Rect bounds;
    std::string overlayName = std::string("Editor.PanelSurface.DockDropPreview.") +
        DockDropAreaSuffix(preview.area);
    if (!preview.tabStackId.empty())
    {
        for (const EditorPanelSurfaceTabStackDropTarget& target : m_tabStackDropTargets)
        {
            if (target.stackId == preview.tabStackId && target.area == preview.area)
            {
                bounds = target.bounds;
                overlayName = std::string("Editor.PanelSurface.DockDropPreview.TabStack.") +
                    preview.tabStackId;
                break;
            }
        }
    }
    else
    {
        bounds = MakeDockDropZoneRect(preview.area, m_lastBuildStats.workArea);
    }
    if (bounds.width <= 1.0f || bounds.height <= 1.0f)
    {
        return;
    }

    const UI::UITheme& theme = m_desc.ui->GetTheme();
    UI::Panel::Ptr overlay = CreatePanel(overlayName,
                                         bounds,
                                         theme.colors.accent.WithAlpha(0.18f),
                                         theme.colors.accentHover.WithAlpha(0.95f),
                                         std::max(2.0f, theme.metrics.borderWidth * 2.0f));
    overlay->SetInteractive(false);
    m_rootPanel->AddChild(std::move(overlay));
    ++m_lastBuildStats.dockDropPreviewCount;
    if (!preview.tabStackId.empty())
    {
        ++m_lastBuildStats.dockTabStackDropPreviewCount;
    }
}

} // namespace RVX::Editor
