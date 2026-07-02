/**
 * @file EditorFrameCoordinator.h
 * @brief Editor frame submission coordinator
 */

#pragma once

#include "Core/Types.h"

namespace RVX
{
    class RenderContext;
    class RHICommandContext;
}

namespace RVX::Editor
{

struct EditorFrameCoordinatorStats
{
    bool renderContextReady = false;
    bool sharedRHIFrameUsed = false;
    bool standaloneFallbackUsed = false;
    bool graphicsContextReady = false;
    bool renderContextFrameBegun = false;
    bool renderContextFrameEnded = false;
};

class IEditorFrameClient
{
public:
    virtual ~IEditorFrameClient() = default;

    virtual void SubmitViewportTargets(RHICommandContext& commandContext) = 0;
    virtual void SubmitNativeMainFramebuffer(RHICommandContext* commandContext) = 0;
    virtual void SubmitViewportTargetsStandalone() = 0;
    virtual void SubmitNativeMainFramebufferStandalone() = 0;
    virtual void HandleGraphicsContextUnavailable() = 0;
};

struct EditorFrameCoordinatorDesc
{
    RenderContext* renderContext = nullptr;
    IEditorFrameClient* client = nullptr;
};

class EditorFrameCoordinator final
{
public:
    // =========================================================================
    // Frame Submission
    // =========================================================================

    const EditorFrameCoordinatorStats& SubmitFrame(
        const EditorFrameCoordinatorDesc& desc);

    // =========================================================================
    // Diagnostics
    // =========================================================================

    const EditorFrameCoordinatorStats& GetStats() const { return m_stats; }

private:
    EditorFrameCoordinatorStats m_stats;
};

} // namespace RVX::Editor
