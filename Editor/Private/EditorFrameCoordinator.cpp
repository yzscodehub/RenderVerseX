/**
 * @file EditorFrameCoordinator.cpp
 * @brief Editor frame submission coordinator implementation
 */

#include "Editor/EditorFrameCoordinator.h"
#include "Render/Context/RenderContext.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"

namespace RVX::Editor
{

const EditorFrameCoordinatorStats& EditorFrameCoordinator::SubmitFrame(
    const EditorFrameCoordinatorDesc& desc)
{
    m_stats = {};
    if (!desc.client)
    {
        return m_stats;
    }

    m_stats.renderContextReady =
        desc.renderContext && desc.renderContext->IsInitialized() &&
        desc.renderContext->GetDevice();
    if (!m_stats.renderContextReady)
    {
        m_stats.standaloneFallbackUsed = true;
        desc.client->SubmitViewportTargetsStandalone();
        desc.client->SubmitNativeMainFramebufferStandalone();
        return m_stats;
    }

    m_stats.sharedRHIFrameUsed = true;
    desc.renderContext->BeginFrame();
    m_stats.renderContextFrameBegun = true;

    RHICommandContext* commandContext = desc.renderContext->GetGraphicsContext();
    m_stats.graphicsContextReady = commandContext != nullptr;
    if (commandContext)
    {
        desc.client->SubmitViewportTargets(*commandContext);
        desc.client->SubmitNativeMainFramebuffer(commandContext);
    }
    else
    {
        desc.client->HandleGraphicsContextUnavailable();
    }

    desc.renderContext->EndFrame();
    m_stats.renderContextFrameEnded = true;
    return m_stats;
}

} // namespace RVX::Editor
