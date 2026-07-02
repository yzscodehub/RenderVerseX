/**
 * @file EditorCommandExecutionService.h
 * @brief Typed editor command execution adapter service.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorOperationService.h"

#include <functional>
#include <string>
#include <utility>

namespace RVX::Editor
{

class EditorNotificationService;

enum class EditorCommandActionStatus : uint8
{
    Succeeded = 0,
    Deferred,
    WaitingForUser,
    Cancelled,
    Failed,
    Unavailable,
    Rejected
};

struct EditorCommandActionResult
{
    EditorCommandActionStatus status = EditorCommandActionStatus::Unavailable;
    std::string title;
    std::string message;
    EditorOperationStatus operationStatus = EditorOperationStatus::Unavailable;

    bool Succeeded() const
    {
        return status == EditorCommandActionStatus::Succeeded;
    }
    explicit operator bool() const { return Succeeded(); }
};

struct EditorCommandActionOptions
{
    bool notify = true;
    bool logFailure = true;
    bool refreshDocumentCommands = true;
};

struct EditorCommandActionPlan
{
    std::string title;
    std::function<EditorCommandActionResult()> execute;
    EditorCommandActionOptions options;
};

using EditorCommandActionRefreshCallback = std::function<void()>;

/**
 * @brief Executes editor command actions and publishes their typed results.
 */
class EditorCommandExecutionService
{
public:
    // =========================================================================
    // Dependencies
    // =========================================================================

    void SetNotificationService(EditorNotificationService* notificationService)
    {
        m_notificationService = notificationService;
    }
    void SetRefreshCallback(EditorCommandActionRefreshCallback refreshCallback)
    {
        m_refreshCallback = std::move(refreshCallback);
    }

    // =========================================================================
    // Execution
    // =========================================================================

    EditorCommandActionResult ExecutePlan(EditorCommandActionPlan plan);
    EditorCommandActionResult ExecuteOperation(
        std::string title,
        std::function<EditorOperationResult()> operation,
        EditorCommandActionOptions options = {});
    EditorCommandActionResult ExecuteVoid(
        std::string title,
        std::function<void()> action,
        std::string successMessage = {},
        EditorCommandActionOptions options = {});
    EditorCommandActionResult PublishWarning(
        std::string title,
        std::string message,
        EditorCommandActionOptions options = {});
    EditorCommandActionResult PublishError(
        std::string title,
        std::string message,
        EditorCommandActionOptions options = {});
    EditorCommandActionResult BuildOperationResult(
        std::string title,
        const EditorOperationResult& operationResult) const;
    EditorCommandActionResult MakeSucceeded(
        std::string title,
        std::string message = {}) const;
    EditorCommandActionResult MakeDeferred(
        std::string title,
        std::string message = {}) const;
    EditorCommandActionResult MakeWaitingForUser(
        std::string title,
        std::string message = {}) const;
    EditorCommandActionResult MakeCancelled(
        std::string title,
        std::string message = {}) const;
    EditorCommandActionResult MakeUnavailable(
        std::string title,
        std::string message = {}) const;
    EditorCommandActionResult MakeRejected(
        std::string title,
        std::string message = {}) const;
    EditorCommandActionResult MakeFailed(
        std::string title,
        std::string message = {}) const;

    const EditorCommandActionResult& GetLastResult() const
    {
        return m_lastResult;
    }

private:
    EditorCommandActionResult MakeResult(EditorCommandActionStatus status,
                                         std::string title,
                                         std::string message,
                                         EditorOperationStatus operationStatus) const;
    EditorCommandActionResult StoreAndPublish(EditorCommandActionResult result,
                                              const EditorCommandActionOptions& options);
    void RefreshIfNeeded(const EditorCommandActionOptions& options) const;

    EditorNotificationService* m_notificationService = nullptr;
    EditorCommandActionRefreshCallback m_refreshCallback;
    EditorCommandActionResult m_lastResult;
};

} // namespace RVX::Editor
