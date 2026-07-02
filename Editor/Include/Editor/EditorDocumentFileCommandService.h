/**
 * @file EditorDocumentFileCommandService.h
 * @brief Document file command completion service.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorCommandExecutionService.h"
#include "Editor/EditorFileDialog.h"
#include "Editor/UI/EditorFilePickerDialog.h"

#include <filesystem>
#include <functional>
#include <string>

namespace RVX::Editor
{

class EditorDocumentActionService;
class EditorDocumentSession;
class EditorNotificationService;
class EditorOperationService;
class EditorSettingsService;
struct EditorOperationResult;

using EditorDocumentFileRefreshCallback = std::function<void()>;
using EditorDocumentFileOverwriteRequest =
    std::function<void(std::filesystem::path, std::function<void(bool)>)>;

/**
 * @brief Completes scene Open/Save file command results outside the app shell.
 */
class EditorDocumentFileCommandService
{
public:
    // =========================================================================
    // Dependencies
    // =========================================================================

    void SetCommandExecutionService(EditorCommandExecutionService* commandService)
    {
        m_commandService = commandService;
    }
    void SetDocumentActionService(EditorDocumentActionService* documentActions)
    {
        m_documentActions = documentActions;
    }
    void SetDocumentSession(EditorDocumentSession* documentSession)
    {
        m_documentSession = documentSession;
    }
    void SetFileDialogService(EditorFileDialogService* fileDialogs)
    {
        m_fileDialogs = fileDialogs;
    }
    void SetNotificationService(EditorNotificationService* notifications)
    {
        m_notifications = notifications;
    }
    void SetOperationService(EditorOperationService* operations)
    {
        m_operations = operations;
    }
    void SetSettingsService(EditorSettingsService* settings)
    {
        m_settings = settings;
    }
    void SetRefreshCallback(EditorDocumentFileRefreshCallback refreshCallback)
    {
        m_refreshCallback = std::move(refreshCallback);
    }

    // =========================================================================
    // Dialog Data
    // =========================================================================

    std::filesystem::path BuildDefaultSceneSavePath() const;
    std::filesystem::path GetPreferredSceneDialogDirectory() const;
    void PopulateSceneFilePickerRecentDirectories(
        EditorFilePickerDialogDesc& dialog) const;

    // =========================================================================
    // Command Completion
    // =========================================================================

    EditorCommandActionResult RunFallbackOpenSceneFileDialog(
        bool publishResult = false);
    EditorCommandActionResult RunFallbackSaveSceneAsDialog(
        bool publishResult = false);
    EditorCommandActionResult CompleteOpenSceneFilePickerResult(
        const EditorFilePickerDialogResult& dialogResult,
        bool publishResult = true);
    EditorCommandActionResult CompleteSaveSceneAsFilePickerResult(
        const EditorFilePickerDialogResult& dialogResult,
        std::function<void(bool)> onSaved = {},
        EditorDocumentFileOverwriteRequest requestOverwriteConfirmation = {},
        bool publishResult = true);
    EditorCommandActionResult CompleteSaveSceneAsAcceptedPath(
        std::filesystem::path path,
        std::function<void(bool)> onSaved = {},
        EditorDocumentFileOverwriteRequest requestOverwriteConfirmation = {},
        bool publishResult = true);
    EditorCommandActionResult SaveSceneAsConfirmedPath(
        std::filesystem::path path,
        std::function<void(bool)> onSaved = {},
        bool publishResult = true);

    const EditorCommandActionResult& GetLastResult() const
    {
        return m_lastResult;
    }

private:
    EditorCommandActionResult CompleteOpenSceneFileDialogResult(
        const EditorFileDialogResult& dialogResult,
        bool publishResult);
    EditorCommandActionResult CompleteSaveSceneAsFileDialogResult(
        const EditorFileDialogResult& dialogResult,
        bool publishResult);
    EditorCommandActionResult PublishResult(EditorCommandActionResult result,
                                            bool publishResult);
    EditorCommandActionResult MakeUnavailable(std::string title,
                                              std::string message) const;
    EditorCommandActionResult MakeRejected(std::string title,
                                           std::string message) const;
    EditorCommandActionResult MakeCancelled(std::string title,
                                            std::string message) const;
    EditorCommandActionResult MakeWaitingForUser(std::string title,
                                                 std::string message) const;
    EditorCommandActionResult BuildOperationResult(
        std::string title,
        const EditorOperationResult& operationResult) const;
    void NotifyWarning(const std::string& title, const std::string& message) const;
    void InvokeSaveCompletion(bool saved, std::function<void(bool)> onSaved) const;
    void RefreshIfNeeded() const;
    void RecordRecentScenePath(const std::filesystem::path& path);

    EditorCommandExecutionService* m_commandService = nullptr;
    EditorDocumentActionService* m_documentActions = nullptr;
    EditorDocumentSession* m_documentSession = nullptr;
    EditorFileDialogService* m_fileDialogs = nullptr;
    EditorNotificationService* m_notifications = nullptr;
    EditorOperationService* m_operations = nullptr;
    EditorSettingsService* m_settings = nullptr;
    EditorDocumentFileRefreshCallback m_refreshCallback;
    EditorCommandActionResult m_lastResult;
};

} // namespace RVX::Editor
