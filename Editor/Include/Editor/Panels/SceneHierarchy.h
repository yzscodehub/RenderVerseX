/**
 * @file SceneHierarchy.h
 * @brief Scene hierarchy panel
 */

#pragma once

#include "Editor/Panels/IEditorPanel.h"
#include <string>

namespace RVX
{
    class Entity;
    class Scene;
}

namespace RVX::Editor
{

/**
 * @brief Scene hierarchy panel showing entity tree
 */
class SceneHierarchyPanel : public IEditorPanel
{
public:
    SceneHierarchyPanel();
    ~SceneHierarchyPanel() override = default;

    // =========================================================================
    // IEditorPanel Interface
    // =========================================================================

    const char* GetName() const override { return "Hierarchy"; }
    const char* GetIcon() const override { return "hierarchy"; }
    void OnGUI() override;

private:
    void DrawToolbar();
    void DrawSceneTree();
    void DrawEntityNode(Entity* entity, int depth = 0);
    void DrawContextMenu(Entity* entity);
    void DrawCreateEntityMenu();

    void HandleDragDrop(Entity* entity);
    bool PassesFilter(Entity* entity) const;

    // State
    std::string m_searchFilter;
    bool m_showHidden = false;
    Entity* m_renamingEntity = nullptr;
    char m_renameBuffer[256];
    bool m_expandAll = false;
    bool m_collapseAll = false;
};

} // namespace RVX::Editor
