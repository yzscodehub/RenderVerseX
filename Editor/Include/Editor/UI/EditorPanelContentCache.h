/**
 * @file EditorPanelContentCache.h
 * @brief Panel-local data snapshot cache helpers
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorUIPanel.h"

#include <string>
#include <string_view>

namespace RVX::Editor
{

struct EditorPanelContentCacheStats
{
    bool valid = false;
    uint32 refreshCount = 0;
    uint64 dataRevision = 0;
    uint64 panelRebuildRevision = 0;
    std::string key;
};

class EditorPanelContentCache
{
public:
    bool NeedsRefresh(const EditorUIPanelFrameContext& context,
                      uint64 dataRevision,
                      std::string_view key = {}) const
    {
        if (!m_valid)
        {
            return true;
        }

        if (m_dataRevision != dataRevision || m_key != key)
        {
            return true;
        }

        if (HasEditorUIPanelRebuildReason(context.rebuildReasonMask,
                                          EditorUIPanelRebuildReason::Initial) ||
            HasEditorUIPanelRebuildReason(context.rebuildReasonMask,
                                          EditorUIPanelRebuildReason::Explicit))
        {
            return true;
        }

        return false;
    }

    void MarkRefreshed(const EditorUIPanelFrameContext& context,
                       uint64 dataRevision,
                       std::string_view key = {})
    {
        m_valid = true;
        m_dataRevision = dataRevision;
        m_panelRebuildRevision = context.rebuildRevision;
        m_key.assign(key.begin(), key.end());
        ++m_refreshCount;
    }

    void Invalidate()
    {
        m_valid = false;
        m_dataRevision = 0;
        m_panelRebuildRevision = 0;
        m_key.clear();
    }

    bool IsValid() const { return m_valid; }
    uint32 GetRefreshCount() const { return m_refreshCount; }
    uint64 GetDataRevision() const { return m_dataRevision; }
    uint64 GetPanelRebuildRevision() const { return m_panelRebuildRevision; }
    const std::string& GetKey() const { return m_key; }

    EditorPanelContentCacheStats GetStats() const
    {
        EditorPanelContentCacheStats stats;
        stats.valid = m_valid;
        stats.refreshCount = m_refreshCount;
        stats.dataRevision = m_dataRevision;
        stats.panelRebuildRevision = m_panelRebuildRevision;
        stats.key = m_key;
        return stats;
    }

private:
    bool m_valid = false;
    uint32 m_refreshCount = 0;
    uint64 m_dataRevision = 0;
    uint64 m_panelRebuildRevision = 0;
    std::string m_key;
};

} // namespace RVX::Editor
