/**
 * @file EditorFrameSubmissionService.cpp
 * @brief Editor frame submission client service implementation
 */

#include "Editor/EditorFrameSubmissionService.h"
#include "Render/Context/RenderContext.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"

namespace RVX::Editor
{

void EditorFrameSubmissionService::SetFrameDesc(
    const EditorFrameSubmissionDesc& desc)
{
    m_desc = desc;
}

void EditorFrameSubmissionService::SubmitViewportTargets(
    RHICommandContext& commandContext)
{
    SubmitViewportTargets(commandContext, true);
}

void EditorFrameSubmissionService::SubmitNativeMainFramebuffer(
    RHICommandContext* commandContext)
{
    SubmitNativeMainFramebufferInternal(commandContext);
}

void EditorFrameSubmissionService::SubmitViewportTargetsStandalone()
{
    const bool renderContextReady =
        m_desc.renderContext && m_desc.renderContext->IsInitialized() &&
        m_desc.renderContext->GetDevice();
    if (!renderContextReady)
    {
        RecordViewportUnavailable("RenderContext unavailable");
        return;
    }

    m_desc.renderContext->BeginFrame();
    RHICommandContext* commandContext = m_desc.renderContext->GetGraphicsContext();
    if (!commandContext)
    {
        RecordViewportUnavailable(
            "RenderContext graphics command context unavailable");
        m_desc.renderContext->EndFrame();
        if (!m_desc.hasMainWindowSwapChain)
        {
            m_desc.renderContext->Present();
        }
        return;
    }

    SubmitViewportTargets(*commandContext, false);
    m_desc.renderContext->EndFrame();
    if (!m_desc.hasMainWindowSwapChain)
    {
        m_desc.renderContext->Present();
    }
}

void EditorFrameSubmissionService::SubmitNativeMainFramebufferStandalone()
{
    SubmitNativeMainFramebufferInternal(nullptr);
}

void EditorFrameSubmissionService::HandleGraphicsContextUnavailable()
{
    RecordViewportUnavailable(
        "RenderContext graphics command context unavailable");
    if (m_desc.nativeUIRenderStatsService && m_desc.nativeUIStats)
    {
        m_desc.nativeUIRenderStatsService->MarkGraphicsContextUnavailable(
            *m_desc.nativeUIStats,
            "RenderContext graphics command context unavailable");
    }
}

void EditorFrameSubmissionService::SubmitViewportTargets(
    RHICommandContext& commandContext,
    bool submittedInEditorRHIFrame)
{
    if (!m_desc.viewportStats)
    {
        return;
    }

    if (!m_desc.viewportRenderService)
    {
        *m_desc.viewportStats = {};
        m_desc.viewportStats->fallbackReason =
            "EditorViewportRenderService unavailable";
        return;
    }

    EditorViewportRenderDesc renderDesc;
    renderDesc.renderContext = m_desc.renderContext;
    renderDesc.commandContext = &commandContext;
    renderDesc.sceneRenderer = m_desc.sceneRenderer;
    renderDesc.runtimeUIRenderer = m_desc.runtimeUIRenderer;
    renderDesc.sceneManager = m_desc.sceneManager;
    renderDesc.editorUIBackend = m_desc.editorUIBackend;
    renderDesc.submittedInEditorRHIFrame = submittedInEditorRHIFrame;
    renderDesc.legacyPanels = m_desc.legacyPanels;
    renderDesc.includeLegacyPanels = m_desc.includeLegacyPanels;
    *m_desc.viewportStats = m_desc.viewportRenderService->Submit(renderDesc);
}

void EditorFrameSubmissionService::SubmitNativeMainFramebufferInternal(
    RHICommandContext* commandContext)
{
    if (!m_desc.nativeUIStats || !m_desc.nativeUIStats->frameRecorded)
    {
        return;
    }

    if (!m_desc.nativeUISubmissionService)
    {
        if (m_desc.nativeUIRenderStatsService)
        {
            EditorNativeUISubmissionResult result;
            result.attempted = true;
            result.fallbackReason =
                "EditorNativeUISubmissionService unavailable";
            m_desc.nativeUIRenderStatsService->ApplySubmissionResult(
                *m_desc.nativeUIStats,
                result);
        }
        return;
    }

    EditorNativeUISubmissionDesc submitDesc;
    submitDesc.renderContext = m_desc.renderContext;
    submitDesc.sceneRenderer = m_desc.sceneRenderer;
    submitDesc.renderer = m_desc.editorUIRenderer;
    submitDesc.commandContext = commandContext;
    const EditorNativeUISubmissionResult result =
        m_desc.nativeUISubmissionService->SubmitToMainFramebuffer(submitDesc);
    if (m_desc.nativeUIRenderStatsService)
    {
        m_desc.nativeUIRenderStatsService->ApplySubmissionResult(
            *m_desc.nativeUIStats,
            result);
    }
}

void EditorFrameSubmissionService::RecordViewportUnavailable(
    const char* reason)
{
    if (!m_desc.viewportStats)
    {
        return;
    }

    *m_desc.viewportStats = {};
    m_desc.viewportStats->renderContextReady =
        m_desc.renderContext && m_desc.renderContext->IsInitialized() &&
        m_desc.renderContext->GetDevice();
    m_desc.viewportStats->fallbackReason = reason;
}

} // namespace RVX::Editor
