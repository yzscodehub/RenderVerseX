/**
 * @file EditorNativeUIFrameService.cpp
 * @brief Native editor UI frame recording helper implementation
 */

#include "Editor/EditorNativeUIFrameService.h"

#include "Editor/EditorAutomationScenarioService.h"
#include "Editor/EditorShortcutProfileLifecycleService.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/IEditorUIBackend.h"
#include "UI/UIRenderer.h"

#include <algorithm>
#include <utility>

namespace RVX::Editor
{

EditorNativeUIBeginFrameResult EditorNativeUIFrameService::BeginFrame(
    const EditorNativeUIBeginFrameDesc& desc) const
{
    EditorNativeUIBeginFrameResult result;
    result.backendAvailable = desc.backend != nullptr;
    if (!desc.backend)
    {
        result.completed = true;
        return result;
    }

    if (!desc.windowFrame || !desc.windowFrame->processed)
    {
        result.error = "Editor window frame state is unavailable";
        return result;
    }

    if (!desc.inputBridgeService)
    {
        result.error = "Editor input bridge service is unavailable";
        return result;
    }

    EditorInputBridgeFrameDesc inputDesc;
    inputDesc.bridge = desc.inputBridge;
    inputDesc.editorUIBackend = desc.backend;
    inputDesc.windowWidth = desc.windowFrame->windowWidth;
    inputDesc.windowHeight = desc.windowFrame->windowHeight;
    inputDesc.framebufferWidth = desc.windowFrame->framebufferWidth;
    inputDesc.framebufferHeight = desc.windowFrame->framebufferHeight;
    result.inputResult = desc.inputBridgeService->PrepareFrameInput(inputDesc);
    result.inputPrepared = static_cast<bool>(result.inputResult);
    if (!result.inputPrepared)
    {
        result.error = std::move(result.inputResult.error);
        return result;
    }

    EditorUIFrameDesc frameDesc;
    frameDesc.surfaceWidth =
        static_cast<uint32>(std::max(0, desc.windowFrame->framebufferWidth));
    frameDesc.surfaceHeight =
        static_cast<uint32>(std::max(0, desc.windowFrame->framebufferHeight));
    frameDesc.deltaTime = desc.deltaTime;
    frameDesc.scaleFactor = desc.scaleFactor;
    frameDesc.inputScaleFactor = 1.0f;
    frameDesc.input = result.inputResult.input;

    result.surfaceWidth = frameDesc.surfaceWidth;
    result.surfaceHeight = frameDesc.surfaceHeight;
    result.frameBegun = desc.backend->BeginFrame(frameDesc);
    if (!result.frameBegun)
    {
        result.error = "Native editor UI backend rejected frame";
        return result;
    }

    result.completed = true;
    return result;
}

EditorNativeUIUpdateResult EditorNativeUIFrameService::Update(
    const EditorNativeUIUpdateDesc& desc) const
{
    EditorNativeUIUpdateResult result;
    result.backendAvailable = desc.backend != nullptr;
    if (!desc.backend)
    {
        result.completed = true;
        return result;
    }

    desc.backend->Update(desc.deltaTime);
    result.updated = true;

    EditorCommandRegistry* registry = desc.backend->GetCommandRegistry();
    result.commandRegistryAvailable = registry != nullptr;
    if (registry && desc.shortcutProfileLifecycleService)
    {
        desc.shortcutProfileLifecycleService->TickAutosave(desc.deltaTime,
                                                           *registry);
        result.shortcutAutosaveTicked = true;
    }

    result.completed = true;
    return result;
}

EditorNativeUIPanelBuildResult EditorNativeUIFrameService::BuildPanels(
    const EditorNativeUIPanelBuildDesc& desc) const
{
    EditorNativeUIPanelBuildResult result;
    result.backendAvailable = desc.backend != nullptr;
    if (!desc.backend)
    {
        result.completed = true;
        return result;
    }

    if (desc.refreshNativeViewCommands)
    {
        desc.refreshNativeViewCommands();
        result.nativeViewCommandsRefreshed = true;
    }

    desc.backend->BuildPanels();
    result.panelsBuilt = true;

    if (desc.automationScenarioService)
    {
        EditorAutomationScenarioContext automationContext;
        automationContext.uiBackend = desc.backend;
        automationContext.documentSession = desc.documentSession;
        automationContext.settingsService = desc.settingsService;
        automationContext.shortcutProfileService =
            desc.shortcutProfileService;
        automationContext.refreshDocumentCommands =
            desc.refreshDocumentCommands;

        const EditorAutomationScenarioAdvanceResult automationResult =
            desc.automationScenarioService->Advance(automationContext);
        result.automationScenarioActive = automationResult.scenarioActive;
        result.automationAdvanced = automationResult.advanced;
        result.automationComplete = automationResult.complete;
        result.automationFailed = automationResult.failed;
        result.requestStop = automationResult.requestStop;
    }

    result.completed = true;
    return result;
}

EditorNativeUIFrameResult EditorNativeUIFrameService::RecordFrame(
    const EditorNativeUIFrameDesc& desc) const
{
    EditorNativeUIFrameResult result;
    result.hostReady = desc.backend && desc.backend->IsInitialized();
    result.rendererReady = desc.renderer && desc.renderer->IsInitialized();
    if (!result.hostReady || !result.rendererReady)
    {
        return result;
    }

    desc.backend->Render(*desc.renderer);
    const EditorUINativeRenderStats* hostStats =
        desc.backend->GetNativeRenderStats();
    if (!hostStats)
    {
        return result;
    }

    result.frameRecorded = hostStats->frameRecorded;
    result.commandCount = hostStats->commandCount;
    result.rectCount = hostStats->rectCount;
    result.textCount = hostStats->textCount;
    result.imageCount = hostStats->imageCount;
    result.vertexCount = hostStats->vertexCount;
    result.indexCount = hostStats->indexCount;
    result.menuCount = hostStats->menuCount;
    result.toolbarCount = hostStats->toolbarCount;
    result.statusItemCount = hostStats->statusItemCount;
    result.executableItemCount = hostStats->executableItemCount;
    result.menuBarHeight = hostStats->menuBarHeight;
    result.toolbarHeight = hostStats->toolbarHeight;
    result.statusBarHeight = hostStats->statusBarHeight;
    result.dockspaceTopReservedHeight = hostStats->topReservedHeight;
    result.dockspaceBottomReservedHeight = hostStats->bottomReservedHeight;
    if (result.frameRecorded)
    {
        result.frameCount = 1;
    }
    return result;
}

EditorNativeUIEndFrameResult EditorNativeUIFrameService::EndFrame(
    const EditorNativeUIEndFrameDesc& desc) const
{
    EditorNativeUIEndFrameResult result;
    result.backendAvailable = desc.backend != nullptr;
    if (!desc.backend)
    {
        result.completed = true;
        return result;
    }

    desc.backend->EndFrame();
    result.frameEnded = true;
    result.completed = true;
    return result;
}

} // namespace RVX::Editor
