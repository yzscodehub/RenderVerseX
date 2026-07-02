/**
 * @file NativeSceneHierarchy.h
 * @brief Native UI scene hierarchy panel
 */

#pragma once

#include "Editor/UI/EditorPanelContentCache.h"
#include "Editor/UI/EditorTreeList.h"
#include "Editor/UI/EditorUIPanel.h"

#include <string>
#include <vector>

namespace RVX
{
    class SceneEntity;
}

namespace RVX::Editor
{

struct NativeSceneHierarchyStats
{
    uint32 totalEntityCount = 0;
    uint32 visibleRowCount = 0;
    uint32 sceneSnapshotRefreshCount = 0;
    uint32 visibleSnapshotRefreshCount = 0;
    uint64 sceneRevision = 0;
    uint64 selectionRevision = 0;
    bool reusedSceneSnapshot = false;
    bool reusedVisibleSnapshot = false;
    bool selectedRowVisible = false;
};

struct NativeSceneHierarchyNodeSnapshot
{
    uint32 handle = ~0u;
    std::string name;
    uint32 depth = 0;
    bool active = true;
    bool expandable = false;
};

class NativeSceneHierarchyPanel final : public IEditorUIPanel
{
public:
    NativeSceneHierarchyPanel();

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void BuildUI(EditorUIPanelFrameContext& context) override;
    const NativeSceneHierarchyStats& GetLastBuildStats() const { return m_lastBuildStats; }

private:
    void AddToolbar(EditorUIPanelFrameContext& context);
    void AddSceneRows(EditorUIPanelFrameContext& context);
    void EnsureSceneSnapshots(EditorUIPanelFrameContext& context);
    void RefreshSceneSnapshot(EditorUIPanelFrameContext& context);
    void AppendEntitySnapshot(SceneEntity* entity, uint32 depth);
    void RefreshVisibleSceneSnapshot(EditorUIPanelFrameContext& context);
    bool SnapshotSubtreeMatchesFilter(size_t index,
                                      const std::string& normalizedFilter) const;
    void OpenEntityContextMenu(EditorUIHost& host, SceneEntity& entity, const Vec2& anchor);

    EditorUIPanelDesc m_desc;
    NativeSceneHierarchyStats m_lastBuildStats;
    EditorPanelContentCache m_sceneSnapshotCache;
    EditorPanelContentCache m_visibleSnapshotCache;
    std::vector<NativeSceneHierarchyNodeSnapshot> m_sceneSnapshots;
    std::vector<size_t> m_visibleSnapshotIndices;
    std::string m_sceneSnapshotKey;
    EditorTreeList m_treeList;
    std::string m_searchFilter;
};

} // namespace RVX::Editor
