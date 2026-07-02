/**
 * @file EditorDocumentCommandService.cpp
 * @brief Document command action plan adapter service implementation.
 */

#include "Editor/EditorDocumentCommandService.h"

#include <memory>
#include <utility>

namespace RVX::Editor
{
namespace
{
    struct DeferredDocumentCommandContinuation
    {
        std::string title;
        std::function<EditorCommandActionResult()> continuation;
        EditorCommandActionResult result;
        bool ran = false;
        bool publishResultWhenRun = false;
    };
}

EditorCommandActionResult EditorDocumentCommandService::RequestGuardedAction(
    std::string actionId,
    std::string label,
    std::function<EditorCommandActionResult()> continuation,
    bool interactiveModalAvailable,
    EditorDocumentCommandUIRequest openUnsavedChangesModal)
{
    const std::string title = label;
    if (!m_documentActions)
    {
        return SetLastResult(MakeResult(
            EditorCommandActionStatus::Unavailable,
            title,
            title + " failed: document action service is unavailable",
            EditorOperationStatus::Unavailable));
    }

    auto continuationState =
        std::make_shared<DeferredDocumentCommandContinuation>();
    continuationState->title = title;
    continuationState->continuation = std::move(continuation);

    std::function<void()> guardedContinuation;
    if (continuationState->continuation)
    {
        guardedContinuation = [this, continuationState]() mutable {
            continuationState->result = continuationState->continuation();
            continuationState->ran = true;
            SetLastResult(continuationState->result);
            if (continuationState->publishResultWhenRun && m_commandService)
            {
                EditorCommandActionOptions options;
                options.refreshDocumentCommands = false;
                m_commandService->ExecutePlan(
                    {continuationState->title,
                     [continuationState]() {
                         return continuationState->result;
                     },
                     options});
            }
        };
    }

    const EditorDocumentActionResult documentResult =
        m_documentActions->RequestGuardedAction(std::move(actionId),
                                                std::move(label),
                                                std::move(guardedContinuation),
                                                interactiveModalAvailable);

    if (documentResult.status == EditorDocumentActionStatus::Deferred)
    {
        continuationState->publishResultWhenRun = true;
    }
    if (documentResult.openUnsavedChangesModal && openUnsavedChangesModal)
    {
        openUnsavedChangesModal();
    }

    if (documentResult.Completed() && continuationState->ran)
    {
        return SetLastResult(continuationState->result);
    }
    return SetLastResult(BuildCommandResult(title, documentResult));
}

EditorCommandActionResult EditorDocumentCommandService::BuildCommandResult(
    const std::string& title,
    const EditorDocumentActionResult& result) const
{
    switch (result.status)
    {
        case EditorDocumentActionStatus::Completed:
            return m_commandService
                       ? m_commandService->MakeSucceeded(title, result.message)
                       : MakeResult(EditorCommandActionStatus::Succeeded,
                                    title,
                                    result.message.empty()
                                        ? title + " completed"
                                        : result.message,
                                    EditorOperationStatus::Succeeded);
        case EditorDocumentActionStatus::Deferred:
            return m_commandService
                       ? m_commandService->MakeWaitingForUser(
                             title,
                             result.message.empty()
                                 ? title +
                                       " waiting for unsaved changes decision"
                                 : result.message)
                       : MakeResult(EditorCommandActionStatus::WaitingForUser,
                                    title,
                                    result.message.empty()
                                        ? title +
                                              " waiting for unsaved changes decision"
                                        : result.message,
                                    EditorOperationStatus::Unavailable);
        case EditorDocumentActionStatus::RequiresSaveAs:
            return m_commandService
                       ? m_commandService->MakeWaitingForUser(
                             title,
                             result.message.empty()
                                 ? "Save Scene As required before continuing"
                                 : result.message)
                       : MakeResult(EditorCommandActionStatus::WaitingForUser,
                                    title,
                                    result.message.empty()
                                        ? "Save Scene As required before continuing"
                                        : result.message,
                                    EditorOperationStatus::Unavailable);
        case EditorDocumentActionStatus::Cancelled:
            return m_commandService
                       ? m_commandService->MakeCancelled(title, result.message)
                       : MakeResult(EditorCommandActionStatus::Cancelled,
                                    title,
                                    result.message.empty() ? title + " cancelled"
                                                           : result.message,
                                    EditorOperationStatus::Unavailable);
        case EditorDocumentActionStatus::Rejected:
            return m_commandService
                       ? m_commandService->MakeRejected(title, result.message)
                       : MakeResult(EditorCommandActionStatus::Rejected,
                                    title,
                                    result.message.empty() ? title + " rejected"
                                                           : result.message,
                                    EditorOperationStatus::Rejected);
        case EditorDocumentActionStatus::Unavailable:
            return m_commandService
                       ? m_commandService->MakeUnavailable(title, result.message)
                       : MakeResult(EditorCommandActionStatus::Unavailable,
                                    title,
                                    result.message.empty()
                                        ? title + " unavailable"
                                        : result.message,
                                    EditorOperationStatus::Unavailable);
    }

    return m_commandService ? m_commandService->MakeFailed(title, result.message)
                            : MakeResult(EditorCommandActionStatus::Failed,
                                         title,
                                         result.message.empty()
                                             ? title + " failed"
                                             : result.message,
                                         EditorOperationStatus::Rejected);
}

EditorCommandActionResult EditorDocumentCommandService::SetLastResult(
    EditorCommandActionResult result)
{
    m_lastResult = std::move(result);
    return m_lastResult;
}

EditorCommandActionResult EditorDocumentCommandService::MakeResult(
    EditorCommandActionStatus status,
    std::string title,
    std::string message,
    EditorOperationStatus operationStatus) const
{
    EditorCommandActionResult result;
    result.status = status;
    result.title = std::move(title);
    result.message = std::move(message);
    result.operationStatus = operationStatus;
    return result;
}

} // namespace RVX::Editor
