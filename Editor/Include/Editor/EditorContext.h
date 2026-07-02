/**
 * @file EditorContext.h
 * @brief Editor context with shared state
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Tools/AssetDatabase.h"
#include <any>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    class ActorComponent;
    class Property;
    class SceneEntity;
    class SceneManager;
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

enum class EditorContextChangeReason : uint32
{
    None = 0,
    Project = 1u << 0u,
    Scene = 1u << 1u,
    SceneDirty = 1u << 2u,
    Selection = 1u << 3u,
    AssetDatabase = 1u << 4u,
    PlayMode = 1u << 5u,
    UndoRedo = 1u << 6u,
    ToolSettings = 1u << 7u
};

constexpr uint32 ToEditorContextChangeReasonMask(EditorContextChangeReason reason)
{
    return static_cast<uint32>(reason);
}

inline bool HasEditorContextChangeReason(uint32 mask, EditorContextChangeReason reason)
{
    return (mask & ToEditorContextChangeReasonMask(reason)) != 0u;
}

struct EditorContextChangeEvent
{
    uint64 revision = 0;
    uint32 reasonMask = 0;
};

using EditorContextChangedCallback =
    std::function<void(const EditorContextChangeEvent& event)>;

/**
 * @brief Editor context holds shared editor state
 */
class EditorContext
{
public:
    ~EditorContext();

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

    void SetActiveSceneManager(SceneManager* sceneManager);
    SceneManager* GetActiveSceneManager() const { return m_activeSceneManager; }

    bool LoadScene(const std::string& path);
    bool SaveScene(const std::string& path);
    void NewScene();

    bool IsSceneDirty() const { return m_sceneDirty; }
    void MarkSceneDirty();

    // =========================================================================
    // Change Tracking
    // =========================================================================

    uint64 GetChangeRevision() const { return m_changeRevision; }
    uint64 GetSceneRevision() const { return m_sceneRevision; }
    uint64 GetSelectionRevision() const { return m_selectionRevision; }
    uint64 GetAssetDatabaseRevision() const { return m_assetDatabaseRevision; }
    uint64 AddChangeCallback(EditorContextChangedCallback callback);
    bool RemoveChangeCallback(uint64 callbackId);

    // =========================================================================
    // Selection
    // =========================================================================

    SelectionType GetSelectionType() const { return m_selectionType; }

    void SelectEntity(SceneEntity* entity);
    void SelectEntities(const std::vector<SceneEntity*>& entities);
    void ClearSelection();

    SceneEntity* GetSelectedEntity() const;
    const std::vector<SceneEntity*>& GetSelectedEntities() const { return m_selectedEntities; }
    bool IsSelected(SceneEntity* entity) const;

    void SelectAsset(const Tools::AssetGUID& guid);
    Tools::AssetGUID GetSelectedAsset() const { return m_selectedAsset; }

    void AddSelectionChangedCallback(SelectionChangedCallback callback);

    // =========================================================================
    // Asset Database
    // =========================================================================

    Tools::AssetDatabase& GetAssetDatabase() { return m_assetDatabase; }
    void RefreshAssetDatabase();

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

    void ExecuteUndoableAction(const std::string& name,
                               std::function<void()> undoAction,
                               std::function<void()> redoAction);
    void ClearUndoHistory();

    void SetEntityNameUndoable(SceneEntity* entity, const std::string& name);
    void SetEntityActiveUndoable(SceneEntity* entity, bool active);
    void SetEntityLayerMaskUndoable(SceneEntity* entity, uint32 layerMask);
    SceneEntity* CreateEntityUndoable(const std::string& name = "Entity",
                                      SceneEntity* parent = nullptr);
    bool DestroyEntityUndoable(SceneEntity* entity);
    void SetEntityTransformUndoable(SceneEntity* entity,
                                    const Vec3& position,
                                    const Quat& rotation,
                                    const Vec3& scale,
                                    const std::string& commandName = "Transform Entity");
    void RecordEntityTransformUndo(SceneEntity* entity,
                                   const Vec3& oldPosition,
                                   const Quat& oldRotation,
                                   const Vec3& oldScale,
                                   const Vec3& newPosition,
                                   const Quat& newRotation,
                                   const Vec3& newScale,
                                   const std::string& commandName = "Transform Entity");
    void SetEntityParentUndoable(SceneEntity* entity, SceneEntity* parent);
    void SetComponentEnabledUndoable(ActorComponent* component, bool enabled);
    ActorComponent* AddComponentUndoable(SceneEntity* entity,
                                         const std::string& className);
    bool CanRemoveComponent(const ActorComponent* component) const;
    bool RemoveComponentUndoable(ActorComponent* component);
    bool SetReflectedPropertyUndoable(const Property& prop,
                                      void* instance,
                                      const std::any& value,
                                      const std::string& commandName = "Edit Property");

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
    void SetGizmoMode(GizmoMode mode);

    GizmoSpace GetGizmoSpace() const { return m_gizmoSpace; }
    void SetGizmoSpace(GizmoSpace space);

    // =========================================================================
    // Grid & Snapping
    // =========================================================================

    bool IsSnapEnabled() const { return m_snapEnabled; }
    void SetSnapEnabled(bool enabled);

    float GetSnapValue() const { return m_snapValue; }
    void SetSnapValue(float value);

private:
    struct UndoCommand
    {
        std::string name;
        std::function<void()> undoAction;
        std::function<void()> redoAction;
    };

    struct ChangeCallbackEntry
    {
        uint64 id = 0;
        EditorContextChangedCallback callback;
    };

    EditorContext() = default;

    void EmitChange(uint32 reasonMask);
    void NotifySelectionChanged();
    void PushUndoCommand(UndoCommand command);
    void PushUndoCommandToHistory(UndoCommand command);

    std::string m_projectPath;
    std::unique_ptr<SceneManager> m_ownedScene;
    SceneManager* m_activeSceneManager = nullptr;
    bool m_sceneDirty = false;

    SelectionType m_selectionType = SelectionType::None;
    std::vector<SceneEntity*> m_selectedEntities;
    Tools::AssetGUID m_selectedAsset;
    std::vector<SelectionChangedCallback> m_selectionCallbacks;
    std::vector<ChangeCallbackEntry> m_changeCallbacks;
    uint64 m_nextChangeCallbackId = 1;
    uint64 m_changeRevision = 0;
    uint64 m_sceneRevision = 0;
    uint64 m_selectionRevision = 0;
    uint64 m_assetDatabaseRevision = 0;

    Tools::AssetDatabase m_assetDatabase;

    std::vector<UndoCommand> m_undoStack;
    std::vector<UndoCommand> m_redoStack;
    std::vector<UndoCommand> m_pendingUndoGroup;
    std::string m_pendingUndoGroupName;
    uint32 m_undoGroupDepth = 0;
    bool m_isApplyingUndoRedo = false;
    size_t m_maxUndoCommands = 128;

    bool m_isPlaying = false;
    bool m_isPaused = false;

    GizmoMode m_gizmoMode = GizmoMode::Translate;
    GizmoSpace m_gizmoSpace = GizmoSpace::World;
    bool m_snapEnabled = false;
    float m_snapValue = 1.0f;
};

} // namespace RVX::Editor
