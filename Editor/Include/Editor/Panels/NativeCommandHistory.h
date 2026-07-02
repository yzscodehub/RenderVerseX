/**
 * @file NativeCommandHistory.h
 * @brief Native UI command history diagnostics panel
 */

#pragma once

#include "Editor/UI/EditorUIPanel.h"

namespace RVX::Editor
{

struct NativeCommandHistoryStats
{
    bool built = false;
    bool hasDiagnostics = false;
    uint32 totalEntryCount = 0;
    uint32 visibleEntryCount = 0;
    uint32 succeededEntryCount = 0;
    uint32 failedEntryCount = 0;
    uint32 directEntryCount = 0;
    uint32 shortcutEntryCount = 0;
    uint32 automationEntryCount = 0;
    uint64 lastSequence = 0;
    uint64 historyDroppedCount = 0;
    bool hasListViewport = false;
    float listViewportHeight = 0.0f;
    float listContentHeight = 0.0f;
    float listScrollOffsetY = 0.0f;
};

class NativeCommandHistoryPanel final : public IEditorUIPanel
{
public:
    NativeCommandHistoryPanel();

    static const char* PanelId() { return "native.commandHistory"; }

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void BuildUI(EditorUIPanelFrameContext& context) override;

    const NativeCommandHistoryStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    void AddToolbar(EditorUIPanelFrameContext& context);
    void AddHistoryRows(EditorUIPanelFrameContext& context);

    EditorUIPanelDesc m_desc;
    NativeCommandHistoryStats m_lastBuildStats;
    float m_scrollOffsetY = 0.0f;
};

} // namespace RVX::Editor
