/**
 * @file EditorFileDialog.cpp
 * @brief Native editor file dialog abstraction implementation
 */

#include "Editor/EditorFileDialog.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#endif

#include <cwchar>
#include <sstream>
#include <utility>

namespace RVX::Editor
{
namespace
{
    std::string JoinPatterns(const std::vector<std::string>& patterns)
    {
        std::ostringstream stream;
        for (size_t i = 0; i < patterns.size(); ++i)
        {
            if (i > 0)
            {
                stream << ';';
            }
            stream << patterns[i];
        }
        return stream.str();
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

    std::wstring BuildWin32FilterBlob(const std::vector<EditorFileDialogFilter>& filters)
    {
        std::wstring blob;
        for (const EditorFileDialogFilter& filter : filters)
        {
            if (filter.patterns.empty())
            {
                continue;
            }

            blob += Utf8ToWide(filter.displayName.empty() ? "Files" : filter.displayName);
            blob.push_back(L'\0');
            blob += Utf8ToWide(JoinPatterns(filter.patterns));
            blob.push_back(L'\0');
        }
        blob.push_back(L'\0');
        return blob;
    }

    std::string Win32DialogError(DWORD errorCode)
    {
        std::ostringstream stream;
        stream << "Windows file dialog failed with error " << errorCode;
        return stream.str();
    }

    class Win32EditorFileDialogProvider final : public IEditorFileDialogProvider
    {
    public:
        bool IsAvailable() const override { return true; }

        EditorFileDialogResult OpenFile(const EditorFileDialogDesc& desc) override
        {
            return ShowDialog(desc, true);
        }

        EditorFileDialogResult SaveFile(const EditorFileDialogDesc& desc) override
        {
            return ShowDialog(desc, false);
        }

    private:
        EditorFileDialogResult ShowDialog(const EditorFileDialogDesc& desc, bool open)
        {
            constexpr DWORD RVX_FILE_DIALOG_BUFFER_SIZE = 4096;
            wchar_t fileName[RVX_FILE_DIALOG_BUFFER_SIZE] = {};
            const std::wstring defaultPath = desc.defaultPath.wstring();
            if (!defaultPath.empty())
            {
                wcsncpy_s(fileName,
                          RVX_FILE_DIALOG_BUFFER_SIZE,
                          defaultPath.c_str(),
                          _TRUNCATE);
            }

            const std::wstring title = Utf8ToWide(desc.title);
            const std::wstring initialDirectory = desc.initialDirectory.wstring();
            const std::wstring defaultExtension = Utf8ToWide(desc.defaultExtension);
            const std::wstring filters = BuildWin32FilterBlob(desc.filters);

            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = nullptr;
            ofn.lpstrFile = fileName;
            ofn.nMaxFile = RVX_FILE_DIALOG_BUFFER_SIZE;
            ofn.lpstrFilter = filters.empty() ? nullptr : filters.c_str();
            ofn.lpstrTitle = title.empty() ? nullptr : title.c_str();
            ofn.lpstrInitialDir =
                initialDirectory.empty() ? nullptr : initialDirectory.c_str();
            ofn.lpstrDefExt =
                defaultExtension.empty() ? nullptr : defaultExtension.c_str();
            ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
            if (open)
            {
                ofn.Flags |= OFN_FILEMUSTEXIST;
            }
            else
            {
                ofn.Flags |= OFN_OVERWRITEPROMPT;
            }

            const BOOL accepted = open ? GetOpenFileNameW(&ofn) : GetSaveFileNameW(&ofn);
            if (accepted)
            {
                EditorFileDialogResult result;
                result.accepted = true;
                result.path = std::filesystem::path(fileName);
                return result;
            }

            const DWORD errorCode = CommDlgExtendedError();
            if (errorCode == 0)
            {
                return {};
            }

            EditorFileDialogResult result;
            result.error = Win32DialogError(errorCode);
            return result;
        }
    };
#endif
}

EditorFileDialogService::EditorFileDialogService()
    : m_provider(CreateNativeProvider())
{
}

EditorFileDialogService::EditorFileDialogService(
    std::unique_ptr<IEditorFileDialogProvider> provider)
    : m_provider(std::move(provider))
{
}

void EditorFileDialogService::SetProvider(
    std::unique_ptr<IEditorFileDialogProvider> provider)
{
    m_provider = std::move(provider);
}

bool EditorFileDialogService::CanOpenFile() const
{
    return m_provider && m_provider->IsAvailable();
}

bool EditorFileDialogService::CanSaveFile() const
{
    return m_provider && m_provider->IsAvailable();
}

EditorFileDialogResult EditorFileDialogService::OpenFile(
    const EditorFileDialogDesc& desc)
{
    if (!CanOpenFile())
    {
        return Unsupported("open file");
    }
    return m_provider->OpenFile(desc);
}

EditorFileDialogResult EditorFileDialogService::SaveFile(
    const EditorFileDialogDesc& desc)
{
    if (!CanSaveFile())
    {
        return Unsupported("save file");
    }
    return m_provider->SaveFile(desc);
}

EditorFileDialogResult EditorFileDialogService::OpenSceneFile()
{
    return OpenFile(MakeSceneFileDialogDesc());
}

EditorFileDialogResult EditorFileDialogService::SaveSceneFileAs(
    const std::filesystem::path& defaultPath)
{
    return SaveFile(MakeSceneFileDialogDesc(defaultPath));
}

EditorFileDialogDesc EditorFileDialogService::MakeSceneFileDialogDesc(
    const std::filesystem::path& defaultPath)
{
    EditorFileDialogDesc desc;
    desc.title = defaultPath.empty() ? "Open Scene" : "Save Scene As";
    desc.defaultPath = defaultPath;
    desc.initialDirectory = defaultPath.parent_path();
    desc.defaultExtension = "rvxscene";
    desc.filters.push_back(EditorFileDialogFilter{
        "RenderVerseX Scene (*.rvxscene)",
        {"*.rvxscene"}});
    desc.filters.push_back(EditorFileDialogFilter{"All Files (*.*)", {"*.*"}});
    return desc;
}

std::unique_ptr<IEditorFileDialogProvider>
EditorFileDialogService::CreateNativeProvider()
{
#if defined(_WIN32)
    return std::make_unique<Win32EditorFileDialogProvider>();
#else
    return nullptr;
#endif
}

EditorFileDialogResult EditorFileDialogService::Unsupported(const char* operation)
{
    EditorFileDialogResult result;
    result.error = std::string("No native file dialog provider is available for ") +
                   operation;
    return result;
}

} // namespace RVX::Editor
