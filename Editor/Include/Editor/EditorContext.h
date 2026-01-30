/**
 * @file EditorContext.h
 * @brief Editor context with shared state
 */

#pragma once

#include "Core/Types.h"
#include "Tools/AssetDatabase.h"
#include <memory>
#include <vector>
#include <functional>

namespace RVX
{
    class Scene;
    class Entity;
}

namespace RVX::Editor
{

class EditorWindow;

/**
 * @brief Selection types
 */
enum class SelectionType : uint8
{
    None,
    Entity,
    Asset,
    Component
};

/**
 * @brief Selection changed callback
 */
using SelectionChangedCallback = std::function<void()>;

/**
 * @brief Editor context holds shared editor state
 */
class EditorContext
{
public:
    static EditorContext& Get();

    // =========================================================================
    // Project
    // =========================================================================

    bool OpenProject(const std::string& projectPath);
    void CloseProject();
    bool HasProject() const { return !m_projectPath.empty(); }
    const std::string& GetProjectPath() const { return m_projectPath; }

    // =========================================================================
    // Scene
    // =========================================================================

    void SetActiveScene(std::shared_ptr<Scene> scene);
    std::shared_ptr<Scene> GetActiveScene() const { return m_activeScene; }

    bool LoadScene(const std::string& path);
    bool SaveScene(const std::string& path);
    void NewScene();

    bool IsSceneDirty() const { return m_sceneDirty; }
    void MarkSceneDirty() { m_sceneDirty = true; }

    // =========================================================================
    // Selection
    // =========================================================================

    SelectionType GetSelectionType() const { return m_selectionType; }

    void SelectEntity(Entity* entity);
    void SelectEntities(const std::vector<Entity*>& entities);
    void ClearSelection();

    Entity* GetSelectedEntity() const;
    const std::vector<Entity*>& GetSelectedEntities() const { return m_selectedEntities; }
    bool IsSelected(Entity* entity) const;

    void SelectAsset(const Tools::AssetGUID& guid);
    Tools::AssetGUID GetSelectedAsset() const { return m_selectedAsset; }

    void AddSelectionChangedCallback(SelectionChangedCallback callback);

    // =========================================================================
    // Asset Database
    // =========================================================================

    Tools::AssetDatabase& GetAssetDatabase() { return m_assetDatabase; }

    // =========================================================================
    // Playmode
    // =========================================================================

    bool IsPlaying() const { return m_isPlaying; }
    bool IsPaused() const { return m_isPaused; }

    void Play();
    void Pause();
    void Stop();
    void Step();

    // =========================================================================
    // Undo/Redo
    // =========================================================================

    void BeginUndoGroup(const std::string& name);
    void EndUndoGroup();

    bool CanUndo() const;
    bool CanRedo() const;
    void Undo();
    void Redo();

    // =========================================================================
    // Gizmo
    // =========================================================================

    enum class GizmoMode : uint8
    {
        Translate,
        Rotate,
        Scale
    };

    enum class GizmoSpace : uint8
    {
        Local,
        World
    };

    GizmoMode GetGizmoMode() const { return m_gizmoMode; }
    void SetGizmoMode(GizmoMode mode) { m_gizmoMode = mode; }

    GizmoSpace GetGizmoSpace() const { return m_gizmoSpace; }
    void SetGizmoSpace(GizmoSpace space) { m_gizmoSpace = space; }

    // =========================================================================
    // Grid & Snapping
    // =========================================================================

    bool IsSnapEnabled() const { return m_snapEnabled; }
    void SetSnapEnabled(bool enabled) { m_snapEnabled = enabled; }

    float GetSnapValue() const { return m_snapValue; }
    void SetSnapValue(float value) { m_snapValue = value; }

private:
    EditorContext() = default;

    std::string m_projectPath;
    std::shared_ptr<Scene> m_activeScene;
    bool m_sceneDirty = false;

    SelectionType m_selectionType = SelectionType::None;
    std::vector<Entity*> m_selectedEntities;
    Tools::AssetGUID m_selectedAsset;
    std::vector<SelectionChangedCallback> m_selectionCallbacks;

    Tools::AssetDatabase m_assetDatabase;

    bool m_isPlaying = false;
    bool m_isPaused = false;

    GizmoMode m_gizmoMode = GizmoMode::Translate;
    GizmoSpace m_gizmoSpace = GizmoSpace::World;
    bool m_snapEnabled = false;
    float m_snapValue = 1.0f;
};

} // namespace RVX::Editor
