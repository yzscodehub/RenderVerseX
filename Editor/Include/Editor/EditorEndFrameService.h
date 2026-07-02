/**
 * @file EditorEndFrameService.h
 * @brief Editor end-frame orchestration boundary.
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorFrameLifecycleService.h"
#include "Editor/EditorLegacyDebugUIService.h"
#include "Editor/EditorMainSwapChainService.h"
#include "Editor/EditorNativeUIFrameService.h"

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

class DebugImGuiLayer;
class EditorFrameCoordinator;
class EditorFrameSubmissionService;
class EditorMainFramePresentationService;
class EditorMainFramebufferService;
class EditorNativeUIRenderStatsService;
class EditorNativeUISubmissionService;
class EditorRunLoopService;
class EditorScreenshotRequestService;
class EditorScreenshotService;
class EditorViewportRenderService;
class IEditorPanel;
class IEditorUIBackend;
struct EditorNativeUIRenderStats;
struct EditorViewportSceneRenderStats;

struct EditorEndFrameDesc
{
    GLFWwindow* window = nullptr;
    RenderContext* renderContext = nullptr;
    SceneRenderer* sceneRenderer = nullptr;
    UI::UIRenderer* runtimeUIRenderer = nullptr;
    UI::UIRenderer* editorUIRenderer = nullptr;
    SceneManager* sceneManager = nullptr;
    IEditorUIBackend* editorUIBackend = nullptr;
    EditorNativeUIFrameService* nativeUIFrameService = nullptr;
    EditorMainSwapChainService* mainSwapChainService = nullptr;
    EditorLegacyDebugUIService* legacyDebugUIService = nullptr;
    EditorFrameLifecycleService* frameLifecycleService = nullptr;
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
    DebugImGuiLayer* debugImGuiLayer = nullptr;
    const std::vector<std::shared_ptr<IEditorPanel>>* legacyPanels = nullptr;
    bool includeLegacyPanels = false;
    bool debugFrameActive = false;
};

struct EditorEndFrameResult
{
    EditorNativeUIEndFrameResult nativeUIEndFrameResult;
    EditorMainSwapChainEnsureResult mainSwapChainResult;
    EditorLegacyDebugUIMainFramebufferResult debugUIMainFramebufferResult;
    EditorFrameLifecycleResult frameLifecycleResult;
    bool completed = false;
    bool frameLifecycleCompleted = false;
    bool debugFrameActiveAfterEnd = false;

    explicit operator bool() const { return completed; }
};

class EditorEndFrameService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorEndFrameResult EndFrame(const EditorEndFrameDesc& desc) const;
};

} // namespace RVX::Editor
