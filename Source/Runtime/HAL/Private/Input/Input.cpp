#include "HAL/Input/Input.h"

namespace RVX::HAL
{
    void Input::ClearFrameState()
    {
        m_state.mouseDeltaX = 0.0f;
        m_state.mouseDeltaY = 0.0f;
        m_state.mouseWheel = 0.0f;
        m_events.clear();
    }

    void Input::OnEvent(const InputEvent& event)
    {
        m_events.push_back(event);

        switch (event.type)
        {
        case InputEventType::KeyDown:
        case InputEventType::KeyUp:
        {
            const auto& keyEvent = static_cast<const KeyEvent&>(event);
            if (keyEvent.key < MAX_KEYS)
                m_state.keys[keyEvent.key] = (event.type == InputEventType::KeyDown);
            break;
        }
        case InputEventType::MouseMove:
        {
            const auto& moveEvent = static_cast<const MouseMoveEvent&>(event);
            m_state.mouseDeltaX += (moveEvent.x - m_state.mouseX);
            m_state.mouseDeltaY += (moveEvent.y - m_state.mouseY);
            m_state.mouseX = moveEvent.x;
            m_state.mouseY = moveEvent.y;
            break;
        }
        case InputEventType::MouseButtonDown:
        case InputEventType::MouseButtonUp:
        {
            const auto& mouseEvent = static_cast<const MouseButtonEvent&>(event);
            if (mouseEvent.button < MAX_MOUSE_BUTTONS)
                m_state.mouseButtons[mouseEvent.button] = (event.type == InputEventType::MouseButtonDown);
            break;
        }
        case InputEventType::MouseWheel:
        {
            const auto& wheelEvent = static_cast<const MouseWheelEvent&>(event);
            m_state.mouseWheel += wheelEvent.delta;
            break;
        }
        default:
            break;
        }
    }

    std::vector<InputEvent> Input::ConsumeEvents()
    {
        auto events = m_events;
        m_events.clear();
        return events;
    }

} // namespace RVX::HAL
