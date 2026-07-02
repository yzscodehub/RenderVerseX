/**
 * @file EditorViewportRenderService.h
 * @brief Editor viewport render-target submission helper
 */

#pragma once

#include "Core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    class RenderContext;
    class RHICommandContext;
    class SceneManager;
    class SceneRenderer;

    namespace UI
    {
        class UIRenderer;
    }
}

namespace RVX::Editor
{

class IEditorPanel;
class IEditorUIBackend;

struct EditorViewportSceneRenderStats
{
    bool renderContextReady = false;
    bool sceneRendererReady = false;
    uint32 readyViewportCount = 0;
    uint32 sceneRenderedViewportCount = 0;
    uint32 fallbackClearViewportCount = 0;
    uint32 runtimeUIOverlayViewportCount = 0;
    uint32 runtimeUIOverlayDrawCallCount = 0;
    uint64 frameCount = 0;
    bool runtimeUIRendererReady = false;
    bool runtimeUIPipelineReady = false;
    bool submittedInEditorRHIFrame = false;
    std::string fallbackReason;
};

struct EditorViewportRenderDesc
{
    RenderContext* renderContext = nullptr;
    RHICommandContext* commandContext = nullptr;
    SceneRenderer* sceneRenderer = nullptr;
    UI::UIRenderer* runtimeUIRenderer = nullptr;
    SceneManager* sceneManager = nullptr;
    IEditorUIBackend* editorUIBackend = nullptr;
    const std::vector<std::shared_ptr<IEditorPanel>>* legacyPanels = nullptr;
    bool includeLegacyPanels = false;
    bool submittedInEditorRHIFrame = true;
};

class EditorViewportRenderService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    const EditorViewportSceneRenderStats& Submit(
        const EditorViewportRenderDesc& desc);

    // =========================================================================
    // Diagnostics
    // =========================================================================
    const EditorViewportSceneRenderStats& GetStats() const { return m_stats; }

private:
    EditorViewportSceneRenderStats m_stats;
};

} // namespace RVX::Editor
