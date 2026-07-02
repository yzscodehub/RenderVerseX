/**
 * @file EditorDocument.cpp
 * @brief Editor document/session state implementation
 */

#include "Editor/EditorDocument.h"
#include "Editor/EditorContext.h"

#include <system_error>
#include <utility>

namespace RVX::Editor
{
namespace
{
    std::filesystem::path NormalizeDocumentPath(const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::error_code ec;
        std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }

        return absolutePath.lexically_normal();
    }

    std::string MakeDisplayName(const std::filesystem::path& path)
    {
        const std::string filename = path.filename().string();
        return filename.empty() ? "Scene" : filename;
    }
}

EditorDocumentSession::EditorDocumentSession(EditorContext& context)
    : m_context(&context)
{
}

bool EditorDocumentSession::NewScene()
{
    if (!m_context)
    {
        return Fail("Editor document session has no editor context");
    }

    m_context->NewScene();
    m_scenePath.clear();
    SetUntitledDisplayName();
    ++m_documentRevision;
    ClearError();
    return true;
}

bool EditorDocumentSession::OpenScene(const std::filesystem::path& path)
{
    if (!m_context)
    {
        return Fail("Editor document session has no editor context");
    }

    const std::filesystem::path normalizedPath = NormalizeDocumentPath(path);
    if (normalizedPath.empty())
    {
        return Fail("Cannot open an editor scene from an empty path");
    }

    if (!m_context->LoadScene(normalizedPath.string()))
    {
        return Fail("Failed to open editor scene: " + normalizedPath.string());
    }

    SetScenePath(normalizedPath);
    ClearError();
    return true;
}

bool EditorDocumentSession::SaveScene()
{
    if (!HasActiveScene())
    {
        return Fail("Cannot save because there is no active editor scene");
    }
    if (!HasScenePath())
    {
        return Fail("Cannot save because the active editor scene has no path");
    }

    if (!m_context->SaveScene(m_scenePath.string()))
    {
        return Fail("Failed to save editor scene: " + m_scenePath.string());
    }

    ClearError();
    return true;
}

bool EditorDocumentSession::SaveSceneAs(const std::filesystem::path& path)
{
    if (!HasActiveScene())
    {
        return Fail("Cannot save because there is no active editor scene");
    }

    const std::filesystem::path normalizedPath = NormalizeDocumentPath(path);
    if (normalizedPath.empty())
    {
        return Fail("Cannot save an editor scene to an empty path");
    }

    if (!m_context->SaveScene(normalizedPath.string()))
    {
        return Fail("Failed to save editor scene: " + normalizedPath.string());
    }

    SetScenePath(normalizedPath);
    ClearError();
    return true;
}

bool EditorDocumentSession::HasActiveScene() const
{
    return m_context && m_context->GetActiveSceneManager() != nullptr;
}

bool EditorDocumentSession::IsDirty() const
{
    return m_context && m_context->IsSceneDirty();
}

bool EditorDocumentSession::CanSaveScene() const
{
    return HasActiveScene() && HasScenePath();
}

bool EditorDocumentSession::CanSaveSceneAs() const
{
    return HasActiveScene();
}

EditorDocumentState EditorDocumentSession::GetState() const
{
    EditorDocumentState state;
    state.scenePath = m_scenePath;
    state.displayName = m_displayName;
    state.revision = m_documentRevision;
    state.hasActiveScene = HasActiveScene();
    state.hasScenePath = HasScenePath();
    state.dirty = IsDirty();
    return state;
}

bool EditorDocumentSession::Fail(std::string error)
{
    m_lastError = std::move(error);
    return false;
}

void EditorDocumentSession::ClearError()
{
    m_lastError.clear();
}

void EditorDocumentSession::SetScenePath(std::filesystem::path path)
{
    m_scenePath = std::move(path);
    m_displayName = MakeDisplayName(m_scenePath);
    ++m_documentRevision;
}

void EditorDocumentSession::SetUntitledDisplayName()
{
    ++m_untitledSceneIndex;
    if (m_untitledSceneIndex <= 1)
    {
        m_displayName = "Untitled Scene";
        return;
    }

    m_displayName = "Untitled Scene " + std::to_string(m_untitledSceneIndex);
}

} // namespace RVX::Editor
