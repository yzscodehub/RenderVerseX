/**
 * @file NativeInspector.h
 * @brief Native UI inspector panel
 */

#pragma once

#include "Editor/EditorContext.h"
#include "Editor/UI/EditorPanelContentCache.h"
#include "Editor/UI/EditorPickerFilterModel.h"
#include "Editor/UI/EditorPickerListModel.h"
#include "Editor/UI/EditorPickerListView.h"
#include "Editor/UI/EditorUIPanel.h"
#include "Scene/ActorComponent.h"
#include "Tools/AssetDatabase.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    class ActorComponent;
    class Component;
    class SceneEntity;
}

namespace RVX::Editor
{

struct NativeInspectorStats
{
    SelectionType selectionType = SelectionType::None;
    bool hasEntity = false;
    bool hasAsset = false;
    uint32 componentCount = 0;
    uint32 actorComponentCount = 0;
    uint32 reflectedPropertyCount = 0;
    uint32 drawnPropertyCount = 0;
    uint32 unsupportedPropertyCount = 0;
    uint32 componentSnapshotCount = 0;
    uint32 componentSnapshotRefreshCount = 0;
    uint32 assetSnapshotRefreshCount = 0;
    uint64 selectionRevision = 0;
    uint64 sceneRevision = 0;
    uint64 assetDatabaseRevision = 0;
    bool reusedComponentSnapshot = false;
    bool reusedAssetSnapshot = false;
    bool hasScrollViewport = false;
    float scrollViewportHeight = 0.0f;
    float scrollContentHeight = 0.0f;
    float scrollOffsetY = 0.0f;
};

struct NativeInspectorComponentSnapshot
{
    ActorComponent::ComponentId componentId = ActorComponent::InvalidComponentId;
    std::string className;
    std::string instanceName;
};

struct NativeInspectorAssetSnapshot
{
    Tools::AssetGUID guid;
    std::string name;
    std::string path;
    Tools::AssetType type = Tools::AssetType::Unknown;
    bool isDirty = false;
    bool valid = false;
};

class NativeInspectorPanel final : public IEditorUIPanel
{
public:
    NativeInspectorPanel();

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void BuildUI(EditorUIPanelFrameContext& context) override;

    const NativeInspectorStats& GetLastBuildStats() const { return m_lastBuildStats; }

private:
    float AddEmptyState(EditorUIPanelFrameContext& context, const std::string& text);
    float AddEntityInspector(EditorUIPanelFrameContext& context, SceneEntity& entity);
    float AddAssetInspector(EditorUIPanelFrameContext& context,
                            const NativeInspectorAssetSnapshot& asset);
    void AddActorComponentInspector(EditorUIPanelFrameContext& context,
                                    ActorComponent& component,
                                    float& y,
                                    uint32 componentIndex);
    void AddLegacyComponentInspector(EditorUIPanelFrameContext& context,
                                     Component& component,
                                     float& y,
                                     uint32 componentIndex);
    void AddReflectedComponentInspector(EditorUIPanelFrameContext& context,
                                        ActorComponent& component,
                                        const std::string& componentName,
                                        const std::string& widgetPrefix,
                                        float& y);
    void AddComponentActions(EditorUIPanelFrameContext& context,
                             SceneEntity& entity,
                             float& y);
    void AddComponentPicker(EditorUIPanelFrameContext& context,
                            SceneEntity& entity,
                            float& y);
    std::string MakeComponentExpansionKey(const ActorComponent& component,
                                          const std::string& componentRootName) const;
    bool IsComponentExpanded(const ActorComponent& component,
                             const std::string& componentRootName) const;
    void SetComponentExpanded(const ActorComponent& component,
                              const std::string& componentRootName,
                              bool expanded);
    void EnsureAssetSnapshot(EditorUIPanelFrameContext& context,
                             const Tools::AssetGUID& guid);
    void RefreshAssetSnapshot(EditorUIPanelFrameContext& context,
                              const Tools::AssetGUID& guid,
                              const std::string& key);
    void EnsureComponentSnapshots(EditorUIPanelFrameContext& context,
                                  SceneEntity& entity);
    void RefreshComponentSnapshots(EditorUIPanelFrameContext& context,
                                   SceneEntity& entity,
                                   const std::string& key);
    std::string MakeComponentSnapshotKey(const SceneEntity& entity) const;
    Component* ResolveLegacyComponentSnapshot(
        SceneEntity& entity,
        const NativeInspectorComponentSnapshot& snapshot) const;
    ActorComponent* ResolveActorComponentSnapshot(
        SceneEntity& entity,
        const NativeInspectorComponentSnapshot& snapshot) const;

    EditorUIPanelDesc m_desc;
    NativeInspectorStats m_lastBuildStats;
    EditorPanelContentCache m_componentSnapshotCache;
    EditorPanelContentCache m_assetSnapshotCache;
    std::vector<NativeInspectorComponentSnapshot> m_legacyComponentSnapshots;
    std::vector<NativeInspectorComponentSnapshot> m_actorComponentSnapshots;
    NativeInspectorAssetSnapshot m_assetSnapshot;
    std::unordered_map<std::string, bool> m_componentExpansion;
    bool m_addComponentPickerOpen = false;
    bool m_addComponentPickerFocusRequested = false;
    EditorPickerFilterModel m_addComponentPickerFilterModel;
    EditorPickerListModel m_addComponentPickerModel;
    EditorPickerListView m_addComponentPickerView;
    std::string m_addComponentFilter;
    float m_scrollOffsetY = 0.0f;
};

} // namespace RVX::Editor
