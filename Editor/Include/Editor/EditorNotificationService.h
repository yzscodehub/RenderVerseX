/**
 * @file EditorNotificationService.h
 * @brief Editor notification and status reporting service.
 */

#pragma once

#include "Core/Types.h"

#include <string>
#include <vector>

namespace RVX::Editor
{

class EditorCommandSurfaceModel;

inline constexpr const char* RVX_EDITOR_NOTIFICATION_STATUS_ITEM_ID =
    "editor.notification";

enum class EditorNotificationSeverity : uint8
{
    Info = 0,
    Success,
    Warning,
    Error
};

struct EditorNotificationEntry
{
    uint64 sequence = 0;
    EditorNotificationSeverity severity = EditorNotificationSeverity::Info;
    std::string title;
    std::string message;
    std::string statusText;
};

/**
 * @brief Publishes user-visible editor status and notification messages.
 */
class EditorNotificationService
{
public:
    // =========================================================================
    // Dependencies
    // =========================================================================

    void SetCommandSurfaceModel(EditorCommandSurfaceModel* surfaceModel);
    void SetMirrorToConsole(bool mirrorToConsole)
    {
        m_mirrorToConsole = mirrorToConsole;
    }

    // =========================================================================
    // Publishing
    // =========================================================================

    const EditorNotificationEntry& Publish(EditorNotificationSeverity severity,
                                           std::string title,
                                           std::string message);
    const EditorNotificationEntry& PublishInfo(std::string title,
                                               std::string message);
    const EditorNotificationEntry& PublishSuccess(std::string title,
                                                  std::string message);
    const EditorNotificationEntry& PublishWarning(std::string title,
                                                  std::string message);
    const EditorNotificationEntry& PublishError(std::string title,
                                                std::string message);

    void Clear();

    // =========================================================================
    // Query
    // =========================================================================

    bool HasNotifications() const { return !m_history.empty(); }
    const EditorNotificationEntry* GetLatest() const;
    const std::vector<EditorNotificationEntry>& GetHistory() const
    {
        return m_history;
    }
    uint64 GetRevision() const { return m_revision; }

private:
    std::string FormatStatusText(EditorNotificationSeverity severity,
                                 const std::string& title,
                                 const std::string& message) const;
    void UpdateStatusSurface();
    void MirrorToConsole(const EditorNotificationEntry& entry) const;

    EditorCommandSurfaceModel* m_surfaceModel = nullptr;
    std::vector<EditorNotificationEntry> m_history;
    uint64 m_nextSequence = 1;
    uint64 m_revision = 0;
    bool m_mirrorToConsole = false;
};

} // namespace RVX::Editor
