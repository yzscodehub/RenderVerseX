/**
 * @file EditorNativeUISubmissionService.cpp
 * @brief Native editor UI main-framebuffer submission helper implementation
 */

#include "Editor/EditorNativeUISubmissionService.h"
#include "Editor/EditorTheme.h"
#include "Render/Context/RenderContext.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/SceneRenderer.h"
#include "RHI/RHIDevice.h"
#include "UI/UIRenderer.h"

namespace RVX::Editor
{

EditorNativeUISubmissionResult EditorNativeUISubmissionService::SubmitToMainFramebuffer(
    const EditorNativeUISubmissionDesc& desc) const
{
    EditorNativeUISubmissionResult result;
    result.attempted = true;
    result.targetReady =
        desc.renderContext &&
        desc.renderContext->IsInitialized() &&
        desc.renderContext->GetDevice();
    if (!result.targetReady)
    {
        result.fallbackReason = "RenderContext unavailable";
        return result;
    }
    if (!desc.renderer || !desc.renderer->IsInitialized())
    {
        result.fallbackReason = "Editor UI renderer unavailable";
        return result;
    }
    if (!desc.sceneRenderer || !desc.sceneRenderer->IsInitialized())
    {
        result.fallbackReason = "SceneRenderer unavailable";
        return result;
    }

    PipelineCache* pipelineCache = desc.sceneRenderer->GetPipelineCache();
    if (!pipelineCache)
    {
        result.fallbackReason = "PipelineCache unavailable";
        return result;
    }

    RHISwapChain* mainSwapChain = desc.renderContext->GetSwapChain();
    RHITextureView* mainBackBufferView =
        mainSwapChain ? desc.renderContext->GetCurrentBackBufferView() : nullptr;
    const bool useMainSwapChain =
        mainSwapChain != nullptr && mainBackBufferView != nullptr;
    const RHIFormat outputFormat =
        useMainSwapChain ? mainSwapChain->GetFormat() : RHIFormat::BGRA8_UNORM;

    RHIPipeline* uiPipeline = pipelineCache->GetUIPipeline(outputFormat);
    RHIDescriptorSetLayout* uiTextureSetLayout =
        pipelineCache->GetUITextureSetLayout();
    result.pipelineReady = uiPipeline != nullptr && uiTextureSetLayout != nullptr;
    if (!result.pipelineReady)
    {
        result.fallbackReason = "Native editor UI pipeline unavailable";
        return result;
    }
    if (!useMainSwapChain &&
        desc.renderContext->GetDevice()->GetBackendType() != RHIBackendType::OpenGL)
    {
        result.fallbackReason =
            "Native editor UI main framebuffer submit requires a main-window RHI swap chain";
        return result;
    }

    const bool ownsRenderContextFrame = desc.commandContext == nullptr;
    RHICommandContext* commandContext = desc.commandContext;
    if (ownsRenderContextFrame)
    {
        desc.renderContext->BeginFrame();
        commandContext = desc.renderContext->GetGraphicsContext();
    }
    if (!commandContext)
    {
        if (ownsRenderContextFrame)
        {
            desc.renderContext->EndFrame();
        }
        result.fallbackReason =
            "RenderContext graphics command context unavailable";
        return result;
    }

    if (useMainSwapChain)
    {
        UI::UIRenderTargetDesc target;
        target.width = mainSwapChain->GetWidth();
        target.height = mainSwapChain->GetHeight();
        target.format = outputFormat;
        target.colorTarget = mainBackBufferView;
        if (!desc.renderer->BindRecordedFrameTarget(target))
        {
            if (ownsRenderContextFrame)
            {
                desc.renderContext->EndFrame();
            }
            result.fallbackReason =
                "Recorded native editor UI frame does not match the main swap chain target";
            return result;
        }
    }

    UI::UIRenderSubmitDesc submitDesc;
    submitDesc.commandContext = commandContext;
    submitDesc.pipeline = uiPipeline;
    submitDesc.textureSetLayout = uiTextureSetLayout;
    submitDesc.allowDefaultFramebufferTarget = !useMainSwapChain;
    submitDesc.colorLoadOp = useMainSwapChain ? RHILoadOp::Clear : RHILoadOp::Load;
    submitDesc.colorStoreOp = RHIStoreOp::Store;
    const Vec4& backgroundColor = EditorTheme::Get().GetColors().background;
    submitDesc.clearColor = {backgroundColor.r,
                             backgroundColor.g,
                             backgroundColor.b,
                             backgroundColor.a};

    result.submitted = desc.renderer->Submit(submitDesc);
    const UI::UIRenderSubmitStats& submitStats = desc.renderer->GetSubmitStats();
    result.defaultFramebufferTarget = submitStats.defaultFramebufferTarget;
    result.submittedToMainSwapChain = result.submitted && useMainSwapChain;
    result.submittedInEditorRHIFrame =
        result.submitted && !ownsRenderContextFrame;
    result.drawCallCount = submitStats.drawCallCount;
    if (!result.submitted)
    {
        result.fallbackReason = "UIRenderer submit returned false";
    }

    if (ownsRenderContextFrame)
    {
        desc.renderContext->EndFrame();
    }
    return result;
}

} // namespace RVX::Editor
