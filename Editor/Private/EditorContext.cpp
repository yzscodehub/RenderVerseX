/**
 * @file EditorContext.cpp
 * @brief EditorContext implementation
 */

#include "Editor/EditorContext.h"
#include "Core/Log.h"
#include <algorithm>
#include <filesystem>

namespace RVX::Editor
{

EditorContext& EditorContext::Get()
{
    static EditorContext instance;
    return instance;
}

bool EditorContext::OpenProject(const std::string& projectPath)
{
    m_projectPath = projectPath;

    // Initialize asset database
    std::filesystem::path assetsPath = std::filesystem::path(projectPath) / "Assets";
    std::filesystem::path importedPath = std::filesystem::path(projectPath) / "Library";

    if (std::filesystem::exists(assetsPath))
    {
        m_assetDatabase.Initialize(assetsPath, importedPath);
    }

    RVX_CORE_INFO("Opened project: {}", projectPath);
    return true;
}

void EditorContext::CloseProject()
{
    m_projectPath.clear();
    m_activeScene.reset();
    ClearSelection();
}

void EditorContext::SetActiveScene(std::shared_ptr<Scene> scene)
{
    m_activeScene = std::move(scene);
    m_sceneDirty = false;
    ClearSelection();
}

bool EditorContext::LoadScene(const std::string& path)
{
    // TODO: Implement scene loading
    RVX_CORE_INFO("Loading scene: {}", path);
    return true;
}

bool EditorContext::SaveScene(const std::string& path)
{
    // TODO: Implement scene saving
    m_sceneDirty = false;
    RVX_CORE_INFO("Saved scene: {}", path);
    return true;
}

void EditorContext::NewScene()
{
    // TODO: Create new scene
    m_sceneDirty = false;
    ClearSelection();
}

void EditorContext::SelectEntity(Entity* entity)
{
    m_selectedEntities.clear();
    if (entity)
    {
        m_selectedEntities.push_back(entity);
    }
    m_selectionType = entity ? SelectionType::Entity : SelectionType::None;

    for (auto& callback : m_selectionCallbacks)
    {
        callback();
    }
}

void EditorContext::SelectEntities(const std::vector<Entity*>& entities)
{
    m_selectedEntities = entities;
    m_selectionType = entities.empty() ? SelectionType::None : SelectionType::Entity;

    for (auto& callback : m_selectionCallbacks)
    {
        callback();
    }
}

void EditorContext::ClearSelection()
{
    m_selectedEntities.clear();
    m_selectedAsset = Tools::AssetGUID();
    m_selectionType = SelectionType::None;

    for (auto& callback : m_selectionCallbacks)
    {
        callback();
    }
}

Entity* EditorContext::GetSelectedEntity() const
{
    return m_selectedEntities.empty() ? nullptr : m_selectedEntities[0];
}

bool EditorContext::IsSelected(Entity* entity) const
{
    return std::find(m_selectedEntities.begin(), m_selectedEntities.end(), entity) 
           != m_selectedEntities.end();
}

void EditorContext::SelectAsset(const Tools::AssetGUID& guid)
{
    m_selectedAsset = guid;
    m_selectionType = SelectionType::Asset;
    m_selectedEntities.clear();

    for (auto& callback : m_selectionCallbacks)
    {
        callback();
    }
}

void EditorContext::AddSelectionChangedCallback(SelectionChangedCallback callback)
{
    m_selectionCallbacks.push_back(std::move(callback));
}

void EditorContext::Play()
{
    if (!m_isPlaying)
    {
        // TODO: Save scene state before play
        m_isPlaying = true;
        m_isPaused = false;
        RVX_CORE_INFO("Entering play mode");
    }
}

void EditorContext::Pause()
{
    if (m_isPlaying)
    {
        m_isPaused = !m_isPaused;
    }
}

void EditorContext::Stop()
{
    if (m_isPlaying)
    {
        m_isPlaying = false;
        m_isPaused = false;
        // TODO: Restore scene state
        RVX_CORE_INFO("Exiting play mode");
    }
}

void EditorContext::Step()
{
    if (m_isPlaying && m_isPaused)
    {
        // TODO: Single frame step
    }
}

void EditorContext::BeginUndoGroup(const std::string& name)
{
    // TODO: Implement undo system
}

void EditorContext::EndUndoGroup()
{
    // TODO: Implement undo system
}

bool EditorContext::CanUndo() const
{
    // TODO: Implement undo system
    return false;
}

bool EditorContext::CanRedo() const
{
    // TODO: Implement undo system
    return false;
}

void EditorContext::Undo()
{
    // TODO: Implement undo system
}

void EditorContext::Redo()
{
    // TODO: Implement undo system
}

} // namespace RVX::Editor
