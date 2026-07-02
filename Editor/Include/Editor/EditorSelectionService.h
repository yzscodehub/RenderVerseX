/**
 * @file EditorSelectionService.h
 * @brief Shared editor selection service boundary.
 */

#pragma once

#include "Editor/EditorContext.h"

namespace RVX::Editor
{

/**
 * @brief Service boundary for editor selection state and selection commands.
 */
class EditorSelectionService
{
public:
    // =========================================================================
    // Construction
    // =========================================================================

    EditorSelectionService();
    explicit EditorSelectionService(EditorContext& context);

    void SetContext(EditorContext& context) { m_context = &context; }
    EditorContext& GetContext() const;

    // =========================================================================
    // Query
    // =========================================================================

    SelectionType GetSelectionType() const;
    uint64 GetSelectionRevision() const;
    SceneEntity* GetSelectedEntity() const;
    const std::vector<SceneEntity*>& GetSelectedEntities() const;
    bool IsSelected(SceneEntity* entity) const;
    Tools::AssetGUID GetSelectedAsset() const;
    SceneManager* GetActiveSceneManager() const;

    // =========================================================================
    // Commands
    // =========================================================================

    bool SelectEntity(SceneEntity* entity);
    bool SelectEntities(const std::vector<SceneEntity*>& entities);
    bool SelectAsset(const Tools::AssetGUID& guid);
    bool ClearSelection();

private:
    EditorContext* m_context = nullptr;
};

} // namespace RVX::Editor
