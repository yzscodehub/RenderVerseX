/**
 * @file EditorDocumentActionService.h
 * @brief Document action guard and modal decision service.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorUnsavedChanges.h"

#include <filesystem>
#include <functional>
#include <string>

namespace RVX::Editor
{

class EditorDocumentSession;
class EditorPendingActionQueue;
struct EditorPendingActionState;

enum class EditorDocumentActionStatus : uint8
{
    Completed = 0,
    Deferred,
    RequiresSaveAs,
    Cancelled,
    Unavailable,
    Rejected
};

struct EditorDocumentActionResult
{
    EditorDocumentActionStatus status = EditorDocumentActionStatus::Unavailable;
    std::string message;
    bool closeModal = false;
    bool openUnsavedChangesModal = false;
    bool openSaveSceneAsPicker = false;

    bool Completed() const
    {
        return status == EditorDocumentActionStatus::Completed;
    }
    explicit operator bool() const { return Completed(); }
};

struct EditorSaveOverwriteConfirmationDesc
{
    std::filesystem::path path;
    std::string title;
    std::string message;
    std::string confirmButtonText;
    std::string cancelButtonText;
};

/**
 * @brief Coordinates guarded document actions without owning UI widgets.
 */
class EditorDocumentActionService
{
public:
    // =========================================================================
    // Dependencies
    // =========================================================================

    void SetDocumentSession(EditorDocumentSession* documentSession)
    {
        m_documentSession = documentSession;
    }
    void SetUnsavedChangesGuard(EditorUnsavedChangesGuard* unsavedChangesGuard)
    {
        m_unsavedChangesGuard = unsavedChangesGuard;
    }
    void SetPendingActions(EditorPendingActionQueue* pendingActions)
    {
        m_pendingActions = pendingActions;
    }

    // =========================================================================
    // Query
    // =========================================================================

    bool HasPendingAction() const;
    const EditorPendingActionState* GetPendingActionState() const;
    const EditorDocumentActionResult& GetLastResult() const
    {
        return m_lastResult;
    }

    // =========================================================================
    // Guarded Actions
    // =========================================================================

    EditorDocumentActionResult RequestGuardedAction(
        std::string actionId,
        std::string label,
        std::function<void()> continuation,
        bool interactiveModalAvailable);
    EditorDocumentActionResult ResolveUnsavedChanges(
        EditorUnsavedChangesChoice choice,
        bool saveAsPickerAvailable);
    EditorDocumentActionResult CompletePendingActionAfterSave(bool saved);
    EditorUnsavedChangesPromptDesc BuildUnsavedChangesPromptDesc() const;

    // =========================================================================
    // Save As Confirmation
    // =========================================================================

    bool ShouldConfirmSaveOverwrite(const std::filesystem::path& path) const;
    EditorSaveOverwriteConfirmationDesc BuildSaveOverwriteConfirmationDesc(
        const std::filesystem::path& path) const;

private:
    EditorDocumentActionResult SetResult(EditorDocumentActionStatus status,
                                         std::string message = {},
                                         bool closeModal = false,
                                         bool openUnsavedChangesModal = false,
                                         bool openSaveSceneAsPicker = false);

    EditorDocumentSession* m_documentSession = nullptr;
    EditorUnsavedChangesGuard* m_unsavedChangesGuard = nullptr;
    EditorPendingActionQueue* m_pendingActions = nullptr;
    EditorDocumentActionResult m_lastResult;
};

} // namespace RVX::Editor
