/**
 * @file InputSubsystem.cpp
 * @brief InputSubsystem implementation
 */

#include "Runtime/Input/InputSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "Platform/InputBackend_GLFW.h"
#include "Core/Log.h"

namespace RVX
{

void InputSubsystem::Initialize()
{
    RVX_CORE_INFO("InputSubsystem initializing...");

    // Get window from WindowSubsystem if available
    if (auto* engine = GetEngine())
    {
        // Note: This will be connected when Engine is refactored
        // For now, window must be set manually via SetWindow()
    }

    RVX_CORE_INFO("InputSubsystem initialized");
}

void InputSubsystem::Deinitialize()
{
    RVX_CORE_DEBUG("InputSubsystem deinitializing...");
    m_backend.reset();
    m_window = nullptr;
    RVX_CORE_INFO("InputSubsystem deinitialized");
}

void InputSubsystem::SetWindow(Window* window)
{
    m_window = window;
    
    if (window)
    {
        // Create GLFW backend (platform-specific)
        m_backend = std::make_unique<GlfwInputBackend>(
            static_cast<GLFWwindow*>(window->GetNativeHandle())
        );
        
        // Initialize mouse position
        const auto& state = m_input.GetState();
        m_lastMouseX = state.mouseX;
        m_lastMouseY = state.mouseY;
    }
}

void InputSubsystem::Tick(float deltaTime)
{
    (void)deltaTime;

    // Store previous state for press/release detection
    auto prevKeys = m_input.GetState().keys;
    auto prevMouseButtons = m_input.GetState().mouseButtons;

    // Clear per-frame state
    m_input.ClearFrameState();

    // Poll backend - pass state to update
    if (m_backend)
    {
        m_backend->Poll(m_input.GetMutableState());
    }

    // Update key press/release states
    const auto& currentState = m_input.GetState();
    for (size_t i = 0; i < m_keysPressed.size(); ++i)
    {
        m_keysPressed[i] = currentState.keys[i] && !prevKeys[i];
        m_keysReleased[i] = !currentState.keys[i] && prevKeys[i];
    }
    
    for (size_t i = 0; i < m_mouseButtonsPressed.size(); ++i)
    {
        m_mouseButtonsPressed[i] = currentState.mouseButtons[i] && !prevMouseButtons[i];
        m_mouseButtonsReleased[i] = !currentState.mouseButtons[i] && prevMouseButtons[i];
    }

    // Update mouse delta
    m_lastMouseX = currentState.mouseX;
    m_lastMouseY = currentState.mouseY;
}

bool InputSubsystem::IsKeyDown(int keyCode) const
{
    if (keyCode < 0 || keyCode >= static_cast<int>(RVX_MAX_KEYS))
        return false;
    return m_input.GetState().keys[keyCode];
}

bool InputSubsystem::IsKeyPressed(int keyCode) const
{
    if (keyCode < 0 || keyCode >= static_cast<int>(RVX_MAX_KEYS))
        return false;
    return m_keysPressed[keyCode];
}

bool InputSubsystem::IsKeyReleased(int keyCode) const
{
    if (keyCode < 0 || keyCode >= static_cast<int>(RVX_MAX_KEYS))
        return false;
    return m_keysReleased[keyCode];
}

bool InputSubsystem::IsMouseButtonDown(int button) const
{
    if (button < 0 || button >= static_cast<int>(RVX_MAX_MOUSE_BUTTONS))
        return false;
    return m_input.GetState().mouseButtons[button];
}

bool InputSubsystem::IsMouseButtonPressed(int button) const
{
    if (button < 0 || button >= static_cast<int>(RVX_MAX_MOUSE_BUTTONS))
        return false;
    return m_mouseButtonsPressed[button];
}

bool InputSubsystem::IsMouseButtonReleased(int button) const
{
    if (button < 0 || button >= static_cast<int>(RVX_MAX_MOUSE_BUTTONS))
        return false;
    return m_mouseButtonsReleased[button];
}

void InputSubsystem::GetMousePosition(float& x, float& y) const
{
    const auto& state = m_input.GetState();
    x = state.mouseX;
    y = state.mouseY;
}

void InputSubsystem::GetMouseDelta(float& dx, float& dy) const
{
    const auto& state = m_input.GetState();
    dx = state.mouseDeltaX;
    dy = state.mouseDeltaY;
}

void InputSubsystem::GetScrollDelta(float& x, float& y) const
{
    const auto& state = m_input.GetState();
    // mouseWheel is the Y scroll delta
    x = 0.0f;
    y = state.mouseWheel;
}

} // namespace RVX
