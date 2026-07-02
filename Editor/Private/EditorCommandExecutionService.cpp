/**
 * @file EditorCommandExecutionService.cpp
 * @brief Typed editor command execution adapter service implementation.
 */

#include "Editor/EditorCommandExecutionService.h"
#include "Core/Log.h"
#include "Editor/EditorNotificationService.h"

#include <utility>

namespace RVX::Editor
{
namespace
{
    EditorCommandActionStatus ToCommandActionStatus(
        EditorOperationStatus status)
    {
        switch (status)
        {
            case EditorOperationStatus::Succeeded:
                return EditorCommandActionStatus::Succeeded;
            case EditorOperationStatus::Unavailable:
                return EditorCommandActionStatus::Unavailable;
            case EditorOperationStatus::Rejected:
                return EditorCommandActionStatus::Rejected;
        }
        return EditorCommandActionStatus::Failed;
    }

    std::string BuildDefaultSuccessMessage(const std::string& title)
    {
        return title.empty() ? "Command completed" : title + " completed";
    }

    std::string BuildDefaultMessage(const std::string& title,
                                    const char* suffix)
    {
        if (title.empty())
        {
            return suffix ? std::string("Command") + suffix : "Command";
        }
        return suffix ? title + suffix : title;
    }

    bool IsFailureStatus(EditorCommandActionStatus status)
    {
        return status == EditorCommandActionStatus::Failed ||
               status == EditorCommandActionStatus::Unavailable ||
               status == EditorCommandActionStatus::Rejected;
    }

    bool IsInformationalStatus(EditorCommandActionStatus status)
    {
        return status == EditorCommandActionStatus::Deferred ||
               status == EditorCommandActionStatus::WaitingForUser ||
               status == EditorCommandActionStatus::Cancelled;
    }
}

EditorCommandActionResult EditorCommandExecutionService::ExecutePlan(
    EditorCommandActionPlan plan)
{
    if (!plan.execute)
    {
        return StoreAndPublish(
            MakeUnavailable(
                std::move(plan.title),
                "Command action plan is unavailable"),
            plan.options);
    }

    EditorCommandActionResult result = plan.execute();
    if (result.title.empty())
    {
        result.title = std::move(plan.title);
    }
    return StoreAndPublish(std::move(result), plan.options);
}

EditorCommandActionResult EditorCommandExecutionService::ExecuteOperation(
    std::string title,
    std::function<EditorOperationResult()> operation,
    EditorCommandActionOptions options)
{
    if (!operation)
    {
        EditorCommandActionResult result;
        result.status = EditorCommandActionStatus::Unavailable;
        result.title = std::move(title);
        result.message = result.title.empty()
                             ? "Command operation is unavailable"
                             : result.title + " failed: operation is unavailable";
        return StoreAndPublish(std::move(result), options);
    }

    return StoreAndPublish(BuildOperationResult(std::move(title), operation()),
                           options);
}

EditorCommandActionResult EditorCommandExecutionService::ExecuteVoid(
    std::string title,
    std::function<void()> action,
    std::string successMessage,
    EditorCommandActionOptions options)
{
    EditorCommandActionResult result;
    result.title = std::move(title);
    if (!action)
    {
        result.status = EditorCommandActionStatus::Unavailable;
        result.message = result.title.empty()
                             ? "Command action is unavailable"
                             : result.title + " failed: action is unavailable";
        return StoreAndPublish(std::move(result), options);
    }

    action();
    result.status = EditorCommandActionStatus::Succeeded;
    result.operationStatus = EditorOperationStatus::Succeeded;
    result.message = successMessage.empty()
                         ? BuildDefaultSuccessMessage(result.title)
                         : std::move(successMessage);
    return StoreAndPublish(std::move(result), options);
}

EditorCommandActionResult EditorCommandExecutionService::PublishWarning(
    std::string title,
    std::string message,
    EditorCommandActionOptions options)
{
    EditorCommandActionResult result;
    result.status = EditorCommandActionStatus::Rejected;
    result.title = std::move(title);
    result.message = std::move(message);
    return StoreAndPublish(std::move(result), options);
}

EditorCommandActionResult EditorCommandExecutionService::PublishError(
    std::string title,
    std::string message,
    EditorCommandActionOptions options)
{
    EditorCommandActionResult result;
    result.status = EditorCommandActionStatus::Failed;
    result.title = std::move(title);
    result.message = std::move(message);
    return StoreAndPublish(std::move(result), options);
}

EditorCommandActionResult EditorCommandExecutionService::BuildOperationResult(
    std::string title,
    const EditorOperationResult& operationResult) const
{
    EditorCommandActionResult result;
    result.status = ToCommandActionStatus(operationResult.status);
    result.title = std::move(title);
    result.operationStatus = operationResult.status;
    result.message = operationResult.message.empty()
                         ? BuildDefaultSuccessMessage(result.title)
                         : operationResult.message;
    return result;
}

EditorCommandActionResult EditorCommandExecutionService::MakeSucceeded(
    std::string title,
    std::string message) const
{
    std::string resolvedMessage =
        message.empty() ? BuildDefaultSuccessMessage(title) : std::move(message);
    return MakeResult(
        EditorCommandActionStatus::Succeeded,
        std::move(title),
        std::move(resolvedMessage),
        EditorOperationStatus::Succeeded);
}

EditorCommandActionResult EditorCommandExecutionService::MakeDeferred(
    std::string title,
    std::string message) const
{
    std::string resolvedMessage =
        message.empty() ? BuildDefaultMessage(title, " deferred")
                        : std::move(message);
    return MakeResult(
        EditorCommandActionStatus::Deferred,
        std::move(title),
        std::move(resolvedMessage),
        EditorOperationStatus::Unavailable);
}

EditorCommandActionResult EditorCommandExecutionService::MakeWaitingForUser(
    std::string title,
    std::string message) const
{
    std::string resolvedMessage =
        message.empty() ? BuildDefaultMessage(title, " waiting for user input")
                        : std::move(message);
    return MakeResult(
        EditorCommandActionStatus::WaitingForUser,
        std::move(title),
        std::move(resolvedMessage),
        EditorOperationStatus::Unavailable);
}

EditorCommandActionResult EditorCommandExecutionService::MakeCancelled(
    std::string title,
    std::string message) const
{
    std::string resolvedMessage =
        message.empty() ? BuildDefaultMessage(title, " cancelled")
                        : std::move(message);
    return MakeResult(
        EditorCommandActionStatus::Cancelled,
        std::move(title),
        std::move(resolvedMessage),
        EditorOperationStatus::Unavailable);
}

EditorCommandActionResult EditorCommandExecutionService::MakeUnavailable(
    std::string title,
    std::string message) const
{
    std::string resolvedMessage =
        message.empty() ? BuildDefaultMessage(title, " unavailable")
                        : std::move(message);
    return MakeResult(
        EditorCommandActionStatus::Unavailable,
        std::move(title),
        std::move(resolvedMessage),
        EditorOperationStatus::Unavailable);
}

EditorCommandActionResult EditorCommandExecutionService::MakeRejected(
    std::string title,
    std::string message) const
{
    std::string resolvedMessage =
        message.empty() ? BuildDefaultMessage(title, " rejected")
                        : std::move(message);
    return MakeResult(
        EditorCommandActionStatus::Rejected,
        std::move(title),
        std::move(resolvedMessage),
        EditorOperationStatus::Rejected);
}

EditorCommandActionResult EditorCommandExecutionService::MakeFailed(
    std::string title,
    std::string message) const
{
    std::string resolvedMessage =
        message.empty() ? BuildDefaultMessage(title, " failed") : std::move(message);
    return MakeResult(
        EditorCommandActionStatus::Failed,
        std::move(title),
        std::move(resolvedMessage),
        EditorOperationStatus::Rejected);
}

EditorCommandActionResult EditorCommandExecutionService::MakeResult(
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

EditorCommandActionResult EditorCommandExecutionService::StoreAndPublish(
    EditorCommandActionResult result,
    const EditorCommandActionOptions& options)
{
    m_lastResult = std::move(result);

    if (options.notify && m_notificationService)
    {
        if (m_lastResult.Succeeded())
        {
            m_notificationService->PublishSuccess(m_lastResult.title,
                                                  m_lastResult.message);
        }
        else if (m_lastResult.status == EditorCommandActionStatus::Rejected)
        {
            m_notificationService->PublishWarning(m_lastResult.title,
                                                  m_lastResult.message);
        }
        else if (IsInformationalStatus(m_lastResult.status))
        {
            m_notificationService->PublishInfo(m_lastResult.title,
                                               m_lastResult.message);
        }
        else
        {
            m_notificationService->PublishError(m_lastResult.title,
                                                m_lastResult.message);
        }
    }

    if (options.logFailure && IsFailureStatus(m_lastResult.status) &&
        !m_lastResult.message.empty())
    {
        RVX_CORE_WARN("{}", m_lastResult.message);
    }

    RefreshIfNeeded(options);
    return m_lastResult;
}

void EditorCommandExecutionService::RefreshIfNeeded(
    const EditorCommandActionOptions& options) const
{
    if (options.refreshDocumentCommands && m_refreshCallback)
    {
        m_refreshCallback();
    }
}

} // namespace RVX::Editor
