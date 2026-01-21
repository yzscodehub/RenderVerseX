/**
 * @file WindowSubsystem.cpp
 * @brief WindowSubsystem implementation
 */

#include "Runtime/Window/WindowSubsystem.h"
#include "Core/Log.h"

namespace RVX
{

void WindowSubsystem::Initialize()
{
    RVX_CORE_INFO("WindowSubsystem initializing...");

    WindowDesc desc;
    desc.width = m_config.width;
    desc.height = m_config.height;
    desc.title = m_config.title;
    desc.resizable = m_config.resizable;
    desc.fullscreen = m_config.fullscreen;

    m_window = CreateWindow(desc);

    if (!m_window)
    {
        RVX_CORE_ERROR("Failed to create window");
        return;
    }

    m_window->GetFramebufferSize(m_lastWidth, m_lastHeight);
    
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
    uint32_t currentWidth, currentHeight;
    m_window->GetFramebufferSize(currentWidth, currentHeight);

    if (currentWidth != m_lastWidth || currentHeight != m_lastHeight)
    {
        m_lastWidth = currentWidth;
        m_lastHeight = currentHeight;

        // Publish resize event
        EventBus::Get().Publish(WindowResizedEvent(currentWidth, currentHeight));
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

float WindowSubsystem::GetDpiScale() const
{
    return m_window ? m_window->GetDpiScale() : 1.0f;
}

void* WindowSubsystem::GetNativeHandle() const
{
    return m_window ? m_window->GetNativeHandle() : nullptr;
}

} // namespace RVX
