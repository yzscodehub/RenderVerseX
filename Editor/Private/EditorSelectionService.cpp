/**
 * @file EditorSelectionService.cpp
 * @brief Shared editor selection service boundary implementation.
 */

#include "Editor/EditorSelectionService.h"

namespace RVX::Editor
{

EditorSelectionService::EditorSelectionService()
    : m_context(&EditorContext::Get())
{
}

EditorSelectionService::EditorSelectionService(EditorContext& context)
    : m_context(&context)
{
}

EditorContext& EditorSelectionService::GetContext() const
{
    return m_context ? *m_context : EditorContext::Get();
}

SelectionType EditorSelectionService::GetSelectionType() const
{
    return GetContext().GetSelectionType();
}

uint64 EditorSelectionService::GetSelectionRevision() const
{
    return GetContext().GetSelectionRevision();
}

SceneEntity* EditorSelectionService::GetSelectedEntity() const
{
    return GetContext().GetSelectedEntity();
}

const std::vector<SceneEntity*>& EditorSelectionService::GetSelectedEntities() const
{
    return GetContext().GetSelectedEntities();
}

bool EditorSelectionService::IsSelected(SceneEntity* entity) const
{
    return GetContext().IsSelected(entity);
}

Tools::AssetGUID EditorSelectionService::GetSelectedAsset() const
{
    return GetContext().GetSelectedAsset();
}

SceneManager* EditorSelectionService::GetActiveSceneManager() const
{
    return GetContext().GetActiveSceneManager();
}

bool EditorSelectionService::SelectEntity(SceneEntity* entity)
{
    GetContext().SelectEntity(entity);
    return GetContext().GetSelectedEntity() == entity;
}

bool EditorSelectionService::SelectEntities(
    const std::vector<SceneEntity*>& entities)
{
    GetContext().SelectEntities(entities);
    return GetContext().GetSelectedEntities().size() == entities.size();
}

bool EditorSelectionService::SelectAsset(const Tools::AssetGUID& guid)
{
    GetContext().SelectAsset(guid);
    return GetContext().GetSelectionType() == SelectionType::Asset &&
           GetContext().GetSelectedAsset() == guid;
}

bool EditorSelectionService::ClearSelection()
{
    GetContext().ClearSelection();
    return GetContext().GetSelectionType() == SelectionType::None &&
           GetContext().GetSelectedEntity() == nullptr;
}

} // namespace RVX::Editor
