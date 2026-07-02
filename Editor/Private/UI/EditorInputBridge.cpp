/**
 * @file EditorInputBridge.cpp
 * @brief Native editor input snapshot bridge implementation
 */

#include "Editor/UI/EditorInputBridge.h"

#include "UI/UIClipboard.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace RVX::Editor
{
namespace
{
    struct CallbackChain
    {
        EditorInputBridge* bridge = nullptr;
        GLFWmousebuttonfun previousMouseButton = nullptr;
        GLFWscrollfun previousScroll = nullptr;
        GLFWcharfun previousChar = nullptr;
        bool installedClipboardProvider = false;
    };

    std::unordered_map<GLFWwindow*, CallbackChain> s_callbackChains;

    uint32 BuildModifierMask(GLFWwindow* window)
    {
        if (!window)
        {
            return 0u;
        }

        uint32 modifiers = 0u;
        const auto keyDown = [window](int key) {
            return glfwGetKey(window, key) == GLFW_PRESS;
        };

        if (keyDown(GLFW_KEY_LEFT_CONTROL) || keyDown(GLFW_KEY_RIGHT_CONTROL))
        {
            modifiers |= UI::ToMask(UI::UIInputModifier::Ctrl);
        }
        if (keyDown(GLFW_KEY_LEFT_SHIFT) || keyDown(GLFW_KEY_RIGHT_SHIFT))
        {
            modifiers |= UI::ToMask(UI::UIInputModifier::Shift);
        }
        if (keyDown(GLFW_KEY_LEFT_ALT) || keyDown(GLFW_KEY_RIGHT_ALT))
        {
            modifiers |= UI::ToMask(UI::UIInputModifier::Alt);
        }
        if (keyDown(GLFW_KEY_LEFT_SUPER) || keyDown(GLFW_KEY_RIGHT_SUPER))
        {
            modifiers |= UI::ToMask(UI::UIInputModifier::Super);
        }
        return modifiers;
    }

    int MapGLFWMouseButtonToUIIndex(int button)
    {
        switch (button)
        {
            case GLFW_MOUSE_BUTTON_LEFT:
                return static_cast<int>(UI::UIMouseButton::Left);
            case GLFW_MOUSE_BUTTON_RIGHT:
                return static_cast<int>(UI::UIMouseButton::Right);
            case GLFW_MOUSE_BUTTON_MIDDLE:
                return static_cast<int>(UI::UIMouseButton::Middle);
            case GLFW_MOUSE_BUTTON_4:
                return static_cast<int>(UI::UIMouseButton::X1);
            case GLFW_MOUSE_BUTTON_5:
                return static_cast<int>(UI::UIMouseButton::X2);
            default:
                return -1;
        }
    }

    void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
    {
        const auto it = s_callbackChains.find(window);
        if (it == s_callbackChains.end())
        {
            return;
        }

        if (it->second.bridge &&
            (action == GLFW_PRESS || action == GLFW_RELEASE))
        {
            it->second.bridge->QueueMouseButton(button, action == GLFW_PRESS);
        }
        if (it->second.previousMouseButton)
        {
            it->second.previousMouseButton(window, button, action, mods);
        }
    }

    void ScrollCallback(GLFWwindow* window, double xOffset, double yOffset)
    {
        const auto it = s_callbackChains.find(window);
        if (it == s_callbackChains.end())
        {
            return;
        }

        if (it->second.bridge)
        {
            it->second.bridge->QueueScroll(xOffset, yOffset);
        }
        if (it->second.previousScroll)
        {
            it->second.previousScroll(window, xOffset, yOffset);
        }
    }

    void CharCallback(GLFWwindow* window, unsigned int codepoint)
    {
        const auto it = s_callbackChains.find(window);
        if (it == s_callbackChains.end())
        {
            return;
        }

        if (it->second.bridge)
        {
            it->second.bridge->QueueCharacter(static_cast<uint32>(codepoint));
        }
        if (it->second.previousChar)
        {
            it->second.previousChar(window, codepoint);
        }
    }
}

bool EditorInputBridge::Attach(GLFWwindow* window)
{
    if (!window)
    {
        return false;
    }

    if (m_window == window)
    {
        return true;
    }
    if (m_window)
    {
        Detach();
    }

    CallbackChain chain;
    chain.bridge = this;
    chain.previousMouseButton =
        glfwSetMouseButtonCallback(window, MouseButtonCallback);
    chain.previousScroll = glfwSetScrollCallback(window, ScrollCallback);
    chain.previousChar = glfwSetCharCallback(window, CharCallback);
    s_callbackChains[window] = chain;

    m_window = window;
    UI::UIClipboardProvider clipboardProvider;
    clipboardProvider.getText = [window](std::string& text) {
        const char* clipboardText = glfwGetClipboardString(window);
        if (!clipboardText)
        {
            text.clear();
            return false;
        }

        text = clipboardText;
        return true;
    };
    clipboardProvider.setText = [window](const std::string& text) {
        glfwSetClipboardString(window, text.c_str());
        return true;
    };
    UI::UIClipboard::SetProvider(std::move(clipboardProvider));
    s_callbackChains[window].installedClipboardProvider = true;

    m_pendingScrollDelta = Vec2(0.0f);
    m_pendingMouseButtonPresses.fill(false);
    m_pendingMouseButtonReleases.fill(false);
    m_pendingTextInput.clear();
    m_lastBuildStats = {};
    m_lastBuildStats.attached = true;
    return true;
}

void EditorInputBridge::Detach()
{
    if (!m_window)
    {
        return;
    }

    const auto it = s_callbackChains.find(m_window);
    if (it != s_callbackChains.end())
    {
        if (it->second.installedClipboardProvider)
        {
            UI::UIClipboard::ResetProvider();
        }
        glfwSetMouseButtonCallback(m_window, it->second.previousMouseButton);
        glfwSetScrollCallback(m_window, it->second.previousScroll);
        glfwSetCharCallback(m_window, it->second.previousChar);
        s_callbackChains.erase(it);
    }

    m_window = nullptr;
    m_pendingScrollDelta = Vec2(0.0f);
    m_pendingMouseButtonPresses.fill(false);
    m_pendingMouseButtonReleases.fill(false);
    m_pendingTextInput.clear();
    m_lastBuildStats = {};
}

UI::UIInputSnapshot EditorInputBridge::BuildSnapshot(
    const EditorInputBridgeCaptureState& captureState)
{
    UI::UIInputSnapshot input;
    PollWindowState(input);
    uint32 mouseButtonEdgeCount = 0;
    for (size_t index = 0; index < UI::RVX_UI_MOUSE_BUTTON_COUNT; ++index)
    {
        if (m_pendingMouseButtonPresses[index])
        {
            ++mouseButtonEdgeCount;
        }
        if (m_pendingMouseButtonReleases[index])
        {
            ++mouseButtonEdgeCount;
        }
        if (m_pendingMouseButtonPresses[index])
        {
            input.mouseButtonsDown[index] = true;
        }
        else if (m_pendingMouseButtonReleases[index])
        {
            input.mouseButtonsDown[index] = false;
        }
    }
    input.scrollDelta = m_pendingScrollDelta;
    input.textInput = std::move(m_pendingTextInput);
    input.wantsMouseCapture = captureState.wantsMouseCapture;
    input.wantsKeyboardCapture = captureState.wantsKeyboardCapture;

    m_lastBuildStats.attached = m_window != nullptr;
    m_lastBuildStats.textByteCount = static_cast<uint32>(input.textInput.size());
    m_lastBuildStats.mouseButtonEdgeCount = mouseButtonEdgeCount;
    m_lastBuildStats.scrollDelta = input.scrollDelta;

    m_pendingScrollDelta = Vec2(0.0f);
    m_pendingMouseButtonPresses.fill(false);
    m_pendingMouseButtonReleases.fill(false);
    m_pendingTextInput.clear();
    return input;
}

void EditorInputBridge::QueueScroll(double xOffset, double yOffset)
{
    m_pendingScrollDelta.x += static_cast<float>(xOffset);
    m_pendingScrollDelta.y += static_cast<float>(yOffset);
}

void EditorInputBridge::QueueCharacter(uint32 codepoint)
{
    AppendCodepointUTF8(m_pendingTextInput, codepoint);
}

void EditorInputBridge::QueueMouseButton(int button, bool pressed)
{
    const int index = MapGLFWMouseButtonToUIIndex(button);
    if (index < 0 ||
        index >= static_cast<int>(UI::RVX_UI_MOUSE_BUTTON_COUNT))
    {
        return;
    }

    if (pressed)
    {
        m_pendingMouseButtonPresses[static_cast<size_t>(index)] = true;
    }
    else
    {
        m_pendingMouseButtonReleases[static_cast<size_t>(index)] = true;
    }
}

uint32 EditorInputBridge::MapGLFWKeyToUIKey(int glfwKey)
{
    if (glfwKey >= GLFW_KEY_A && glfwKey <= GLFW_KEY_Z)
    {
        return static_cast<uint32>('A' + (glfwKey - GLFW_KEY_A));
    }
    if (glfwKey >= GLFW_KEY_0 && glfwKey <= GLFW_KEY_9)
    {
        return static_cast<uint32>('0' + (glfwKey - GLFW_KEY_0));
    }

    switch (glfwKey)
    {
        case GLFW_KEY_BACKSPACE:
            return UI::RVX_UI_KEY_BACKSPACE;
        case GLFW_KEY_ENTER:
            return UI::RVX_UI_KEY_ENTER;
        case GLFW_KEY_ESCAPE:
            return UI::RVX_UI_KEY_ESCAPE;
        case GLFW_KEY_SPACE:
            return UI::RVX_UI_KEY_SPACE;
        case GLFW_KEY_DELETE:
            return UI::RVX_UI_KEY_DELETE;
        case GLFW_KEY_LEFT:
            return UI::RVX_UI_KEY_LEFT;
        case GLFW_KEY_RIGHT:
            return UI::RVX_UI_KEY_RIGHT;
        case GLFW_KEY_UP:
            return UI::RVX_UI_KEY_UP;
        case GLFW_KEY_DOWN:
            return UI::RVX_UI_KEY_DOWN;
        case GLFW_KEY_F10:
            return UI::RVX_UI_KEY_F10;
        case GLFW_KEY_HOME:
            return UI::RVX_UI_KEY_HOME;
        case GLFW_KEY_END:
            return UI::RVX_UI_KEY_END;
        default:
            return UI::RVX_UI_MAX_KEY_COUNT;
    }
}

void EditorInputBridge::AppendCodepointUTF8(std::string& output, uint32 codepoint)
{
    if (codepoint < 32u)
    {
        return;
    }

    if (codepoint <= 0x7Fu)
    {
        output.push_back(static_cast<char>(codepoint));
        return;
    }
    if (codepoint <= 0x7FFu)
    {
        output.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        return;
    }
    if (codepoint <= 0xFFFFu)
    {
        output.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
        return;
    }
    if (codepoint <= 0x10FFFFu)
    {
        output.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
}

void EditorInputBridge::PollWindowState(UI::UIInputSnapshot& input) const
{
    if (!m_window)
    {
        return;
    }

    double mouseX = 0.0;
    double mouseY = 0.0;
    glfwGetCursorPos(m_window, &mouseX, &mouseY);
    input.mousePosition = Vec2(static_cast<float>(mouseX),
                               static_cast<float>(mouseY));

    input.SetMouseButtonDown(
        UI::UIMouseButton::Left,
        glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    input.SetMouseButtonDown(
        UI::UIMouseButton::Right,
        glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
    input.SetMouseButtonDown(
        UI::UIMouseButton::Middle,
        glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS);
    input.SetMouseButtonDown(
        UI::UIMouseButton::X1,
        glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_4) == GLFW_PRESS);
    input.SetMouseButtonDown(
        UI::UIMouseButton::X2,
        glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_5) == GLFW_PRESS);

    input.modifiers = BuildModifierMask(m_window);
    for (int glfwKey = GLFW_KEY_SPACE; glfwKey <= GLFW_KEY_LAST; ++glfwKey)
    {
        const uint32 uiKey = MapGLFWKeyToUIKey(glfwKey);
        if (uiKey < UI::RVX_UI_MAX_KEY_COUNT)
        {
            input.SetKeyDown(uiKey, glfwGetKey(m_window, glfwKey) == GLFW_PRESS);
        }
    }
}

} // namespace RVX::Editor
