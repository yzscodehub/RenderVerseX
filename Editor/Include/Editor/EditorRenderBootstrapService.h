/**
 * @file EditorRenderBootstrapService.h
 * @brief Editor render-context startup dependency assembly service.
 */

#pragma once

#include "Core/Types.h"
#include "RHI/RHIDefinitions.h"

#include <memory>
#include <string>

struct GLFWwindow;

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

class EditorMainSwapChainService;
class EditorNativeUIRenderStatsService;
struct EditorNativeUIRenderStats;

struct EditorRenderBootstrapDesc
{
    GLFWwindow* window = nullptr;
    std::unique_ptr<RenderContext>* renderContext = nullptr;
    std::unique_ptr<SceneRenderer>* sceneRenderer = nullptr;
    std::unique_ptr<UI::UIRenderer>* runtimeUIRenderer = nullptr;
    std::unique_ptr<UI::UIRenderer>* editorUIRenderer = nullptr;
    EditorMainSwapChainService* mainSwapChainService = nullptr;
    EditorNativeUIRenderStatsService* nativeUIRenderStatsService = nullptr;
    EditorNativeUIRenderStats* nativeUIRenderStats = nullptr;

    RHIBackendType backendType = RHIBackendType::Auto;
    bool enableValidation = true;
    bool enableGPUValidation = false;
    bool vsync = true;
    uint32 frameBuffering = 2;
    const char* appName = "RenderVerseX Editor";
};

struct EditorRenderBootstrapResult
{
    bool initialized = false;
    bool alreadyInitialized = false;
    bool renderContextInitialized = false;
    bool mainSwapChainReady = false;
    bool runtimeUIRendererReady = false;
    bool editorUIRendererReady = false;
    bool sceneRendererReady = false;
    RHIBackendType backendType = RHIBackendType::None;
    std::string error;

    explicit operator bool() const { return initialized; }
};

/**
 * @brief Builds editor render runtime dependencies for viewport/native UI use.
 */
class EditorRenderBootstrapService
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    RHIBackendType ResolveDefaultBackend() const;
    EditorRenderBootstrapResult Bootstrap(
        const EditorRenderBootstrapDesc& desc) const;

private:
    static EditorRenderBootstrapResult Fail(std::string error,
                                            RHIBackendType backendType =
                                                RHIBackendType::None);
};

} // namespace RVX::Editor
