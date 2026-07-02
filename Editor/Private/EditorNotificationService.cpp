/**
 * @file EditorNotificationService.cpp
 * @brief Editor notification and status reporting service implementation.
 */

#include "Editor/EditorNotificationService.h"
#include "Editor/Panels/Console.h"
#include "Editor/UI/EditorCommandSurfaceModel.h"

#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr size_t RVX_EDITOR_NOTIFICATION_HISTORY_CAPACITY = 128;

    const char* SeverityText(EditorNotificationSeverity severity)
    {
        switch (severity)
        {
            case EditorNotificationSeverity::Info:
                return "Info";
            case EditorNotificationSeverity::Success:
                return "OK";
            case EditorNotificationSeverity::Warning:
                return "Warning";
            case EditorNotificationSeverity::Error:
                return "Error";
        }
        return "Info";
    }
}

void EditorNotificationService::SetCommandSurfaceModel(
    EditorCommandSurfaceModel* surfaceModel)
{
    m_surfaceModel = surfaceModel;
    UpdateStatusSurface();
}

const EditorNotificationEntry& EditorNotificationService::Publish(
    EditorNotificationSeverity severity,
    std::string title,
    std::string message)
{
    EditorNotificationEntry entry;
    entry.sequence = m_nextSequence++;
    entry.severity = severity;
    entry.title = std::move(title);
    entry.message = std::move(message);
    entry.statusText = FormatStatusText(entry.severity,
                                        entry.title,
                                        entry.message);

    if (m_history.size() >= RVX_EDITOR_NOTIFICATION_HISTORY_CAPACITY)
    {
        m_history.erase(m_history.begin());
    }
    m_history.push_back(std::move(entry));
    ++m_revision;

    UpdateStatusSurface();
    MirrorToConsole(m_history.back());
    return m_history.back();
}

const EditorNotificationEntry& EditorNotificationService::PublishInfo(
    std::string title,
    std::string message)
{
    return Publish(EditorNotificationSeverity::Info,
                   std::move(title),
                   std::move(message));
}

const EditorNotificationEntry& EditorNotificationService::PublishSuccess(
    std::string title,
    std::string message)
{
    return Publish(EditorNotificationSeverity::Success,
                   std::move(title),
                   std::move(message));
}

const EditorNotificationEntry& EditorNotificationService::PublishWarning(
    std::string title,
    std::string message)
{
    return Publish(EditorNotificationSeverity::Warning,
                   std::move(title),
                   std::move(message));
}

const EditorNotificationEntry& EditorNotificationService::PublishError(
    std::string title,
    std::string message)
{
    return Publish(EditorNotificationSeverity::Error,
                   std::move(title),
                   std::move(message));
}

void EditorNotificationService::Clear()
{
    m_history.clear();
    ++m_revision;
    UpdateStatusSurface();
}

const EditorNotificationEntry* EditorNotificationService::GetLatest() const
{
    return m_history.empty() ? nullptr : &m_history.back();
}

std::string EditorNotificationService::FormatStatusText(
    EditorNotificationSeverity severity,
    const std::string& title,
    const std::string& message) const
{
    std::string text = SeverityText(severity);
    if (!title.empty())
    {
        text += " - " + title;
    }
    if (!message.empty())
    {
        text += title.empty() ? " - " : ": ";
        text += message;
    }
    return text;
}

void EditorNotificationService::UpdateStatusSurface()
{
    if (!m_surfaceModel)
    {
        return;
    }

    EditorStatusSurfaceItem item;
    item.id = RVX_EDITOR_NOTIFICATION_STATUS_ITEM_ID;
    item.label = "Status";
    item.value = m_history.empty() ? "Ready" : m_history.back().statusText;
    item.priority = 1;
    item.visible = true;
    m_surfaceModel->SetStatusItem(std::move(item));
}

void EditorNotificationService::MirrorToConsole(
    const EditorNotificationEntry& entry) const
{
    if (!m_mirrorToConsole)
    {
        return;
    }

    switch (entry.severity)
    {
        case EditorNotificationSeverity::Info:
        case EditorNotificationSeverity::Success:
            ConsolePanel::LogInfo(entry.statusText);
            break;
        case EditorNotificationSeverity::Warning:
            ConsolePanel::LogWarning(entry.statusText);
            break;
        case EditorNotificationSeverity::Error:
            ConsolePanel::LogError(entry.statusText);
            break;
    }
}

} // namespace RVX::Editor
