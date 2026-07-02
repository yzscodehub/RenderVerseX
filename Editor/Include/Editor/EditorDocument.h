/**
 * @file EditorDocument.h
 * @brief Editor document/session state above EditorContext scene IO
 */

#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <string>

namespace RVX::Editor
{

class EditorContext;

struct EditorDocumentState
{
    std::filesystem::path scenePath;
    std::string displayName = "No Scene";
    uint64 revision = 0;
    bool hasActiveScene = false;
    bool hasScenePath = false;
    bool dirty = false;
};

/**
 * @brief Owns editor document state while delegating scene storage to EditorContext.
 */
class EditorDocumentSession
{
public:
    explicit EditorDocumentSession(EditorContext& context);

    bool NewScene();
    bool OpenScene(const std::filesystem::path& path);
    bool SaveScene();
    bool SaveSceneAs(const std::filesystem::path& path);

    bool HasActiveScene() const;
    bool HasScenePath() const { return !m_scenePath.empty(); }
    bool IsDirty() const;
    bool CanSaveScene() const;
    bool CanSaveSceneAs() const;

    const std::filesystem::path& GetScenePath() const { return m_scenePath; }
    const std::string& GetDisplayName() const { return m_displayName; }
    uint64 GetDocumentRevision() const { return m_documentRevision; }
    const std::string& GetLastError() const { return m_lastError; }

    EditorDocumentState GetState() const;

private:
    bool Fail(std::string error);
    void ClearError();
    void SetScenePath(std::filesystem::path path);
    void SetUntitledDisplayName();

    EditorContext* m_context = nullptr;
    std::filesystem::path m_scenePath;
    std::string m_displayName = "No Scene";
    std::string m_lastError;
    uint64 m_documentRevision = 0;
    uint32 m_untitledSceneIndex = 0;
};

} // namespace RVX::Editor
