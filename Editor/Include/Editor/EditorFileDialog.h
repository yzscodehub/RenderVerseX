/**
 * @file EditorFileDialog.h
 * @brief Native editor file dialog abstraction
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace RVX::Editor
{

struct EditorFileDialogFilter
{
    std::string displayName;
    std::vector<std::string> patterns;
};

struct EditorFileDialogDesc
{
    std::string title;
    std::filesystem::path initialDirectory;
    std::filesystem::path defaultPath;
    std::string defaultExtension;
    std::vector<EditorFileDialogFilter> filters;
};

struct EditorFileDialogResult
{
    bool accepted = false;
    std::filesystem::path path;
    std::string error;
};

class IEditorFileDialogProvider
{
public:
    virtual ~IEditorFileDialogProvider() = default;

    virtual bool IsAvailable() const = 0;
    virtual EditorFileDialogResult OpenFile(const EditorFileDialogDesc& desc) = 0;
    virtual EditorFileDialogResult SaveFile(const EditorFileDialogDesc& desc) = 0;
};

class EditorFileDialogService
{
public:
    EditorFileDialogService();
    explicit EditorFileDialogService(std::unique_ptr<IEditorFileDialogProvider> provider);

    void SetProvider(std::unique_ptr<IEditorFileDialogProvider> provider);
    bool HasProvider() const { return m_provider != nullptr; }
    bool CanOpenFile() const;
    bool CanSaveFile() const;

    EditorFileDialogResult OpenFile(const EditorFileDialogDesc& desc);
    EditorFileDialogResult SaveFile(const EditorFileDialogDesc& desc);

    EditorFileDialogResult OpenSceneFile();
    EditorFileDialogResult SaveSceneFileAs(const std::filesystem::path& defaultPath);

    static EditorFileDialogDesc MakeSceneFileDialogDesc(
        const std::filesystem::path& defaultPath = {});
    static std::unique_ptr<IEditorFileDialogProvider> CreateNativeProvider();

private:
    static EditorFileDialogResult Unsupported(const char* operation);

    std::unique_ptr<IEditorFileDialogProvider> m_provider;
};

} // namespace RVX::Editor
