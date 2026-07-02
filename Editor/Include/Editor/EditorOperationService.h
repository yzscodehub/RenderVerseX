/**
 * @file EditorOperationService.h
 * @brief Shared editor operation service boundary.
 */

#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>

namespace RVX
{
class SceneEntity;
}

namespace RVX::Editor
{

class EditorContext;
class EditorDocumentSession;
class EditorSelectionService;

enum class EditorOperationStatus : uint8
{
    Succeeded = 0,
    Unavailable,
    Rejected
};

struct EditorOperationResult
{
    EditorOperationStatus status = EditorOperationStatus::Unavailable;
    std::string message;
    SceneEntity* entity = nullptr;

    bool Succeeded() const { return status == EditorOperationStatus::Succeeded; }
    explicit operator bool() const { return Succeeded(); }
};

/**
 * @brief Service boundary for document and scene editing operations.
 */
class EditorOperationService
{
public:
    // =========================================================================
    // Dependencies
    // =========================================================================

    void SetDocumentSession(EditorDocumentSession* documentSession)
    {
        m_documentSession = documentSession;
    }
    void SetSelectionService(EditorSelectionService* selectionService)
    {
        m_selectionService = selectionService;
    }

    // =========================================================================
    // Query
    // =========================================================================

    bool CanCreateEntity() const;
    bool CanDeleteSelection() const;
    bool CanUndo() const;
    bool CanRedo() const;
    const EditorOperationResult& GetLastResult() const { return m_lastResult; }

    // =========================================================================
    // Document Operations
    // =========================================================================

    EditorOperationResult NewScene();
    EditorOperationResult OpenScene(const std::filesystem::path& path);
    EditorOperationResult SaveScene();
    EditorOperationResult SaveSceneAs(const std::filesystem::path& path);

    // =========================================================================
    // Scene Operations
    // =========================================================================

    EditorOperationResult CreateEmptyEntity(std::string name = "Entity");
    EditorOperationResult DeleteSelection();
    EditorOperationResult Undo();
    EditorOperationResult Redo();

private:
    EditorContext* GetContext() const;
    EditorOperationResult Succeed(std::string message = {},
                                  SceneEntity* entity = nullptr);
    EditorOperationResult Fail(EditorOperationStatus status,
                               std::string message);

    EditorDocumentSession* m_documentSession = nullptr;
    EditorSelectionService* m_selectionService = nullptr;
    EditorOperationResult m_lastResult;
};

} // namespace RVX::Editor
