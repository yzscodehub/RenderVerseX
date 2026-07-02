/**
 * @file EditorFrameLifecycleService.h
 * @brief Editor end-frame lifecycle orchestration service
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorFrameCoordinator.h"
#include "Editor/EditorMainFramePresentationService.h"
#include "Editor/EditorScreenshotRequestService.h"

#include <memory>
#include <vector>

struct GLFWwindow;

namespace RVX
{
    class RenderContext;
    class SceneManager;
    class SceneRenderer;

    namespace UI
    {
        class UIRenderer;
    }
}

namespace RVX::Editor
{

class EditorFrameSubmissionService;
class EditorMainFramebufferService;
class EditorNativeUIRenderStatsService;
class EditorNativeUISubmissionService;
class EditorRunLoopService;
class EditorScreenshotService;
class EditorViewportRenderService;
class IEditorPanel;
class IEditorUIBackend;
struct EditorNativeUIRenderStats;
struct EditorViewportSceneRenderStats;

struct EditorFrameLifecycleDesc
{
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
    SceneRenderer* sceneRenderer = nullptr;
    UI::UIRenderer* runtimeUIRenderer = nullptr;
    UI::UIRenderer* editorUIRenderer = nullptr;
    SceneManager* sceneManager = nullptr;
    IEditorUIBackend* editorUIBackend = nullptr;
    EditorFrameCoordinator* frameCoordinator = nullptr;
    EditorFrameSubmissionService* frameSubmissionService = nullptr;
    EditorMainFramePresentationService* mainFramePresentationService = nullptr;
    const EditorMainFramebufferService* mainFramebufferService = nullptr;
    EditorNativeUISubmissionService* nativeUISubmissionService = nullptr;
    EditorNativeUIRenderStatsService* nativeUIRenderStatsService = nullptr;
    EditorViewportRenderService* viewportRenderService = nullptr;
    EditorRunLoopService* runLoopService = nullptr;
    EditorScreenshotRequestService* screenshotRequestService = nullptr;
    EditorScreenshotService* screenshotService = nullptr;
    EditorViewportSceneRenderStats* viewportStats = nullptr;
    EditorNativeUIRenderStats* nativeUIStats = nullptr;
    const std::vector<std::shared_ptr<IEditorPanel>>* legacyPanels = nullptr;
    bool includeLegacyPanels = false;
    bool mainSwapChainReady = false;
    bool debugImGuiMainFramebufferRendered = false;
};

struct EditorFrameLifecycleResult
{
    EditorMainFramePrepareResult prepareResult;
    EditorFrameCoordinatorStats frameCoordinatorStats;
    EditorScreenshotRequestResult screenshotRequestResult;
    EditorMainFramePresentResult presentResult;
    bool frameCoordinatorSubmitted = false;
};

class EditorFrameLifecycleService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorFrameLifecycleResult CompleteFrame(
        const EditorFrameLifecycleDesc& desc) const;
};

} // namespace RVX::Editor
