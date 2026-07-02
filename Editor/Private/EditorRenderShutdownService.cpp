/**
 * @file EditorRenderShutdownService.cpp
 * @brief Editor render resource shutdown and lifetime release service.
 */

#include "Editor/EditorRenderShutdownService.h"

#include "Render/Context/RenderContext.h"
#include "Render/Renderer/SceneRenderer.h"
#include "UI/UIRenderer.h"

#include <utility>

namespace RVX::Editor
{
namespace
{
    template <typename T>
    bool Require(T* value, const char* name, std::string& error)
    {
        if (value)
        {
            return true;
        }

        error = name;
        error += " is unavailable";
        return false;
    }
} // namespace

EditorRenderShutdownResult
EditorRenderShutdownService::PrepareForPanelShutdown(
    const EditorRenderShutdownDesc& desc) const
{
    std::string missingDependency;
    if (!Validate(desc, missingDependency))
    {
        return Fail(std::move(missingDependency));
    }

    EditorRenderShutdownResult result;
    if (desc.waitForIdle && *desc.renderContext)
    {
        (*desc.renderContext)->WaitIdle();
        result.waitedForIdle = true;
    }
    if (desc.clearSceneExternalRenderTarget && *desc.sceneRenderer)
    {
        (*desc.sceneRenderer)->ClearExternalRenderTarget();
        result.clearedSceneExternalRenderTarget = true;
    }

    result.completed = true;
    return result;
}

EditorRenderShutdownResult
EditorRenderShutdownService::ShutdownResources(
    const EditorRenderShutdownDesc& desc) const
{
    std::string missingDependency;
    if (!Validate(desc, missingDependency))
    {
        return Fail(std::move(missingDependency));
    }

    EditorRenderShutdownResult result;
    if (*desc.sceneRenderer)
    {
        (*desc.sceneRenderer)->Shutdown();
        desc.sceneRenderer->reset();
        result.sceneRendererShutdown = true;
    }
    if (*desc.runtimeUIRenderer)
    {
        (*desc.runtimeUIRenderer)->Shutdown();
        desc.runtimeUIRenderer->reset();
        result.runtimeUIRendererShutdown = true;
    }
    if (*desc.editorUIRenderer)
    {
        (*desc.editorUIRenderer)->Shutdown();
        desc.editorUIRenderer->reset();
        result.editorUIRendererShutdown = true;
    }
    if (*desc.renderContext)
    {
        (*desc.renderContext)->Shutdown();
        desc.renderContext->reset();
        result.renderContextShutdown = true;
    }

    result.completed = true;
    return result;
}

EditorRenderShutdownResult
EditorRenderShutdownService::Fail(std::string error)
{
    EditorRenderShutdownResult result;
    result.error = std::move(error);
    return result;
}

bool EditorRenderShutdownService::Validate(
    const EditorRenderShutdownDesc& desc,
    std::string& error)
{
    return Require(desc.renderContext,
                   "Editor RenderContext storage",
                   error) &&
           Require(desc.sceneRenderer,
                   "Editor SceneRenderer storage",
                   error) &&
           Require(desc.runtimeUIRenderer,
                   "Editor runtime UI renderer storage",
                   error) &&
           Require(desc.editorUIRenderer,
                   "Editor native UI renderer storage",
                   error);
}

} // namespace RVX::Editor
