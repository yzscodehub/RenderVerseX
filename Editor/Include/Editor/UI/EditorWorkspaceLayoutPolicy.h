/**
 * @file EditorWorkspaceLayoutPolicy.h
 * @brief Native editor default workspace layout policy
 */

#pragma once

#include "Editor/UI/EditorDockingModel.h"
#include "Editor/UI/EditorUIPanel.h"

#include <string>

namespace RVX::Editor
{

struct EditorWorkspaceLayoutPreset
{
    EditorDockAreaSizing areaSizing;
    std::string leftStackId;
    std::string rightStackId;
    std::string bottomDrawerStackId;
};

class EditorWorkspaceLayoutPolicy
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorWorkspaceLayoutPreset GetDefaultPreset() const;
    EditorDockPanelPlacement BuildDefaultPlacement(const EditorUIPanelDesc& desc,
                                                   bool visible,
                                                   uint32 fallbackOrder) const;

    std::string ResolveDefaultTabStackId(const EditorUIPanelDesc& desc) const;
    float ResolveDefaultPanelWeight(const EditorUIPanelDesc& desc) const;
    uint32 ResolveDefaultOrder(const EditorUIPanelDesc& desc,
                               uint32 fallbackOrder) const;
};

} // namespace RVX::Editor
