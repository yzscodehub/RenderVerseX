/**
 * @file EditorOperationService.cpp
 * @brief Shared editor operation service boundary implementation.
 */

#include "Editor/EditorOperationService.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorDocument.h"
#include "Editor/EditorSelectionService.h"
#include "Scene/SceneEntity.h"

#include <utility>

namespace RVX::Editor
{

bool EditorOperationService::CanCreateEntity() const
{
    const EditorContext* context = GetContext();
    return context && context->GetActiveSceneManager() != nullptr;
}

bool EditorOperationService::CanDeleteSelection() const
{
    return m_selectionService &&
           m_selectionService->GetSelectionType() == SelectionType::Entity &&
           m_selectionService->GetSelectedEntity() != nullptr;
}

bool EditorOperationService::CanUndo() const
{
    const EditorContext* context = GetContext();
    return context && context->CanUndo();
}

bool EditorOperationService::CanRedo() const
{
    const EditorContext* context = GetContext();
    return context && context->CanRedo();
}

EditorOperationResult EditorOperationService::NewScene()
{
    if (!m_documentSession)
    {
        return Fail(EditorOperationStatus::Unavailable,
                    "New Scene failed: document session is unavailable");
    }

    if (!m_documentSession->NewScene())
    {
        return Fail(EditorOperationStatus::Rejected,
                    "New Scene failed: " + m_documentSession->GetLastError());
    }

    return Succeed("New Scene");
}

EditorOperationResult EditorOperationService::OpenScene(
    const std::filesystem::path& path)
{
    if (!m_documentSession)
    {
        return Fail(EditorOperationStatus::Unavailable,
                    "Open Scene failed: document session is unavailable");
    }

    if (!m_documentSession->OpenScene(path))
    {
        return Fail(EditorOperationStatus::Rejected,
                    "Open Scene failed: " + m_documentSession->GetLastError());
    }

    return Succeed("Open Scene");
}

EditorOperationResult EditorOperationService::SaveScene()
{
    if (!m_documentSession)
    {
        return Fail(EditorOperationStatus::Unavailable,
                    "Save Scene failed: document session is unavailable");
    }

    if (!m_documentSession->SaveScene())
    {
        return Fail(EditorOperationStatus::Rejected,
                    "Save Scene failed: " + m_documentSession->GetLastError());
    }

    return Succeed("Save Scene");
}

EditorOperationResult EditorOperationService::SaveSceneAs(
    const std::filesystem::path& path)
{
    if (!m_documentSession)
    {
        return Fail(EditorOperationStatus::Unavailable,
                    "Save Scene As failed: document session is unavailable");
    }

    if (!m_documentSession->SaveSceneAs(path))
    {
        return Fail(EditorOperationStatus::Rejected,
                    "Save Scene As failed: " +
                        m_documentSession->GetLastError());
    }

    return Succeed("Save Scene As");
}

EditorOperationResult EditorOperationService::CreateEmptyEntity(
    std::string name)
{
    EditorContext* context = GetContext();
    if (!context || !CanCreateEntity())
    {
        return Fail(EditorOperationStatus::Unavailable,
                    "Create Empty failed: no active editor scene");
    }

    SceneEntity* entity =
        context->CreateEntityUndoable(name.empty() ? "Entity" : name);
    if (!entity)
    {
        return Fail(EditorOperationStatus::Rejected,
                    "Create Empty failed: entity creation was rejected");
    }

    if (m_selectionService)
    {
        m_selectionService->SelectEntity(entity);
    }

    return Succeed("Create Empty", entity);
}

EditorOperationResult EditorOperationService::DeleteSelection()
{
    EditorContext* context = GetContext();
    if (!context || !m_selectionService)
    {
        return Fail(EditorOperationStatus::Unavailable,
                    "Delete Selection failed: selection service is unavailable");
    }

    SceneEntity* selected = m_selectionService->GetSelectedEntity();
    if (!selected)
    {
        return Fail(EditorOperationStatus::Rejected,
                    "Delete Selection failed: no entity is selected");
    }

    if (!context->DestroyEntityUndoable(selected))
    {
        return Fail(EditorOperationStatus::Rejected,
                    "Delete Selection failed: selected entity could not be deleted");
    }

    return Succeed("Delete Selection");
}

EditorOperationResult EditorOperationService::Undo()
{
    EditorContext* context = GetContext();
    if (!context || !context->CanUndo())
    {
        return Fail(EditorOperationStatus::Rejected,
                    "Undo failed: no undo command is available");
    }

    context->Undo();
    return Succeed("Undo");
}

EditorOperationResult EditorOperationService::Redo()
{
    EditorContext* context = GetContext();
    if (!context || !context->CanRedo())
    {
        return Fail(EditorOperationStatus::Rejected,
                    "Redo failed: no redo command is available");
    }

    context->Redo();
    return Succeed("Redo");
}

EditorContext* EditorOperationService::GetContext() const
{
    return m_selectionService ? &m_selectionService->GetContext() : nullptr;
}

EditorOperationResult EditorOperationService::Succeed(std::string message,
                                                       SceneEntity* entity)
{
    m_lastResult.status = EditorOperationStatus::Succeeded;
    m_lastResult.message = std::move(message);
    m_lastResult.entity = entity;
    return m_lastResult;
}

EditorOperationResult EditorOperationService::Fail(
    EditorOperationStatus status,
    std::string message)
{
    m_lastResult.status = status;
    m_lastResult.message = std::move(message);
    m_lastResult.entity = nullptr;
    return m_lastResult;
}

} // namespace RVX::Editor
