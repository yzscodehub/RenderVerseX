/**
 * @file EditorNativeUISubmissionService.h
 * @brief Native editor UI main-framebuffer submission helper
 */

#pragma once

#include "Core/Types.h"

#include <string>

namespace RVX
{
    class RHICommandContext;
    class RenderContext;
    class SceneRenderer;

    namespace UI
    {
        class UIRenderer;
    }
}

namespace RVX::Editor
{

struct EditorNativeUISubmissionDesc
{
    RenderContext* renderContext = nullptr;
    SceneRenderer* sceneRenderer = nullptr;
    UI::UIRenderer* renderer = nullptr;
    RHICommandContext* commandContext = nullptr;
};

struct EditorNativeUISubmissionResult
{
    bool attempted = false;
    bool targetReady = false;
    bool pipelineReady = false;
    bool submitted = false;
    bool defaultFramebufferTarget = false;
    bool submittedToMainSwapChain = false;
    bool submittedInEditorRHIFrame = false;
    uint32 drawCallCount = 0;
    std::string fallbackReason;
};

class EditorNativeUISubmissionService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    EditorNativeUISubmissionResult SubmitToMainFramebuffer(
        const EditorNativeUISubmissionDesc& desc) const;
};

} // namespace RVX::Editor
