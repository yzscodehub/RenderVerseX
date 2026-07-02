/**
 * @file EditorDocumentActionService.cpp
 * @brief Document action guard and modal decision service implementation.
 */

#include "Editor/EditorDocumentActionService.h"
#include "Editor/EditorDocument.h"
#include "Editor/EditorPendingAction.h"

#include <filesystem>
#include <system_error>
#include <utility>

namespace RVX::Editor
{

bool EditorDocumentActionService::HasPendingAction() const
{
    return m_pendingActions && m_pendingActions->HasPendingAction();
}

const EditorPendingActionState*
EditorDocumentActionService::GetPendingActionState() const
{
    return HasPendingAction() ? &m_pendingActions->GetState() : nullptr;
}

EditorDocumentActionResult EditorDocumentActionService::RequestGuardedAction(
    std::string actionId,
    std::string label,
    std::function<void()> continuation,
    bool interactiveModalAvailable)
{
    if (!continuation)
    {
        return SetResult(EditorDocumentActionStatus::Rejected,
                         label + " cancelled: no continuation was provided");
    }

    if (!m_documentSession || !m_documentSession->IsDirty())
    {
        continuation();
        return SetResult(EditorDocumentActionStatus::Completed, label);
    }

    if (!m_unsavedChangesGuard)
    {
        return SetResult(
            EditorDocumentActionStatus::Unavailable,
            label + " cancelled: cannot resolve unsaved changes without a guard");
    }

    if (!interactiveModalAvailable || !m_pendingActions)
    {
        if (m_unsavedChangesGuard->ConfirmSaveDiscardOrCancel())
        {
            continuation();
            return SetResult(EditorDocumentActionStatus::Completed, label);
        }

        if (!m_unsavedChangesGuard->GetLastError().empty())
        {
            return SetResult(EditorDocumentActionStatus::Rejected,
                             label + " cancelled: " +
                                 m_unsavedChangesGuard->GetLastError());
        }

        return SetResult(EditorDocumentActionStatus::Cancelled,
                         label + " cancelled");
    }

    if (m_pendingActions->HasPendingAction())
    {
        return SetResult(
            EditorDocumentActionStatus::Rejected,
            label + " ignored: pending editor action '" +
                m_pendingActions->GetState().id + "' is waiting for a decision");
    }

    EditorPendingActionDesc pendingAction;
    pendingAction.id = std::move(actionId);
    pendingAction.label = std::move(label);
    pendingAction.callback = std::move(continuation);
    if (!m_pendingActions->Begin(std::move(pendingAction)))
    {
        return SetResult(EditorDocumentActionStatus::Rejected,
                         "Unable to defer editor action: " +
                             m_pendingActions->GetLastError());
    }

    return SetResult(EditorDocumentActionStatus::Deferred,
                     {},
                     false,
                     true,
                     false);
}

EditorDocumentActionResult EditorDocumentActionService::ResolveUnsavedChanges(
    EditorUnsavedChangesChoice choice,
    bool saveAsPickerAvailable)
{
    if (!HasPendingAction())
    {
        return SetResult(EditorDocumentActionStatus::Unavailable,
                         "No pending editor action is waiting for unsaved changes",
                         true);
    }

    if (choice == EditorUnsavedChangesChoice::Save)
    {
        if (m_documentSession && !m_documentSession->CanSaveScene() &&
            saveAsPickerAvailable)
        {
            return SetResult(EditorDocumentActionStatus::RequiresSaveAs,
                             {},
                             true,
                             false,
                             true);
        }

        if (!m_unsavedChangesGuard)
        {
            return SetResult(
                EditorDocumentActionStatus::Unavailable,
                "Save before continuing failed: unsaved changes guard is unavailable");
        }
        if (!m_unsavedChangesGuard->SaveActiveDocument())
        {
            const std::string error = m_unsavedChangesGuard->GetLastError();
            return SetResult(
                EditorDocumentActionStatus::Rejected,
                error.empty() ? "Save before continuing failed"
                              : "Save before continuing failed: " + error);
        }
    }
    else if (choice == EditorUnsavedChangesChoice::Cancel)
    {
        if (!m_pendingActions->Cancel())
        {
            return SetResult(EditorDocumentActionStatus::Rejected,
                             "Unable to cancel pending editor action: " +
                                 m_pendingActions->GetLastError(),
                             true);
        }

        return SetResult(EditorDocumentActionStatus::Cancelled,
                         {},
                         true);
    }

    if (!m_pendingActions->Complete())
    {
        return SetResult(EditorDocumentActionStatus::Rejected,
                         "Unable to complete pending editor action: " +
                             m_pendingActions->GetLastError(),
                         true);
    }

    return SetResult(EditorDocumentActionStatus::Completed, {}, true);
}

EditorDocumentActionResult
EditorDocumentActionService::CompletePendingActionAfterSave(bool saved)
{
    if (!HasPendingAction())
    {
        return SetResult(EditorDocumentActionStatus::Unavailable,
                         "No pending editor action is waiting for Save Scene As");
    }

    if (!saved)
    {
        return SetResult(EditorDocumentActionStatus::Deferred,
                         {},
                         false,
                         true,
                         false);
    }

    if (!m_pendingActions->Complete())
    {
        return SetResult(EditorDocumentActionStatus::Rejected,
                         "Unable to complete pending editor action: " +
                             m_pendingActions->GetLastError());
    }

    return SetResult(EditorDocumentActionStatus::Completed);
}

EditorUnsavedChangesPromptDesc
EditorDocumentActionService::BuildUnsavedChangesPromptDesc() const
{
    if (m_unsavedChangesGuard)
    {
        return m_unsavedChangesGuard->BuildPromptDesc();
    }

    EditorUnsavedChangesPromptDesc desc;
    desc.title = "Unsaved Scene Changes";
    desc.documentName = "Untitled Scene";
    desc.message = "Save changes before continuing?";
    return desc;
}

bool EditorDocumentActionService::ShouldConfirmSaveOverwrite(
    const std::filesystem::path& path) const
{
    if (path.empty())
    {
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
    {
        return false;
    }

    if (m_documentSession && !m_documentSession->GetScenePath().empty())
    {
        ec.clear();
        if (std::filesystem::equivalent(path,
                                        m_documentSession->GetScenePath(),
                                        ec) &&
            !ec)
        {
            return false;
        }
    }

    return true;
}

EditorSaveOverwriteConfirmationDesc
EditorDocumentActionService::BuildSaveOverwriteConfirmationDesc(
    const std::filesystem::path& path) const
{
    const std::string fileName = path.filename().empty()
                                     ? path.string()
                                     : path.filename().string();

    EditorSaveOverwriteConfirmationDesc desc;
    desc.path = path;
    desc.title = "Overwrite Existing Scene?";
    desc.message = "A file named '" + fileName +
                   "' already exists. Replace it with this scene?";
    desc.confirmButtonText = "Overwrite";
    desc.cancelButtonText = "Cancel";
    return desc;
}

EditorDocumentActionResult EditorDocumentActionService::SetResult(
    EditorDocumentActionStatus status,
    std::string message,
    bool closeModal,
    bool openUnsavedChangesModal,
    bool openSaveSceneAsPicker)
{
    m_lastResult.status = status;
    m_lastResult.message = std::move(message);
    m_lastResult.closeModal = closeModal;
    m_lastResult.openUnsavedChangesModal = openUnsavedChangesModal;
    m_lastResult.openSaveSceneAsPicker = openSaveSceneAsPicker;
    return m_lastResult;
}

} // namespace RVX::Editor
