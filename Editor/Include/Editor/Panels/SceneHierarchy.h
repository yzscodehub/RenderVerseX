/**
 * @file SceneHierarchy.h
 * @brief Scene hierarchy panel
 */

#pragma once

#include "Editor/Panels/IEditorPanel.h"

#include <string>
#include <vector>

namespace RVX
{
    class SceneEntity;
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
    void OnNativeInput(const UI::UIInputState& input) override;

private:
    struct EntityRowHit
    {
        SceneEntity* entity = nullptr;
        UI::Rect bounds;
    };

    void DrawToolbar();
    void DrawSceneTree();
    void DrawEntityNode(SceneEntity* entity, int depth = 0);
    void DrawContextMenu(SceneEntity* entity);
    void DrawCreateEntityMenu();

    void HandleDragDrop(SceneEntity* entity);
    bool PassesFilter(SceneEntity* entity) const;

    // State
    std::string m_searchFilter;
    bool m_showHidden = false;
    SceneEntity* m_renamingEntity = nullptr;
    UI::Rect m_renameEditBounds;
    char m_renameBuffer[256];
    std::vector<EntityRowHit> m_entityRows;
    bool m_expandAll = false;
    bool m_collapseAll = false;
};

} // namespace RVX::Editor
