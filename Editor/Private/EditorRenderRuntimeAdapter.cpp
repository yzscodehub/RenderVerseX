/**
 * @file EditorRenderRuntimeAdapter.cpp
 * @brief Value-only Editor adapter for the dedicated render runtime.
 */

#include "Editor/EditorRenderRuntimeAdapter.h"

#include "Core/Log.h"
#include "Render/RenderSubsystem.h"

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

#include <algorithm>
#include <utility>

namespace RVX::Editor
{

EditorRenderRuntimeAdapter::EditorRenderRuntimeAdapter() = default;

EditorRenderRuntimeAdapter::~EditorRenderRuntimeAdapter()
{
    static_cast<void>(Shutdown());
}

RHIBackendType
EditorRenderRuntimeAdapter::ResolveDefaultBackend() const noexcept
{
#if defined(RVX_ENABLE_DX12) && RVX_ENABLE_DX12
    return RHIBackendType::DX12;
#elif defined(RVX_ENABLE_VULKAN) && RVX_ENABLE_VULKAN
    return RHIBackendType::Vulkan;
#elif defined(RVX_ENABLE_METAL) && RVX_ENABLE_METAL
    return RHIBackendType::Metal;
#elif defined(RVX_ENABLE_OPENGL) && RVX_ENABLE_OPENGL
    return RHIBackendType::OpenGL;
#elif defined(RVX_ENABLE_DX11) && RVX_ENABLE_DX11
    return RHIBackendType::DX11;
#else
    return RHIBackendType::None;
#endif
}

EditorRenderRuntimeAdapterStartResult EditorRenderRuntimeAdapter::Start(
    GLFWwindow* window,
    const EditorRenderRuntimeAdapterConfig& config)
{
    EditorRenderRuntimeAdapterStartResult result;
    if (IsRunning())
    {
        result.started = true;
        result.backend = m_lastRuntime.backend;
        result.surface = m_surface;
        result.runtime = m_lastRuntime;
        return result;
    }

    const RHIBackendType backend =
        config.backendType == RHIBackendType::Auto
            ? ResolveDefaultBackend()
            : config.backendType;
    result.backend = backend;
    if (backend == RHIBackendType::None || window == nullptr)
    {
        result.runtime.code = RenderRuntimeCode::InvalidConfiguration;
        result.runtime.resultClass = RenderResultClass::RuntimeFatal;
        result.runtime.message =
            "Editor render adapter requires a window and compiled backend";
        m_lastRuntime = result.runtime;
        return result;
    }

    m_surface = CaptureSurface(window, backend, config.vsync, 1);
    result.surface = m_surface;
    if (!m_surface.IsValidFor(backend))
    {
        result.runtime.code = RenderRuntimeCode::InvalidSurface;
        result.runtime.resultClass = RenderResultClass::RuntimeFatal;
        result.runtime.backend = backend;
        result.runtime.message = "Editor native render surface is invalid";
        m_lastRuntime = result.runtime;
        return result;
    }

    if (backend == RHIBackendType::OpenGL)
    {
        glfwMakeContextCurrent(nullptr);
    }

    RenderRuntimeConfig runtimeConfig;
    runtimeConfig.backendType = backend;
    runtimeConfig.enableValidation = config.enableValidation;
    runtimeConfig.enableGPUValidation = config.enableGPUValidation;
    runtimeConfig.frameBuffering = config.frameBuffering;

    m_render = std::make_unique<RenderSubsystem>();
    try
    {
        m_render->Configure(runtimeConfig, m_surface);
        m_render->Initialize();
    }
    catch (const RenderSubsystemInitializationError& error)
    {
        m_lastRuntime = error.GetResult();
        result.runtime = m_lastRuntime;
        m_render->Deinitialize();
        m_render.reset();
        return result;
    }

    m_lastRuntime = m_render->GetLastRuntimeResult();
    result.runtime = m_lastRuntime;
    result.backend = m_lastRuntime.backend;
    result.started = m_lastRuntime.code == RenderRuntimeCode::Running;
    if (!result.started)
    {
        m_render->Deinitialize();
        m_render.reset();
    }
    return result;
}

RenderFramePublishResult EditorRenderRuntimeAdapter::PublishFrame(
    std::unique_ptr<const RenderFramePacket> packet)
{
    if (!m_render)
    {
        RenderFramePublishResult result;
        result.code = RenderFramePublishCode::NotRunning;
        result.resultClass =
            ClassifyRenderFramePublishCode(result.code);
        return result;
    }
    return m_render->TryPublishFrame(std::move(packet));
}

RenderResizeResult EditorRenderRuntimeAdapter::RequestResize(uint32 width,
                                                             uint32 height)
{
    if (!m_render)
    {
        RenderResizeResult result;
        result.code = RenderResizeCode::NotRunning;
        result.resultClass = ClassifyRenderResizeCode(result.code);
        return result;
    }

    NativeSurfaceDesc surface = m_surface;
    surface.width = width;
    surface.height = height;
    ++surface.generation;
    const RenderResizeResult result = m_render->RequestResize(surface);
    if (result.code == RenderResizeCode::Accepted ||
        result.code == RenderResizeCode::CoalescedOlder)
    {
        m_surface = surface;
    }
    return result;
}

RenderDiagnosticsSnapshot
EditorRenderRuntimeAdapter::GetDiagnostics() const
{
    return m_render ? m_render->GetDiagnosticsSnapshot()
                    : RenderDiagnosticsSnapshot{};
}

RenderShutdownResult EditorRenderRuntimeAdapter::Shutdown()
{
    if (!m_render)
    {
        if (m_lastShutdown.code == RenderShutdownCode::None)
        {
            m_lastShutdown.code = RenderShutdownCode::AlreadyStopped;
            m_lastShutdown.lifecycle = RenderLifecycleState::Stopped;
        }
        return m_lastShutdown;
    }

    m_render->Deinitialize();
    m_lastShutdown = m_render->GetLastShutdownResult();
    m_render.reset();
    return m_lastShutdown;
}

bool EditorRenderRuntimeAdapter::IsRunning() const noexcept
{
    return m_render != nullptr &&
           m_lastRuntime.code == RenderRuntimeCode::Running;
}

EditorRenderFeatureStatus EditorRenderRuntimeAdapter::GetFeatureStatus(
    EditorRenderFeature feature) const noexcept
{
    switch (feature)
    {
        case EditorRenderFeature::FramePublication:
        case EditorRenderFeature::ValueDiagnostics:
            return EditorRenderFeatureStatus::Available;
        case EditorRenderFeature::ViewportRendering:
        case EditorRenderFeature::NativeUISubmission:
        case EditorRenderFeature::ScreenshotService:
            return EditorRenderFeatureStatus::
                UnavailableDuringM1ArchitectureCut;
    }
    return EditorRenderFeatureStatus::UnavailableDuringM1ArchitectureCut;
}

NativeSurfaceDesc EditorRenderRuntimeAdapter::CaptureSurface(
    GLFWwindow* window,
    RHIBackendType backend,
    bool vsync,
    uint64 generation)
{
    NativeSurfaceDesc surface;
    if (!window)
    {
        return surface;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    float xScale = 1.0f;
    float yScale = 1.0f;
    glfwGetWindowContentScale(window, &xScale, &yScale);

#ifdef _WIN32
    surface.platform = NativeSurfacePlatform::Win32;
    surface.nativeWindow =
        reinterpret_cast<uintptr_t>(glfwGetWin32Window(window));
#elif defined(__APPLE__)
    surface.platform = NativeSurfacePlatform::Cocoa;
    surface.nativeWindow =
        reinterpret_cast<uintptr_t>(glfwGetCocoaWindow(window));
#else
    surface.platform = NativeSurfacePlatform::GLFW;
#endif
    surface.backendWindow = reinterpret_cast<uintptr_t>(window);
    surface.width = framebufferWidth > 0
                        ? static_cast<uint32>(framebufferWidth)
                        : 0;
    surface.height = framebufferHeight > 0
                         ? static_cast<uint32>(framebufferHeight)
                         : 0;
    surface.contentScale = std::max(xScale, yScale);
    surface.preferredFormat = RHIFormat::BGRA8_UNORM;
    surface.vsync = vsync;
    surface.generation = generation;
    if (!surface.IsValidFor(backend))
    {
        return {};
    }
    return surface;
}

} // namespace RVX::Editor
