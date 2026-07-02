/**
 * @file EditorDocumentCommandService.h
 * @brief Document command action plan adapter service.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorCommandExecutionService.h"
#include "Editor/EditorDocumentActionService.h"

#include <functional>
#include <string>

namespace RVX::Editor
{

using EditorDocumentCommandUIRequest = std::function<void()>;

/**
 * @brief Bridges document guards to typed command action results.
 */
class EditorDocumentCommandService
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

    // =========================================================================
    // Guarded Commands
    // =========================================================================

    EditorCommandActionResult RequestGuardedAction(
        std::string actionId,
        std::string label,
        std::function<EditorCommandActionResult()> continuation,
        bool interactiveModalAvailable,
        EditorDocumentCommandUIRequest openUnsavedChangesModal = {});
    EditorCommandActionResult BuildCommandResult(
        const std::string& title,
        const EditorDocumentActionResult& result) const;

    const EditorCommandActionResult& GetLastResult() const
    {
        return m_lastResult;
    }

private:
    EditorCommandActionResult SetLastResult(EditorCommandActionResult result);
    EditorCommandActionResult MakeResult(EditorCommandActionStatus status,
                                         std::string title,
                                         std::string message,
                                         EditorOperationStatus operationStatus) const;

    EditorCommandExecutionService* m_commandService = nullptr;
    EditorDocumentActionService* m_documentActions = nullptr;
    EditorCommandActionResult m_lastResult;
};

} // namespace RVX::Editor
