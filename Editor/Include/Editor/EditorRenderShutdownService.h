/**
 * @file EditorRenderShutdownService.h
 * @brief Editor render resource shutdown and lifetime release service.
 */

#pragma once

#include "Core/Types.h"

#include <memory>
#include <string>

namespace RVX
{
class RenderContext;
class SceneRenderer;

namespace UI
{
class UIRenderer;
} // namespace UI
} // namespace RVX

namespace RVX::Editor
{

struct EditorRenderShutdownDesc
{
    std::unique_ptr<RenderContext>* renderContext = nullptr;
    std::unique_ptr<SceneRenderer>* sceneRenderer = nullptr;
    std::unique_ptr<UI::UIRenderer>* runtimeUIRenderer = nullptr;
    std::unique_ptr<UI::UIRenderer>* editorUIRenderer = nullptr;

    bool waitForIdle = true;
    bool clearSceneExternalRenderTarget = true;
};

struct EditorRenderShutdownResult
{
    bool completed = false;
    bool waitedForIdle = false;
    bool clearedSceneExternalRenderTarget = false;
    bool sceneRendererShutdown = false;
    bool runtimeUIRendererShutdown = false;
    bool editorUIRendererShutdown = false;
    bool renderContextShutdown = false;
    std::string error;

    explicit operator bool() const { return completed; }
};

/**
 * @brief Owns ordered render shutdown steps that are independent of panels.
 */
class EditorRenderShutdownService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    EditorRenderShutdownResult PrepareForPanelShutdown(
        const EditorRenderShutdownDesc& desc) const;
    EditorRenderShutdownResult ShutdownResources(
        const EditorRenderShutdownDesc& desc) const;

private:
    static EditorRenderShutdownResult Fail(std::string error);
    static bool Validate(const EditorRenderShutdownDesc& desc,
                         std::string& error);
};

} // namespace RVX::Editor
