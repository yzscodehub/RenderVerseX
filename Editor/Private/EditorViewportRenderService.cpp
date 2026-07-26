/**
 * @file EditorViewportRenderService.cpp
 * @brief Editor viewport render-target submission helper implementation
 */

#include "Editor/EditorViewportRenderService.h"
#include "Editor/Panels/IEditorPanel.h"
#include "Editor/Panels/NativeViewport.h"
#include "Editor/Panels/Viewport.h"
#include "Editor/UI/IEditorUIBackend.h"
#include "Render/Context/RenderContext.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/SceneRenderer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "UI/UIRenderer.h"

namespace RVX::Editor
{
namespace
{
    NativeViewportPanel* FindNativeViewportPanel(IEditorUIBackend* backend)
    {
        if (!backend)
        {
            return nullptr;
        }

        return dynamic_cast<NativeViewportPanel*>(
            backend->GetPanel(NativeViewportPanel::PanelId()));
    }

    ViewportRenderGraphDiagnostics BuildViewportRenderGraphDiagnostics(
        const SceneRendererFrameDiagnostics& diagnostics)
    {
        ViewportRenderGraphDiagnostics result;
        result.graphCompiled = diagnostics.graphCompiled;
        result.graphCompileValid = diagnostics.graphCompileValid;
        result.graphExecutionSkipped = diagnostics.graphExecutionSkipped;
        result.totalPasses = diagnostics.renderGraphTotalPasses;
        result.culledPasses = diagnostics.renderGraphCulledPasses;
        result.barrierCount = diagnostics.renderGraphBarrierCount;
        result.textureBarrierCount = diagnostics.renderGraphTextureBarrierCount;
        result.bufferBarrierCount = diagnostics.renderGraphBufferBarrierCount;
        result.validationWarningCount =
            diagnostics.renderGraphValidationWarningCount;
        result.validationErrorCount =
            diagnostics.renderGraphValidationErrorCount;
        result.skippedReason = diagnostics.skippedReason;
        if (!diagnostics.graphDiagnostics.empty())
        {
            result.firstDiagnostic = diagnostics.graphDiagnostics.front();
        }
        return result;
    }
}

const EditorViewportSceneRenderStats& EditorViewportRenderService::Submit(
    const EditorViewportRenderDesc& desc)
{
    m_stats = {};
    m_stats.renderContextReady =
        desc.renderContext && desc.renderContext->IsInitialized() &&
        desc.renderContext->GetDevice();
    m_stats.sceneRendererReady =
        desc.sceneRenderer && desc.sceneRenderer->IsInitialized();
    m_stats.runtimeUIRendererReady =
        desc.runtimeUIRenderer && desc.runtimeUIRenderer->IsInitialized();

    if (!m_stats.renderContextReady)
    {
        m_stats.fallbackReason = "RenderContext unavailable";
        return m_stats;
    }
    if (!desc.commandContext)
    {
        m_stats.fallbackReason =
            "RenderContext graphics command context unavailable";
        return m_stats;
    }

    std::vector<ViewportPanel*> readyViewports;
    if (desc.includeLegacyPanels && desc.legacyPanels)
    {
        for (const auto& panel : *desc.legacyPanels)
        {
            if (auto* viewport = dynamic_cast<ViewportPanel*>(panel.get()))
            {
                if (viewport->IsVisible() && viewport->IsRHIRenderTargetReady())
                {
                    readyViewports.push_back(viewport);
                }
            }
        }
    }

    if (NativeViewportPanel* nativeViewport =
            FindNativeViewportPanel(desc.editorUIBackend))
    {
        ViewportPanel& viewport = nativeViewport->GetViewportCore();
        if (desc.editorUIBackend->IsPanelVisible(NativeViewportPanel::PanelId()) &&
            viewport.IsRHIRenderTargetReady())
        {
            readyViewports.push_back(&viewport);
        }
    }

    m_stats.readyViewportCount = static_cast<uint32>(readyViewports.size());
    if (readyViewports.empty())
    {
        if (desc.sceneRenderer)
        {
            desc.sceneRenderer->ClearExternalRenderTarget();
        }
        return m_stats;
    }
    m_stats.submittedInEditorRHIFrame = desc.submittedInEditorRHIFrame;

    for (ViewportPanel* viewport : readyViewports)
    {
        const ViewportRenderTargetState& state = viewport->GetRenderTargetState();
        bool sceneRecorded = false;
        bool baseRendered = false;
        std::string fallbackReason;

        fallbackReason =
            "Editor scene publication is deferred; using fallback clear";
        viewport->MarkSceneRenderResult(false,
                                        state.colorState,
                                        state.depthState,
                                        fallbackReason);
        viewport->ClearRenderGraphDiagnostics();

        if (sceneRecorded)
        {
            ++m_stats.sceneRenderedViewportCount;
            baseRendered = true;
        }
        else if (viewport->RenderRHI(*desc.commandContext))
        {
            ++m_stats.fallbackClearViewportCount;
            baseRendered = true;
            if (m_stats.fallbackReason.empty())
            {
                m_stats.fallbackReason = fallbackReason;
            }
        }

        if (baseRendered &&
            viewport->IsRuntimeUIOverlayEnabled() &&
            desc.runtimeUIRenderer &&
            desc.runtimeUIRenderer->IsInitialized() &&
            desc.sceneRenderer &&
            desc.sceneRenderer->IsInitialized())
        {
            PipelineCache* pipelineCache = desc.sceneRenderer->GetPipelineCache();
            RHIPipeline* uiPipeline = pipelineCache
                                          ? pipelineCache->GetUIPipeline(state.colorFormat)
                                          : nullptr;
            RHIDescriptorSetLayout* uiTextureSetLayout =
                pipelineCache ? pipelineCache->GetUITextureSetLayout()
                              : nullptr;
            m_stats.runtimeUIPipelineReady =
                uiPipeline != nullptr && uiTextureSetLayout != nullptr;

            if (viewport->RenderRuntimeUIOverlay(*desc.commandContext,
                                                 *desc.runtimeUIRenderer,
                                                 uiPipeline,
                                                 uiTextureSetLayout))
            {
                ++m_stats.runtimeUIOverlayViewportCount;
                m_stats.runtimeUIOverlayDrawCallCount +=
                    viewport->GetRenderTargetState().runtimeUIOverlayDrawCallCount;
            }
        }
    }

    ++m_stats.frameCount;
    return m_stats;
}

} // namespace RVX::Editor
