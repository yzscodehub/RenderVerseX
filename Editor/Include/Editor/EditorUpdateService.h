/**
 * @file EditorUpdateService.h
 * @brief Editor per-frame update orchestration boundary.
 */

#pragma once

#include "Core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX::Editor
{

class EditorNativeUIFrameService;
class EditorShortcutProfileLifecycleService;
class IEditorPanel;
class IEditorUIBackend;

struct EditorUpdateDesc
{
    IEditorUIBackend* editorUIBackend = nullptr;
    EditorNativeUIFrameService* nativeUIFrameService = nullptr;
    EditorShortcutProfileLifecycleService* shortcutProfileLifecycleService =
        nullptr;
    const std::vector<std::shared_ptr<IEditorPanel>>* legacyPanels = nullptr;
    float deltaTime = 0.0f;
    bool updateLegacyPanels = true;
};

struct EditorUpdateResult
{
    bool completed = false;
    bool nativeUIUpdateAttempted = false;
    bool nativeUIBackendAvailable = false;
    bool nativeUIUpdated = false;
    bool nativeCommandRegistryAvailable = false;
    bool shortcutAutosaveTicked = false;
    uint32 visibleLegacyPanelCount = 0;
    uint32 legacyPanelUpdateCount = 0;
    std::string error;

    explicit operator bool() const { return completed; }
};

class EditorUpdateService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    EditorUpdateResult Update(const EditorUpdateDesc& desc) const;

private:
    static EditorUpdateResult Fail(std::string error);
};

} // namespace RVX::Editor
