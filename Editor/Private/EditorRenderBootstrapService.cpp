/**
 * @file EditorRenderBootstrapService.cpp
 * @brief Editor render-context startup dependency assembly service.
 */

#include "Editor/EditorRenderBootstrapService.h"

#include "Core/Log.h"
#include "Editor/EditorMainSwapChainService.h"
#include "Editor/EditorNativeUIRenderStatsService.h"
#include "Render/Context/RenderContext.h"
#include "Render/Renderer/SceneRenderer.h"
#include "RHI/RHIDevice.h"
#include "UI/UIRenderer.h"

#include <memory>
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

RHIBackendType EditorRenderBootstrapService::ResolveDefaultBackend() const
{
#if defined(RVX_ENABLE_OPENGL) && RVX_ENABLE_OPENGL
    return RHIBackendType::OpenGL;
#elif defined(RVX_ENABLE_DX11) && RVX_ENABLE_DX11
    return RHIBackendType::DX11;
#elif defined(RVX_ENABLE_VULKAN) && RVX_ENABLE_VULKAN
    return RHIBackendType::Vulkan;
#elif defined(RVX_ENABLE_DX12) && RVX_ENABLE_DX12
    return RHIBackendType::DX12;
#else
    return RHIBackendType::None;
#endif
}

EditorRenderBootstrapResult
EditorRenderBootstrapService::Bootstrap(
    const EditorRenderBootstrapDesc& desc) const
{
    std::string missingDependency;
    if (!Require(desc.renderContext,
                 "Editor RenderContext storage",
                 missingDependency) ||
        !Require(desc.sceneRenderer,
                 "Editor SceneRenderer storage",
                 missingDependency) ||
        !Require(desc.runtimeUIRenderer,
                 "Editor runtime UI renderer storage",
                 missingDependency) ||
        !Require(desc.editorUIRenderer,
                 "Editor native UI renderer storage",
                 missingDependency) ||
        !Require(desc.mainSwapChainService,
                 "Editor main swap-chain service",
                 missingDependency) ||
        !Require(desc.nativeUIRenderStatsService,
                 "Editor native UI render stats service",
                 missingDependency) ||
        !Require(desc.nativeUIRenderStats,
                 "Editor native UI render stats",
                 missingDependency))
    {
        return Fail(std::move(missingDependency));
    }

    EditorRenderBootstrapResult result;
    if (*desc.renderContext && (*desc.renderContext)->IsInitialized())
    {
        result.initialized = true;
        result.alreadyInitialized = true;
        result.renderContextInitialized = true;
        if (IRHIDevice* device = (*desc.renderContext)->GetDevice())
        {
            result.backendType = device->GetBackendType();
        }
        result.mainSwapChainReady =
            desc.mainSwapChainService->HasSwapChain(desc.renderContext->get());
        result.runtimeUIRendererReady = desc.runtimeUIRenderer->get() != nullptr;
        result.editorUIRendererReady = desc.editorUIRenderer->get() != nullptr;
        result.sceneRendererReady = desc.sceneRenderer->get() != nullptr;
        return result;
    }

    const RHIBackendType backend =
        desc.backendType == RHIBackendType::Auto
            ? ResolveDefaultBackend()
            : desc.backendType;
    result.backendType = backend;
    if (backend == RHIBackendType::None)
    {
        RVX_CORE_WARN("Editor RHI disabled: no compiled backend is available");
        return Fail("Editor RHI backend is unavailable", backend);
    }

    RenderContextConfig config;
    config.backendType = backend;
    config.enableValidation = desc.enableValidation;
    config.enableGPUValidation = desc.enableGPUValidation;
    config.vsync = desc.vsync;
    config.frameBuffering = desc.frameBuffering;
    config.appName = desc.appName ? desc.appName : "RenderVerseX Editor";

    const NativeSurfaceDesc initialSurface =
        desc.mainSwapChainService->CaptureSurface(
            desc.window,
            backend,
            RHIFormat::BGRA8_UNORM,
            desc.vsync);
    *desc.renderContext = std::make_unique<RenderContext>();
    if (!(*desc.renderContext)->Initialize(config, initialSurface))
    {
        RVX_CORE_ERROR("Editor failed to initialize RenderContext for backend {}",
                       ToString(backend));
        desc.renderContext->reset();
        return Fail("Editor RenderContext initialization failed", backend);
    }
    result.renderContextInitialized = true;

    EditorMainSwapChainEnsureDesc swapChainDesc;
    swapChainDesc.window = desc.window;
    swapChainDesc.renderContext = desc.renderContext->get();
    const EditorMainSwapChainEnsureResult swapChainResult =
        desc.mainSwapChainService->Ensure(swapChainDesc);
    desc.nativeUIRenderStatsService->ApplyMainSwapChainResult(
        *desc.nativeUIRenderStats,
        swapChainResult);
    result.mainSwapChainReady = swapChainResult.ready;
    if (!swapChainResult.ready)
    {
        RVX_CORE_WARN("Editor main-window swap chain initialization failed: {}",
                      desc.nativeUIRenderStats->mainFramebufferFallbackReason);
    }

    IRHIDevice* device = (*desc.renderContext)->GetDevice();
    *desc.runtimeUIRenderer = std::make_unique<UI::UIRenderer>();
    if (!(*desc.runtimeUIRenderer)->Initialize(device))
    {
        RVX_CORE_WARN("Editor runtime UI renderer initialization failed; "
                      "viewport overlay will be disabled");
        desc.runtimeUIRenderer->reset();
    }
    result.runtimeUIRendererReady = desc.runtimeUIRenderer->get() != nullptr;

    *desc.editorUIRenderer = std::make_unique<UI::UIRenderer>();
    if (!(*desc.editorUIRenderer)->Initialize(device))
    {
        RVX_CORE_WARN("Editor native UI renderer initialization failed; "
                      "native editor UI submit will be disabled");
        desc.editorUIRenderer->reset();
    }
    result.editorUIRendererReady = desc.editorUIRenderer->get() != nullptr;

    desc.sceneRenderer->reset();
    result.sceneRendererReady = false;
    RVX_CORE_INFO("Editor SceneRenderer publication integration is deferred; "
                  "viewport uses the explicit fallback clear path");

    RVX_CORE_INFO("Editor RenderContext initialized for viewport targets: {}",
                  ToString((*desc.renderContext)
                               ->GetDevice()
                               ->GetBackendType()));
    result.initialized = true;
    return result;
}

EditorRenderBootstrapResult
EditorRenderBootstrapService::Fail(std::string error,
                                   RHIBackendType backendType)
{
    EditorRenderBootstrapResult result;
    result.backendType = backendType;
    result.error = std::move(error);
    return result;
}

} // namespace RVX::Editor
