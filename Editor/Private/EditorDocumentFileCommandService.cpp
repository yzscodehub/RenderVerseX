/**
 * @file EditorDocumentFileCommandService.cpp
 * @brief Document file command completion service implementation.
 */

#include "Editor/EditorDocumentFileCommandService.h"
#include "Core/Log.h"
#include "Editor/EditorDocument.h"
#include "Editor/EditorDocumentActionService.h"
#include "Editor/EditorNotificationService.h"
#include "Editor/EditorOperationService.h"
#include "Editor/EditorSettings.h"

#include <utility>

namespace RVX::Editor
{

std::filesystem::path
EditorDocumentFileCommandService::BuildDefaultSceneSavePath() const
{
    if (!m_documentSession)
    {
        return "Untitled.rvxscene";
    }

    std::filesystem::path defaultPath = m_documentSession->GetScenePath();
    if (defaultPath.empty())
    {
        const std::filesystem::path preferredDirectory =
            GetPreferredSceneDialogDirectory();
        defaultPath = preferredDirectory.empty()
                          ? std::filesystem::path("Untitled.rvxscene")
                          : preferredDirectory / "Untitled.rvxscene";
    }
    return defaultPath;
}

std::filesystem::path
EditorDocumentFileCommandService::GetPreferredSceneDialogDirectory() const
{
    if (!m_settings)
    {
        return {};
    }

    const std::vector<std::filesystem::path>& directories =
        m_settings->GetRecentSceneDirectories();
    return directories.empty() ? std::filesystem::path{} : directories.front();
}

void EditorDocumentFileCommandService::PopulateSceneFilePickerRecentDirectories(
    EditorFilePickerDialogDesc& dialog) const
{
    if (!m_settings)
    {
        return;
    }

    dialog.picker.maxRecentDirectories =
        m_settings->GetMaxRecentSceneDirectories();
    dialog.picker.recentDirectories = m_settings->GetRecentSceneDirectories();
    if (dialog.picker.initialDirectory.empty() &&
        !dialog.picker.recentDirectories.empty())
    {
        dialog.picker.initialDirectory = dialog.picker.recentDirectories.front();
    }
}

EditorCommandActionResult
EditorDocumentFileCommandService::RunFallbackOpenSceneFileDialog(
    bool publishResult)
{
    if (!m_documentSession || !m_fileDialogs)
    {
        RVX_CORE_WARN("Open Scene dialog failed: no file dialog service");
        RefreshIfNeeded();
        return PublishResult(MakeUnavailable(
                                 "Open Scene",
                                 "Dialog failed: no file dialog service"),
                             publishResult);
    }

    return CompleteOpenSceneFileDialogResult(m_fileDialogs->OpenSceneFile(),
                                             publishResult);
}

EditorCommandActionResult
EditorDocumentFileCommandService::RunFallbackSaveSceneAsDialog(
    bool publishResult)
{
    if (!m_documentSession || !m_fileDialogs)
    {
        RVX_CORE_WARN("Save Scene As dialog failed: no file dialog service");
        RefreshIfNeeded();
        return PublishResult(MakeUnavailable(
                                 "Save Scene As",
                                 "Dialog failed: no file dialog service"),
                             publishResult);
    }

    return CompleteSaveSceneAsFileDialogResult(
        m_fileDialogs->SaveSceneFileAs(BuildDefaultSceneSavePath()),
        publishResult);
}

EditorCommandActionResult
EditorDocumentFileCommandService::CompleteOpenSceneFilePickerResult(
    const EditorFilePickerDialogResult& dialogResult,
    bool publishResult)
{
    EditorFileDialogResult fileDialogResult;
    fileDialogResult.accepted = dialogResult.accepted;
    fileDialogResult.path = dialogResult.path;
    fileDialogResult.error = dialogResult.error;
    return CompleteOpenSceneFileDialogResult(fileDialogResult, publishResult);
}

EditorCommandActionResult
EditorDocumentFileCommandService::CompleteSaveSceneAsFilePickerResult(
    const EditorFilePickerDialogResult& dialogResult,
    std::function<void(bool)> onSaved,
    EditorDocumentFileOverwriteRequest requestOverwriteConfirmation,
    bool publishResult)
{
    if (!dialogResult.accepted)
    {
        InvokeSaveCompletion(false, std::move(onSaved));
        EditorFileDialogResult fileDialogResult;
        fileDialogResult.accepted = false;
        fileDialogResult.path = dialogResult.path;
        fileDialogResult.error = dialogResult.error;
        return CompleteSaveSceneAsFileDialogResult(fileDialogResult,
                                                   publishResult);
    }

    return CompleteSaveSceneAsAcceptedPath(dialogResult.path,
                                           std::move(onSaved),
                                           std::move(requestOverwriteConfirmation),
                                           publishResult);
}

EditorCommandActionResult
EditorDocumentFileCommandService::CompleteSaveSceneAsAcceptedPath(
    std::filesystem::path path,
    std::function<void(bool)> onSaved,
    EditorDocumentFileOverwriteRequest requestOverwriteConfirmation,
    bool publishResult)
{
    if (m_documentActions &&
        m_documentActions->ShouldConfirmSaveOverwrite(path))
    {
        if (requestOverwriteConfirmation)
        {
            requestOverwriteConfirmation(std::move(path), std::move(onSaved));
            RefreshIfNeeded();
            return PublishResult(MakeWaitingForUser(
                                     "Save Scene As",
                                     "Confirm overwrite before saving"),
                                 publishResult);
        }

        InvokeSaveCompletion(false, std::move(onSaved));
        return PublishResult(MakeRejected(
                                 "Save Scene As",
                                 "Save Scene As failed: overwrite confirmation is unavailable"),
                             publishResult);
    }

    return SaveSceneAsConfirmedPath(std::move(path),
                                    std::move(onSaved),
                                    publishResult);
}

EditorCommandActionResult
EditorDocumentFileCommandService::SaveSceneAsConfirmedPath(
    std::filesystem::path path,
    std::function<void(bool)> onSaved,
    bool publishResult)
{
    if (!m_operations)
    {
        InvokeSaveCompletion(false, std::move(onSaved));
        RefreshIfNeeded();
        return PublishResult(MakeUnavailable(
                                 "Save Scene As",
                                 "Save Scene As failed: operation service is unavailable"),
                             publishResult);
    }

    bool saved = false;
    const EditorOperationResult saveResult = m_operations->SaveSceneAs(path);
    if (!saveResult)
    {
        RVX_CORE_WARN("{}", saveResult.message);
    }
    else
    {
        saved = true;
        RecordRecentScenePath(path);
    }

    InvokeSaveCompletion(saved, std::move(onSaved));
    RefreshIfNeeded();
    return PublishResult(BuildOperationResult("Save Scene As", saveResult),
                         publishResult);
}

EditorCommandActionResult
EditorDocumentFileCommandService::CompleteOpenSceneFileDialogResult(
    const EditorFileDialogResult& dialogResult,
    bool publishResult)
{
    if (!dialogResult.accepted)
    {
        if (!dialogResult.error.empty())
        {
            RVX_CORE_WARN("Open Scene dialog failed: {}", dialogResult.error);
            RefreshIfNeeded();
            return PublishResult(MakeRejected("Open Scene", dialogResult.error),
                                 publishResult);
        }

        RefreshIfNeeded();
        return PublishResult(MakeCancelled("Open Scene", "Cancelled"),
                             publishResult);
    }

    if (!m_operations)
    {
        RefreshIfNeeded();
        return PublishResult(MakeUnavailable(
                                 "Open Scene",
                                 "Open Scene failed: operation service is unavailable"),
                             publishResult);
    }

    const EditorOperationResult openResult =
        m_operations->OpenScene(dialogResult.path);
    if (openResult)
    {
        RecordRecentScenePath(dialogResult.path);
    }
    else
    {
        RVX_CORE_WARN("{}", openResult.message);
    }
    RefreshIfNeeded();
    return PublishResult(BuildOperationResult("Open Scene", openResult),
                         publishResult);
}

EditorCommandActionResult
EditorDocumentFileCommandService::CompleteSaveSceneAsFileDialogResult(
    const EditorFileDialogResult& dialogResult,
    bool publishResult)
{
    if (!dialogResult.accepted)
    {
        if (!dialogResult.error.empty())
        {
            RVX_CORE_WARN("Save Scene As dialog failed: {}",
                          dialogResult.error);
            RefreshIfNeeded();
            return PublishResult(MakeRejected("Save Scene As",
                                              dialogResult.error),
                                 publishResult);
        }

        RefreshIfNeeded();
        return PublishResult(MakeCancelled("Save Scene As", "Cancelled"),
                             publishResult);
    }

    return SaveSceneAsConfirmedPath(dialogResult.path, {}, publishResult);
}

EditorCommandActionResult EditorDocumentFileCommandService::PublishResult(
    EditorCommandActionResult result,
    bool publishResult)
{
    m_lastResult = std::move(result);
    if (publishResult && m_commandService)
    {
        EditorCommandActionOptions options;
        options.refreshDocumentCommands = false;
        m_lastResult = m_commandService->ExecutePlan(
            {m_lastResult.title,
             [result = m_lastResult]() {
                 return result;
             },
             options});
    }
    return m_lastResult;
}

EditorCommandActionResult EditorDocumentFileCommandService::MakeUnavailable(
    std::string title,
    std::string message) const
{
    return m_commandService ? m_commandService->MakeUnavailable(
                                  std::move(title),
                                  std::move(message))
                            : EditorCommandActionResult{
                                  EditorCommandActionStatus::Unavailable,
                                  std::move(title),
                                  std::move(message),
                                  EditorOperationStatus::Unavailable};
}

EditorCommandActionResult EditorDocumentFileCommandService::MakeRejected(
    std::string title,
    std::string message) const
{
    return m_commandService ? m_commandService->MakeRejected(std::move(title),
                                                             std::move(message))
                            : EditorCommandActionResult{
                                  EditorCommandActionStatus::Rejected,
                                  std::move(title),
                                  std::move(message),
                                  EditorOperationStatus::Rejected};
}

EditorCommandActionResult EditorDocumentFileCommandService::MakeCancelled(
    std::string title,
    std::string message) const
{
    return m_commandService ? m_commandService->MakeCancelled(
                                  std::move(title),
                                  std::move(message))
                            : EditorCommandActionResult{
                                  EditorCommandActionStatus::Cancelled,
                                  std::move(title),
                                  std::move(message),
                                  EditorOperationStatus::Unavailable};
}

EditorCommandActionResult EditorDocumentFileCommandService::MakeWaitingForUser(
    std::string title,
    std::string message) const
{
    return m_commandService ? m_commandService->MakeWaitingForUser(
                                  std::move(title),
                                  std::move(message))
                            : EditorCommandActionResult{
                                  EditorCommandActionStatus::WaitingForUser,
                                  std::move(title),
                                  std::move(message),
                                  EditorOperationStatus::Unavailable};
}

EditorCommandActionResult
EditorDocumentFileCommandService::BuildOperationResult(
    std::string title,
    const EditorOperationResult& operationResult) const
{
    return m_commandService
               ? m_commandService->BuildOperationResult(std::move(title),
                                                        operationResult)
               : EditorCommandActionResult{
                     operationResult ? EditorCommandActionStatus::Succeeded
                                     : EditorCommandActionStatus::Rejected,
                     std::move(title),
                     operationResult.message,
                     operationResult.status};
}

void EditorDocumentFileCommandService::NotifyWarning(
    const std::string& title,
    const std::string& message) const
{
    if (m_notifications)
    {
        m_notifications->PublishWarning(title, message);
    }
}

void EditorDocumentFileCommandService::InvokeSaveCompletion(
    bool saved,
    std::function<void(bool)> onSaved) const
{
    if (onSaved)
    {
        onSaved(saved);
    }
}

void EditorDocumentFileCommandService::RefreshIfNeeded() const
{
    if (m_refreshCallback)
    {
        m_refreshCallback();
    }
}

void EditorDocumentFileCommandService::RecordRecentScenePath(
    const std::filesystem::path& path)
{
    if (!m_settings)
    {
        return;
    }

    if (!m_settings->AddRecentScenePath(path))
    {
        if (!m_settings->GetLastError().empty())
        {
            NotifyWarning("Recent Scenes", m_settings->GetLastError());
            RVX_CORE_WARN("Editor recent scene directory update failed: {}",
                          m_settings->GetLastError());
        }
        return;
    }

    if (!m_settings->Save())
    {
        NotifyWarning("Editor Settings", m_settings->GetLastError());
        RVX_CORE_WARN("Editor settings save failed: {}",
                      m_settings->GetLastError());
    }
}

} // namespace RVX::Editor
