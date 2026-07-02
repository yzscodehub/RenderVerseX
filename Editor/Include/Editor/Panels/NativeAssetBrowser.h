/**
 * @file NativeAssetBrowser.h
 * @brief Native UI asset browser panel
 */

#pragma once

#include "Editor/UI/EditorPanelContentCache.h"
#include "Editor/UI/EditorUIPanel.h"
#include "Tools/AssetDatabase.h"

#include <string>
#include <vector>

namespace RVX::Editor
{

struct NativeAssetBrowserStats
{
    uint32 totalAssetCount = 0;
    uint32 visibleAssetCount = 0;
    uint32 dirtyAssetCount = 0;
    uint32 assetSnapshotRefreshCount = 0;
    uint32 visibleSnapshotRefreshCount = 0;
    uint64 assetDatabaseRevision = 0;
    bool reusedAssetSnapshot = false;
    bool reusedVisibleSnapshot = false;
    bool selectedAssetVisible = false;
    bool hasListViewport = false;
    float listViewportHeight = 0.0f;
    float listContentHeight = 0.0f;
    float listScrollOffsetY = 0.0f;
};

struct NativeAssetBrowserAssetSnapshot
{
    Tools::AssetGUID guid;
    std::string path;
    std::string name;
    Tools::AssetType type = Tools::AssetType::Unknown;
    bool isDirty = false;
};

class NativeAssetBrowserPanel final : public IEditorUIPanel
{
public:
    NativeAssetBrowserPanel();

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void BuildUI(EditorUIPanelFrameContext& context) override;

    bool SelectAssetGuid(const Tools::AssetGUID& guid);
    const NativeAssetBrowserStats& GetLastBuildStats() const { return m_lastBuildStats; }

private:
    void AddToolbar(EditorUIPanelFrameContext& context);
    void AddAssetRows(EditorUIPanelFrameContext& context);
    void EnsureAssetSnapshots(EditorUIPanelFrameContext& context);
    void OpenAssetContextMenu(EditorUIHost& host,
                              const Tools::AssetGUID& guid,
                              const Vec2& anchor);

    void RefreshAssetSnapshot(EditorUIPanelFrameContext& context);
    void RefreshVisibleAssetSnapshot(EditorUIPanelFrameContext& context);

    EditorUIPanelDesc m_desc;
    NativeAssetBrowserStats m_lastBuildStats;
    EditorPanelContentCache m_assetSnapshotCache;
    EditorPanelContentCache m_visibleSnapshotCache;
    std::vector<NativeAssetBrowserAssetSnapshot> m_assetSnapshots;
    std::vector<size_t> m_visibleAssetIndices;
    std::string m_searchFilter;
    float m_scrollOffsetY = 0.0f;
};

} // namespace RVX::Editor
