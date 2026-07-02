/**
 * @file EditorUnsavedChanges.h
 * @brief Unsaved document change guard for destructive editor commands
 */

#pragma once

#include "Core/Types.h"

#include <memory>
#include <string>

namespace RVX::Editor
{

class EditorDocumentSession;
class EditorFileDialogService;

enum class EditorUnsavedChangesChoice : uint8
{
    Save,
    Discard,
    Cancel
};

struct EditorUnsavedChangesPromptDesc
{
    std::string title;
    std::string documentName;
    std::string message;
};

struct EditorUnsavedChangesPromptResult
{
    EditorUnsavedChangesChoice choice = EditorUnsavedChangesChoice::Cancel;
    std::string error;
};

class IEditorUnsavedChangesPromptProvider
{
public:
    virtual ~IEditorUnsavedChangesPromptProvider() = default;

    virtual bool IsAvailable() const = 0;
    virtual EditorUnsavedChangesPromptResult Prompt(
        const EditorUnsavedChangesPromptDesc& desc) = 0;
};

class EditorUnsavedChangesGuard
{
public:
    EditorUnsavedChangesGuard(EditorDocumentSession& document,
                              EditorFileDialogService& fileDialogs);
    EditorUnsavedChangesGuard(
        EditorDocumentSession& document,
        EditorFileDialogService& fileDialogs,
        std::unique_ptr<IEditorUnsavedChangesPromptProvider> promptProvider);

    void SetPromptProvider(
        std::unique_ptr<IEditorUnsavedChangesPromptProvider> promptProvider);

    bool CanPrompt() const;
    bool ConfirmSaveDiscardOrCancel();
    bool SaveActiveDocument();
    EditorUnsavedChangesPromptDesc BuildPromptDesc() const;

    const std::string& GetLastError() const { return m_lastError; }

    static std::unique_ptr<IEditorUnsavedChangesPromptProvider>
    CreateNativePromptProvider();

private:
    bool Fail(std::string error);
    void ClearError();

    EditorDocumentSession* m_document = nullptr;
    EditorFileDialogService* m_fileDialogs = nullptr;
    std::unique_ptr<IEditorUnsavedChangesPromptProvider> m_promptProvider;
    std::string m_lastError;
};

} // namespace RVX::Editor
