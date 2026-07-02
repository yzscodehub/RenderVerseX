/**
 * @file EditorFrameSubmissionService.h
 * @brief Editor frame submission client service
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorFrameCoordinator.h"
#include "Editor/EditorNativeUIRenderStatsService.h"
#include "Editor/EditorNativeUISubmissionService.h"
#include "Editor/EditorViewportRenderService.h"

#include <memory>
#include <vector>

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

class IEditorPanel;
class IEditorUIBackend;

struct EditorFrameSubmissionDesc
{
    RenderContext* renderContext = nullptr;
    SceneRenderer* sceneRenderer = nullptr;
    UI::UIRenderer* runtimeUIRenderer = nullptr;
    UI::UIRenderer* editorUIRenderer = nullptr;
    SceneManager* sceneManager = nullptr;
    IEditorUIBackend* editorUIBackend = nullptr;
    EditorViewportRenderService* viewportRenderService = nullptr;
    EditorNativeUISubmissionService* nativeUISubmissionService = nullptr;
    EditorNativeUIRenderStatsService* nativeUIRenderStatsService = nullptr;
    EditorViewportSceneRenderStats* viewportStats = nullptr;
    EditorNativeUIRenderStats* nativeUIStats = nullptr;
    const std::vector<std::shared_ptr<IEditorPanel>>* legacyPanels = nullptr;
    bool includeLegacyPanels = false;
    bool hasMainWindowSwapChain = false;
};

class EditorFrameSubmissionService final : public IEditorFrameClient
{
public:
    // =========================================================================
    // Configuration
    // =========================================================================
    void SetFrameDesc(const EditorFrameSubmissionDesc& desc);

    // =========================================================================
    // IEditorFrameClient
    // =========================================================================
    void SubmitViewportTargets(RHICommandContext& commandContext) override;
    void SubmitNativeMainFramebuffer(RHICommandContext* commandContext) override;
    void SubmitViewportTargetsStandalone() override;
    void SubmitNativeMainFramebufferStandalone() override;
    void HandleGraphicsContextUnavailable() override;

private:
    void SubmitViewportTargets(RHICommandContext& commandContext,
                               bool submittedInEditorRHIFrame);
    void SubmitNativeMainFramebufferInternal(
        RHICommandContext* commandContext);
    void RecordViewportUnavailable(const char* reason);

    EditorFrameSubmissionDesc m_desc;
};

} // namespace RVX::Editor
