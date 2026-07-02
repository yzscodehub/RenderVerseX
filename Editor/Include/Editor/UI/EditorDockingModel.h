/**
 * @file EditorDockingModel.h
 * @brief Native editor dock layout model
 */

#pragma once

#include "Editor/UI/EditorUIPanel.h"

#include <string>
#include <vector>

namespace RVX::Editor
{

struct EditorDockFloatingBounds
{
    float x = 0.08f;
    float y = 0.08f;
    float width = 0.32f;
    float height = 0.32f;
};

struct EditorDockPanelPlacement
{
    std::string panelId;
    EditorUIPanelDockArea area = EditorUIPanelDockArea::Floating;
    std::string tabStackId;
    float normalizedSize = 1.0f;
    uint32 order = 0;
    bool visible = true;
    bool hasFloatingBounds = false;
    EditorDockFloatingBounds floatingBounds;
    uint32 floatingZOrder = 0;
};

struct EditorDockAreaSizing
{
    float leftWidthRatio = 0.18f;
    float rightWidthRatio = 0.20f;
    float bottomHeightRatio = 0.24f;
};

struct EditorDockDropPreview
{
    bool active = false;
    std::string panelId;
    EditorUIPanelDockArea area = EditorUIPanelDockArea::Floating;
    std::string tabStackId;
};

struct EditorDockTabStackState
{
    std::string stackId;
    std::string activePanelId;
};

class EditorDockingModel
{
public:
    void Clear();

    bool DockPanel(EditorDockPanelPlacement placement);
    bool UndockPanel(const std::string& panelId);

    EditorDockPanelPlacement* FindPlacement(const std::string& panelId);
    const EditorDockPanelPlacement* FindPlacement(const std::string& panelId) const;

    void SetPanelVisible(const std::string& panelId, bool visible);
    bool SetDockAreaSize(EditorUIPanelDockArea area, float normalizedSize);
    float GetDockAreaSize(EditorUIPanelDockArea area) const;
    bool SetDockAreaCollapsed(EditorUIPanelDockArea area, bool collapsed);
    bool IsDockAreaCollapsed(EditorUIPanelDockArea area) const;
    void SetAreaSizing(const EditorDockAreaSizing& sizing);
    const EditorDockAreaSizing& GetAreaSizing() const { return m_areaSizing; }
    bool SetFloatingPanelBounds(const std::string& panelId,
                                const EditorDockFloatingBounds& bounds);
    bool BringFloatingPanelToFront(const std::string& panelId);
    bool SetPanelDockArea(const std::string& panelId, EditorUIPanelDockArea area);
    bool SetPanelDockTabStack(const std::string& panelId,
                              EditorUIPanelDockArea area,
                              const std::string& stackId);
    bool SetDockDropPreview(const std::string& panelId,
                            EditorUIPanelDockArea area,
                            const std::string& tabStackId = std::string());
    void ClearDockDropPreview(const std::string& panelId = std::string());
    bool SetPanelTabStack(const std::string& panelId, const std::string& stackId);
    bool SetActiveDockTab(const std::string& panelId);
    bool MoveDockTab(const std::string& panelId,
                     const std::string& targetPanelId,
                     bool afterTarget);
    bool DetachDockTabToFloating(const std::string& panelId,
                                 const EditorDockFloatingBounds& bounds);
    std::string GetActiveDockTab(const std::string& stackId) const;
    std::string ResolveActiveDockTab(const std::string& stackId) const;

    uint32 GetPanelCount(EditorUIPanelDockArea area) const;
    uint32 GetTabStackPanelCount(const std::string& stackId) const;
    size_t GetPlacementCount() const { return m_placements.size(); }
    const std::vector<EditorDockPanelPlacement>& GetPlacements() const { return m_placements; }
    const EditorDockDropPreview& GetDockDropPreview() const { return m_dropPreview; }
    const std::vector<EditorDockTabStackState>& GetTabStackStates() const
    {
        return m_tabStackStates;
    }

private:
    static float ClampAreaSize(float value);
    static EditorDockFloatingBounds ClampFloatingBounds(
        const EditorDockFloatingBounds& bounds);
    EditorDockTabStackState* FindTabStackState(const std::string& stackId);
    const EditorDockTabStackState* FindTabStackState(const std::string& stackId) const;
    void EnsureTabStackActivePanel(const std::string& stackId);
    void RemoveEmptyTabStackStates();
    uint32 AllocateDockAreaOrder(EditorUIPanelDockArea area) const;
    uint32 AllocateFloatingZOrder();

    std::vector<EditorDockPanelPlacement> m_placements;
    EditorDockAreaSizing m_areaSizing;
    bool m_bottomAreaCollapsed = false;
    EditorDockDropPreview m_dropPreview;
    std::vector<EditorDockTabStackState> m_tabStackStates;
};

} // namespace RVX::Editor
