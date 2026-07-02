/**
 * @file EditorWorkspaceLayoutPolicy.cpp
 * @brief Native editor default workspace layout policy implementation
 */

#include "Editor/UI/EditorWorkspaceLayoutPolicy.h"

#include <algorithm>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_WORKSPACE_LEFT_STACK =
        "workspace.left.scene";
    constexpr const char* RVX_EDITOR_WORKSPACE_RIGHT_STACK =
        "workspace.right.details";
    constexpr const char* RVX_EDITOR_WORKSPACE_BOTTOM_DRAWER_STACK =
        "workspace.bottom.drawer";

    constexpr uint32 RVX_EDITOR_WORKSPACE_DEFAULT_ORDER_BASE = 100u;

    bool IsPanelId(const EditorUIPanelDesc& desc, const char* panelId)
    {
        return desc.id == panelId;
    }

    bool IsDefaultStackArea(EditorUIPanelDockArea area)
    {
        return area == EditorUIPanelDockArea::Left ||
               area == EditorUIPanelDockArea::Right ||
               area == EditorUIPanelDockArea::Bottom;
    }

    bool UsesLeftWorkspaceStack(const EditorUIPanelDesc& desc)
    {
        return IsPanelId(desc, "native.sceneHierarchy");
    }

    bool UsesRightWorkspaceStack(const EditorUIPanelDesc& desc)
    {
        return IsPanelId(desc, "native.inspector") ||
               IsPanelId(desc, "native.materialEditor");
    }

    bool UsesBottomWorkspaceDrawerStack(const EditorUIPanelDesc& desc)
    {
        return IsPanelId(desc, "native.assetBrowser") ||
               IsPanelId(desc, "native.console") ||
               IsPanelId(desc, "native.animationEditor");
    }
}

EditorWorkspaceLayoutPreset EditorWorkspaceLayoutPolicy::GetDefaultPreset() const
{
    EditorWorkspaceLayoutPreset preset;
    preset.areaSizing.leftWidthRatio = 0.18f;
    preset.areaSizing.rightWidthRatio = 0.22f;
    preset.areaSizing.bottomHeightRatio = 0.24f;
    preset.leftStackId = RVX_EDITOR_WORKSPACE_LEFT_STACK;
    preset.rightStackId = RVX_EDITOR_WORKSPACE_RIGHT_STACK;
    preset.bottomDrawerStackId = RVX_EDITOR_WORKSPACE_BOTTOM_DRAWER_STACK;
    return preset;
}

EditorDockPanelPlacement EditorWorkspaceLayoutPolicy::BuildDefaultPlacement(
    const EditorUIPanelDesc& desc,
    bool visible,
    uint32 fallbackOrder) const
{
    EditorDockPanelPlacement placement;
    placement.panelId = desc.id;
    placement.area = desc.defaultDockArea;
    placement.tabStackId = ResolveDefaultTabStackId(desc);
    placement.normalizedSize = ResolveDefaultPanelWeight(desc);
    placement.order = ResolveDefaultOrder(desc, fallbackOrder);
    placement.visible = visible;
    return placement;
}

std::string EditorWorkspaceLayoutPolicy::ResolveDefaultTabStackId(
    const EditorUIPanelDesc& desc) const
{
    const EditorWorkspaceLayoutPreset preset = GetDefaultPreset();
    if (desc.defaultDockArea == EditorUIPanelDockArea::Left &&
        UsesLeftWorkspaceStack(desc))
    {
        return preset.leftStackId;
    }
    if (desc.defaultDockArea == EditorUIPanelDockArea::Right &&
        UsesRightWorkspaceStack(desc))
    {
        return preset.rightStackId;
    }
    if (desc.defaultDockArea == EditorUIPanelDockArea::Bottom &&
        UsesBottomWorkspaceDrawerStack(desc))
    {
        return preset.bottomDrawerStackId;
    }

    return {};
}

float EditorWorkspaceLayoutPolicy::ResolveDefaultPanelWeight(
    const EditorUIPanelDesc& desc) const
{
    if (!IsDefaultStackArea(desc.defaultDockArea))
    {
        return 1.0f;
    }

    return 1.0f;
}

uint32 EditorWorkspaceLayoutPolicy::ResolveDefaultOrder(
    const EditorUIPanelDesc& desc,
    uint32 fallbackOrder) const
{
    if (IsPanelId(desc, "native.viewport"))
    {
        return 10u;
    }
    if (IsPanelId(desc, "native.sceneHierarchy"))
    {
        return 100u;
    }
    if (IsPanelId(desc, "native.inspector"))
    {
        return 100u;
    }
    if (IsPanelId(desc, "native.materialEditor"))
    {
        return 120u;
    }
    if (IsPanelId(desc, "native.preferences"))
    {
        return 900u;
    }
    if (IsPanelId(desc, "native.panelCatalog"))
    {
        return 920u;
    }
    if (IsPanelId(desc, "native.assetBrowser"))
    {
        return 100u;
    }
    if (IsPanelId(desc, "native.console"))
    {
        return 120u;
    }
    if (IsPanelId(desc, "native.animationEditor"))
    {
        return 140u;
    }
    if (IsPanelId(desc, "native.commandHistory"))
    {
        return 900u;
    }

    return RVX_EDITOR_WORKSPACE_DEFAULT_ORDER_BASE +
           std::min(fallbackOrder, 900u);
}

} // namespace RVX::Editor
