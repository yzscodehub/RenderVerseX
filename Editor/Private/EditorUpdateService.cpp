/**
 * @file EditorUpdateService.cpp
 * @brief Editor per-frame update orchestration boundary implementation.
 */

#include "Editor/EditorUpdateService.h"

#include "Editor/EditorNativeUIFrameService.h"
#include "Editor/Panels/IEditorPanel.h"

#include <utility>

namespace RVX::Editor
{

EditorUpdateResult EditorUpdateService::Update(
    const EditorUpdateDesc& desc) const
{
    if (!desc.nativeUIFrameService)
    {
        return Fail("Editor native UI frame service is unavailable");
    }

    EditorUpdateResult result;

    EditorNativeUIUpdateDesc nativeUIUpdateDesc;
    nativeUIUpdateDesc.backend = desc.editorUIBackend;
    nativeUIUpdateDesc.shortcutProfileLifecycleService =
        desc.shortcutProfileLifecycleService;
    nativeUIUpdateDesc.deltaTime = desc.deltaTime;

    const EditorNativeUIUpdateResult nativeUIUpdateResult =
        desc.nativeUIFrameService->Update(nativeUIUpdateDesc);
    result.nativeUIUpdateAttempted = true;
    result.nativeUIBackendAvailable = nativeUIUpdateResult.backendAvailable;
    result.nativeUIUpdated = nativeUIUpdateResult.updated;
    result.nativeCommandRegistryAvailable =
        nativeUIUpdateResult.commandRegistryAvailable;
    result.shortcutAutosaveTicked =
        nativeUIUpdateResult.shortcutAutosaveTicked;
    if (!nativeUIUpdateResult)
    {
        result.error = "Native editor UI update failed";
        return result;
    }

    if (desc.updateLegacyPanels && desc.legacyPanels)
    {
        for (const std::shared_ptr<IEditorPanel>& panel : *desc.legacyPanels)
        {
            if (!panel || !panel->IsVisible())
            {
                continue;
            }

            ++result.visibleLegacyPanelCount;
            panel->OnUpdate(desc.deltaTime);
            ++result.legacyPanelUpdateCount;
        }
    }

    result.completed = true;
    return result;
}

EditorUpdateResult EditorUpdateService::Fail(std::string error)
{
    EditorUpdateResult result;
    result.error = std::move(error);
    return result;
}

} // namespace RVX::Editor
