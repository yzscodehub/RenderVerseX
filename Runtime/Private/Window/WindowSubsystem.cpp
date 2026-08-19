/**
 * @file WindowSubsystem.cpp
 * @brief WindowSubsystem implementation
 */

#include "Runtime/Window/WindowSubsystem.h"
#include "HAL/HAL.h"
#include "Core/Log.h"

namespace RVX
{

void WindowSubsystem::Initialize()
{
    RVX_CORE_INFO("WindowSubsystem initializing...");

    HAL::WindowDesc desc;
    desc.width = m_config.width;
    desc.height = m_config.height;
    desc.title = m_config.title;
    desc.resizable = m_config.resizable;
    desc.fullscreen = m_config.fullscreen;
    desc.vsync = m_config.vsync;
    desc.graphicsApi = m_config.graphicsApi;

    m_window = HAL::CreateWindow(desc);

    if (!m_window)
    {
        RVX_CORE_ERROR("Failed to create window");
        return;
    }

    uint32 w, h;
    m_window->GetFramebufferSize(w, h);
    m_lastWidth = w;
    m_lastHeight = h;
    m_surfaceGeneration = 1;
    
    RVX_CORE_INFO("WindowSubsystem initialized: {}x{}", m_lastWidth, m_lastHeight);
}

void WindowSubsystem::Deinitialize()
{
    RVX_CORE_DEBUG("WindowSubsystem deinitializing...");
    m_window.reset();
    RVX_CORE_INFO("WindowSubsystem deinitialized");
}

void WindowSubsystem::Tick(float deltaTime)
{
    (void)deltaTime;

    if (!m_window)
        return;

    m_window->PollEvents();

    // Check for resize
    uint32 currentWidth, currentHeight;
    m_window->GetFramebufferSize(currentWidth, currentHeight);

    if (currentWidth != m_lastWidth || currentHeight != m_lastHeight)
    {
        m_lastWidth = currentWidth;
        m_lastHeight = currentHeight;
        ++m_surfaceGeneration;

        // Publish resize event
        EventBus::Get().Publish(HAL::WindowResizedEvent(currentWidth, currentHeight));
    }
}

bool WindowSubsystem::ShouldClose() const
{
    return m_window ? m_window->ShouldClose() : true;
}

void WindowSubsystem::GetFramebufferSize(uint32_t& width, uint32_t& height) const
{
    if (m_window)
    {
        uint32 w, h;
        m_window->GetFramebufferSize(w, h);
        width = w;
        height = h;
    }
    else
    {
        width = 0;
        height = 0;
    }
}

bool WindowSubsystem::RequestResize(uint32 width, uint32 height)
{
    if (!m_window)
    {
        RVX_CORE_ERROR("Window resize requested before WindowSubsystem initialization");
        return false;
    }

    if (!m_window->RequestResize(width, height))
    {
        RVX_CORE_ERROR("Native window resize request failed: {}x{}", width, height);
        return false;
    }

    return true;
}

float WindowSubsystem::GetDpiScale() const
{
    return m_window ? m_window->GetDpiScale() : 1.0f;
}

void* WindowSubsystem::GetNativeHandle() const
{
    return m_window ? m_window->GetNativeHandle() : nullptr;
}

void* WindowSubsystem::GetInternalHandle() const
{
    return m_window ? m_window->GetInternalHandle() : nullptr;
}

NativeSurfaceDesc WindowSubsystem::CaptureRenderSurface(
    RHIFormat preferredFormat) const
{
    NativeSurfaceDesc surface;
    if (!m_window)
    {
        return surface;
    }

    const HAL::WindowRenderSurfaceHandles handles =
        m_window->CaptureRenderSurfaceHandles();
#if defined(_WIN32)
    surface.platform = NativeSurfacePlatform::Win32;
#elif defined(__APPLE__)
    surface.platform = NativeSurfacePlatform::Cocoa;
#else
    surface.platform = NativeSurfacePlatform::GLFW;
#endif
    surface.nativeWindow = handles.nativeWindow;
    surface.nativeDisplay = handles.nativeDisplay;
    surface.nativeLayer = handles.nativeLayer;
    surface.backendWindow = handles.backendWindow;
    surface.width = handles.width;
    surface.height = handles.height;
    surface.contentScale = handles.contentScale;
    surface.preferredFormat = preferredFormat;
    surface.vsync = m_config.vsync;
    surface.generation = m_surfaceGeneration;
    return surface;
}

void WindowSubsystem::ReleaseGraphicsContextFromCurrentThread()
{
    if (m_window)
    {
        m_window->ReleaseGraphicsContextFromCurrentThread();
    }
}

} // namespace RVX
