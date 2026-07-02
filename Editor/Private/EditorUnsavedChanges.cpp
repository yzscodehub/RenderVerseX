/**
 * @file EditorUnsavedChanges.cpp
 * @brief Unsaved document change guard implementation
 */

#include "Editor/EditorUnsavedChanges.h"
#include "Editor/EditorDocument.h"
#include "Editor/EditorFileDialog.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <filesystem>
#include <utility>

namespace RVX::Editor
{
namespace
{
    std::filesystem::path BuildDefaultSavePath(const EditorDocumentSession& document)
    {
        std::filesystem::path defaultPath = document.GetScenePath();
        if (defaultPath.empty())
        {
            defaultPath = document.GetDisplayName().empty()
                              ? std::filesystem::path("Untitled.rvxscene")
                              : std::filesystem::path(document.GetDisplayName());
        }
        if (defaultPath.extension().empty())
        {
            defaultPath.replace_extension(".rvxscene");
        }
        return defaultPath;
    }

    EditorUnsavedChangesPromptDesc BuildPromptDescForDocument(
        const EditorDocumentSession& document)
    {
        EditorUnsavedChangesPromptDesc desc;
        desc.title = "Unsaved Scene Changes";
        desc.documentName =
            document.GetDisplayName().empty() ? "Untitled Scene" : document.GetDisplayName();
        desc.message = "Save changes to '" + desc.documentName +
                       "' before continuing?";
        return desc;
    }

#if defined(_WIN32)
    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }

        const int length = MultiByteToWideChar(CP_UTF8,
                                               0,
                                               value.data(),
                                               static_cast<int>(value.size()),
                                               nullptr,
                                               0);
        if (length <= 0)
        {
            return {};
        }

        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8,
                            0,
                            value.data(),
                            static_cast<int>(value.size()),
                            wide.data(),
                            length);
        return wide;
    }

    class Win32UnsavedChangesPromptProvider final
        : public IEditorUnsavedChangesPromptProvider
    {
    public:
        bool IsAvailable() const override { return true; }

        EditorUnsavedChangesPromptResult Prompt(
            const EditorUnsavedChangesPromptDesc& desc) override
        {
            const std::wstring title = Utf8ToWide(desc.title);
            const std::wstring message = Utf8ToWide(
                desc.message +
                "\n\nYes: save changes\nNo: discard changes\nCancel: return to the editor");

            const int result = MessageBoxW(nullptr,
                                           message.c_str(),
                                           title.c_str(),
                                           MB_ICONWARNING | MB_YESNOCANCEL);
            EditorUnsavedChangesPromptResult promptResult;
            switch (result)
            {
                case IDYES:
                    promptResult.choice = EditorUnsavedChangesChoice::Save;
                    break;
                case IDNO:
                    promptResult.choice = EditorUnsavedChangesChoice::Discard;
                    break;
                case IDCANCEL:
                default:
                    promptResult.choice = EditorUnsavedChangesChoice::Cancel;
                    break;
            }
            return promptResult;
        }
    };
#endif
}

EditorUnsavedChangesGuard::EditorUnsavedChangesGuard(
    EditorDocumentSession& document,
    EditorFileDialogService& fileDialogs)
    : EditorUnsavedChangesGuard(document,
                                fileDialogs,
                                CreateNativePromptProvider())
{
}

EditorUnsavedChangesGuard::EditorUnsavedChangesGuard(
    EditorDocumentSession& document,
    EditorFileDialogService& fileDialogs,
    std::unique_ptr<IEditorUnsavedChangesPromptProvider> promptProvider)
    : m_document(&document)
    , m_fileDialogs(&fileDialogs)
    , m_promptProvider(std::move(promptProvider))
{
}

void EditorUnsavedChangesGuard::SetPromptProvider(
    std::unique_ptr<IEditorUnsavedChangesPromptProvider> promptProvider)
{
    m_promptProvider = std::move(promptProvider);
}

bool EditorUnsavedChangesGuard::CanPrompt() const
{
    return m_promptProvider && m_promptProvider->IsAvailable();
}

EditorUnsavedChangesPromptDesc EditorUnsavedChangesGuard::BuildPromptDesc() const
{
    if (!m_document)
    {
        EditorUnsavedChangesPromptDesc desc;
        desc.title = "Unsaved Scene Changes";
        desc.documentName = "Untitled Scene";
        desc.message = "Save changes before continuing?";
        return desc;
    }

    return BuildPromptDescForDocument(*m_document);
}

bool EditorUnsavedChangesGuard::ConfirmSaveDiscardOrCancel()
{
    if (!m_document)
    {
        return Fail("Unsaved changes guard has no editor document session");
    }
    if (!m_document->IsDirty())
    {
        ClearError();
        return true;
    }
    if (!CanPrompt())
    {
        return Fail("Cannot resolve unsaved changes without a prompt provider");
    }

    EditorUnsavedChangesPromptResult result =
        m_promptProvider->Prompt(BuildPromptDescForDocument(*m_document));
    if (!result.error.empty())
    {
        return Fail(result.error);
    }

    switch (result.choice)
    {
        case EditorUnsavedChangesChoice::Save:
            return SaveActiveDocument();
        case EditorUnsavedChangesChoice::Discard:
            ClearError();
            return true;
        case EditorUnsavedChangesChoice::Cancel:
        default:
            ClearError();
            return false;
    }
}

bool EditorUnsavedChangesGuard::SaveActiveDocument()
{
    if (!m_document)
    {
        return Fail("Unsaved changes guard has no editor document session");
    }
    if (m_document->CanSaveScene())
    {
        if (!m_document->SaveScene())
        {
            return Fail(m_document->GetLastError());
        }
        ClearError();
        return true;
    }
    if (!m_fileDialogs)
    {
        return Fail("Cannot save untitled scene without a file dialog service");
    }

    EditorFileDialogResult dialogResult =
        m_fileDialogs->SaveSceneFileAs(BuildDefaultSavePath(*m_document));
    if (!dialogResult.accepted)
    {
        if (!dialogResult.error.empty())
        {
            return Fail(dialogResult.error);
        }
        ClearError();
        return false;
    }

    if (!m_document->SaveSceneAs(dialogResult.path))
    {
        return Fail(m_document->GetLastError());
    }

    ClearError();
    return true;
}

bool EditorUnsavedChangesGuard::Fail(std::string error)
{
    m_lastError = std::move(error);
    return false;
}

void EditorUnsavedChangesGuard::ClearError()
{
    m_lastError.clear();
}

std::unique_ptr<IEditorUnsavedChangesPromptProvider>
EditorUnsavedChangesGuard::CreateNativePromptProvider()
{
#if defined(_WIN32)
    return std::make_unique<Win32UnsavedChangesPromptProvider>();
#else
    return nullptr;
#endif
}

} // namespace RVX::Editor
