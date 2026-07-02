/**
 * @file NativePanelCatalog.h
 * @brief Native UI panel catalog and visibility diagnostics panel
 */

#pragma once

#include "Editor/UI/EditorUIPanel.h"

#include <string>

namespace RVX::Editor
{

struct NativePanelCatalogStats
{
    bool built = false;
    bool hasHost = false;
    uint32 totalPanelCount = 0;
    uint32 visiblePanelCount = 0;
    uint32 hiddenPanelCount = 0;
    uint32 defaultVisiblePanelCount = 0;
    uint32 pendingRebuildPanelCount = 0;
    uint32 rowCount = 0;
    uint32 filteredPanelCount = 0;
    uint32 searchInputCount = 0;
    uint32 toggleButtonCount = 0;
    uint32 toggleIconButtonCount = 0;
    uint32 rowMetadataLabelCount = 0;
    uint32 layoutCommandButtonCount = 0;
    uint32 layoutCommandIconButtonCount = 0;
    uint32 layoutCommandExecutableCount = 0;
    bool filterActive = false;
    bool hasListViewport = false;
    float rowHeight = 0.0f;
    float listViewportHeight = 0.0f;
    float listContentHeight = 0.0f;
    float listScrollOffsetY = 0.0f;
    std::string filterText;
};

class NativePanelCatalogPanel final : public IEditorUIPanel
{
public:
    NativePanelCatalogPanel();

    static constexpr const char* PanelId() { return "native.panelCatalog"; }

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void BuildUI(EditorUIPanelFrameContext& context) override;

    const NativePanelCatalogStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    void AddToolbar(EditorUIPanelFrameContext& context);
    void AddPanelRows(EditorUIPanelFrameContext& context);

    EditorUIPanelDesc m_desc;
    NativePanelCatalogStats m_lastBuildStats;
    std::string m_searchFilter;
    float m_scrollOffsetY = 0.0f;
};

} // namespace RVX::Editor
