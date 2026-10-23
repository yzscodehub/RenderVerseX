/**
 * @file UIContext.cpp
 * @brief Application-level UI context implementation
 */

#include "UI/UIContext.h"

#include <algorithm>
#include <cmath>

namespace RVX::UI
{
namespace
{
    constexpr float RVX_UI_DOUBLE_CLICK_MAX_INTERVAL = 0.30f;
    constexpr float RVX_UI_DOUBLE_CLICK_MAX_DISTANCE = 6.0f;
    constexpr float RVX_UI_CLICK_AGE_RESET = 1000.0f;

    size_t MouseButtonIndex(UIMouseButton button)
    {
        return static_cast<size_t>(button);
    }

    bool IsValidMouseButton(UIMouseButton button)
    {
        return MouseButtonIndex(button) < RVX_UI_MOUSE_BUTTON_COUNT;
    }

    bool IsValidKey(uint32 keyCode)
    {
        return keyCode < RVX_UI_MAX_KEY_COUNT;
    }

    bool IsWithinDoubleClickDistance(const Vec2& lhs, const Vec2& rhs)
    {
        const float dx = lhs.x - rhs.x;
        const float dy = lhs.y - rhs.y;
        return dx * dx + dy * dy <=
               RVX_UI_DOUBLE_CLICK_MAX_DISTANCE * RVX_UI_DOUBLE_CLICK_MAX_DISTANCE;
    }

    float SanitizeScale(float scale, float fallback)
    {
        if (!std::isfinite(scale) || scale <= 0.0f)
        {
            scale = fallback;
        }
        return std::max(0.01f, scale);
    }

    float SnapThemeMetric(float value)
    {
        if (!std::isfinite(value))
        {
            return 0.0f;
        }
        return std::round(value);
    }

    float ScaleThemeMetric(float value, float scale, float minimum = 0.0f)
    {
        return std::max(minimum, SnapThemeMetric(value * scale));
    }

    float ScaleStrokeMetric(float value, float scale)
    {
        if (value <= 0.0f)
        {
            return 0.0f;
        }
        return std::max(1.0f, SnapThemeMetric(value * scale));
    }

    UITheme BuildScaledTheme(const UITheme& baseTheme, float scale)
    {
        UITheme theme = baseTheme;
        const float sanitizedScale = SanitizeScale(scale, 1.0f);
        theme.metrics.scale = sanitizedScale;
        theme.metrics.fontSize =
            ScaleThemeMetric(baseTheme.metrics.fontSize, sanitizedScale, 13.0f);
        theme.metrics.smallFontSize =
            ScaleThemeMetric(baseTheme.metrics.smallFontSize, sanitizedScale, 11.0f);
        theme.metrics.largeFontSize =
            ScaleThemeMetric(baseTheme.metrics.largeFontSize, sanitizedScale, 16.0f);
        theme.metrics.controlHeight =
            ScaleThemeMetric(baseTheme.metrics.controlHeight, sanitizedScale, 24.0f);
        theme.metrics.toolbarHeight =
            ScaleThemeMetric(baseTheme.metrics.toolbarHeight, sanitizedScale, 28.0f);
        theme.metrics.statusBarHeight =
            ScaleThemeMetric(baseTheme.metrics.statusBarHeight, sanitizedScale, 22.0f);
        theme.metrics.spacing =
            ScaleThemeMetric(baseTheme.metrics.spacing, sanitizedScale, 1.0f);
        theme.metrics.padding =
            ScaleThemeMetric(baseTheme.metrics.padding, sanitizedScale, 1.0f);
        theme.metrics.borderWidth =
            ScaleStrokeMetric(baseTheme.metrics.borderWidth, sanitizedScale);
        theme.metrics.cornerRadius =
            ScaleThemeMetric(baseTheme.metrics.cornerRadius, sanitizedScale);
        return theme;
    }
}

void UIInputSnapshot::SetMouseButtonDown(UIMouseButton button, bool down)
{
    if (IsValidMouseButton(button))
    {
        mouseButtonsDown[MouseButtonIndex(button)] = down;
    }
}

bool UIInputSnapshot::IsMouseButtonDown(UIMouseButton button) const
{
    return IsValidMouseButton(button) && mouseButtonsDown[MouseButtonIndex(button)];
}

void UIInputSnapshot::SetKeyDown(uint32 keyCode, bool down)
{
    if (IsValidKey(keyCode))
    {
        keysDown[keyCode] = down;
    }
}

bool UIInputSnapshot::IsKeyDown(uint32 keyCode) const
{
    return IsValidKey(keyCode) && keysDown[keyCode];
}

bool UIInputState::WasMouseButtonPressed(UIMouseButton button) const
{
    return IsValidMouseButton(button) && mouseButtonsPressed[MouseButtonIndex(button)];
}

bool UIInputState::WasMouseButtonReleased(UIMouseButton button) const
{
    return IsValidMouseButton(button) && mouseButtonsReleased[MouseButtonIndex(button)];
}

bool UIInputState::WasMouseButtonDoubleClicked(UIMouseButton button) const
{
    return IsValidMouseButton(button) && mouseButtonsDoubleClicked[MouseButtonIndex(button)];
}

bool UIInputState::WasKeyPressed(uint32 keyCode) const
{
    return IsValidKey(keyCode) && keysPressed[keyCode];
}

bool UIInputState::WasKeyReleased(uint32 keyCode) const
{
    return IsValidKey(keyCode) && keysReleased[keyCode];
}

UITheme UITheme::RuntimeDark()
{
    UITheme theme;
    theme.name = "Runtime Dark";
    return theme;
}

UILayoutRegion::UILayoutRegion(const Rect& bounds, const EdgeInsets& padding, float spacing)
{
    Reset(bounds, padding, spacing);
}

void UILayoutRegion::Reset(const Rect& bounds, const EdgeInsets& padding, float spacing)
{
    m_bounds = bounds;
    m_padding = padding;
    m_spacing = spacing;
    m_cursor = Vec2(bounds.x + padding.left, bounds.y + padding.top);
}

Rect UILayoutRegion::AllocateRow(float height)
{
    const float rowHeight = std::max(0.0f, height);
    const float rowWidth = std::max(0.0f, m_bounds.width - m_padding.Horizontal());
    Rect rect(m_cursor.x, m_cursor.y, rowWidth, rowHeight);
    m_cursor.x = m_bounds.x + m_padding.left;
    m_cursor.y += rowHeight + m_spacing;
    return rect;
}

Rect UILayoutRegion::AllocateColumn(float width, float height)
{
    const float columnWidth = std::max(0.0f, width);
    const float columnHeight = std::max(0.0f, height);
    Rect rect(m_cursor.x, m_cursor.y, columnWidth, columnHeight);
    m_cursor.x += columnWidth + m_spacing;
    return rect;
}

bool UIContext::Initialize(const UIContextDesc& desc)
{
    if (m_initialized)
    {
        Shutdown();
    }

    m_debugName = desc.debugName;
    m_width = desc.width;
    m_height = desc.height;
    m_scaleFactor = SanitizeScale(desc.scaleFactor, 1.0f);
    m_inputScaleFactor =
        SanitizeScale(desc.inputScaleFactor > 0.0f ? desc.inputScaleFactor
                                                    : m_scaleFactor,
                      m_scaleFactor);
    m_baseTheme = desc.theme;
    m_theme = BuildScaledTheme(m_baseTheme, m_scaleFactor);
    m_deltaTime = 0.0f;
    m_frameIndex = 0;
    m_frameActive = false;
    m_inputState = {};
    m_lastClickAge.fill(RVX_UI_CLICK_AGE_RESET);
    m_lastClickPosition.fill(Vec2(0.0f));

    m_canvas.Initialize(static_cast<float>(m_width), static_cast<float>(m_height));
    m_canvas.SetScaleFactor(m_inputScaleFactor);

    m_initialized = true;
    return true;
}

void UIContext::Shutdown()
{
    m_canvas.Shutdown();
    m_inputState = {};
    m_lastClickAge.fill(RVX_UI_CLICK_AGE_RESET);
    m_lastClickPosition.fill(Vec2(0.0f));
    m_initialized = false;
    m_frameActive = false;
    m_width = 0;
    m_height = 0;
    m_scaleFactor = 1.0f;
    m_inputScaleFactor = 1.0f;
    m_deltaTime = 0.0f;
    m_frameIndex = 0;
}

bool UIContext::BeginFrame(const UIFrameDesc& desc)
{
    if (!m_initialized)
    {
        return false;
    }

    m_frameActive = true;
    ++m_frameIndex;
    m_deltaTime = std::max(0.0f, desc.deltaTime);
    m_width = desc.width;
    m_height = desc.height;
    m_scaleFactor = SanitizeScale(desc.scaleFactor, m_scaleFactor);
    m_inputScaleFactor =
        SanitizeScale(desc.inputScaleFactor > 0.0f ? desc.inputScaleFactor
                                                    : m_scaleFactor,
                      m_scaleFactor);
    m_theme = BuildScaledTheme(m_baseTheme, m_scaleFactor);

    m_canvas.SetSize(static_cast<float>(m_width), static_cast<float>(m_height));
    m_canvas.SetScaleFactor(m_inputScaleFactor);

    UpdateInputState(desc.input);
    DispatchInputEvents();
    m_canvas.Update(m_deltaTime);
    return true;
}

void UIContext::EndFrame()
{
    if (!m_initialized)
    {
        return;
    }

    m_frameActive = false;
}

void UIContext::SetTheme(const UITheme& theme)
{
    m_baseTheme = theme;
    m_theme = BuildScaledTheme(m_baseTheme, m_scaleFactor);
}

void UIContext::UpdateInputState(const UIInputSnapshot& input)
{
    m_inputState.previous = m_inputState.current;
    m_inputState.current = input;
    m_inputState.mouseDelta =
        m_inputState.current.mousePosition - m_inputState.previous.mousePosition;

    for (size_t index = 0; index < RVX_UI_MOUSE_BUTTON_COUNT; ++index)
    {
        const bool wasDown = m_inputState.previous.mouseButtonsDown[index];
        const bool isDown = m_inputState.current.mouseButtonsDown[index];
        m_inputState.mouseButtonsPressed[index] = !wasDown && isDown;
        m_inputState.mouseButtonsReleased[index] = wasDown && !isDown;
        m_inputState.mouseButtonsDoubleClicked[index] = false;

        m_lastClickAge[index] =
            std::min(RVX_UI_CLICK_AGE_RESET, m_lastClickAge[index] + m_deltaTime);
        if (m_inputState.mouseButtonsPressed[index])
        {
            const bool clickedRecently =
                m_lastClickAge[index] <= RVX_UI_DOUBLE_CLICK_MAX_INTERVAL;
            const bool clickedNearby =
                IsWithinDoubleClickDistance(m_lastClickPosition[index],
                                            m_inputState.current.mousePosition);
            m_inputState.mouseButtonsDoubleClicked[index] = clickedRecently && clickedNearby;
            m_lastClickAge[index] = 0.0f;
            m_lastClickPosition[index] = m_inputState.current.mousePosition;
        }
    }

    for (size_t index = 0; index < RVX_UI_MAX_KEY_COUNT; ++index)
    {
        const bool wasDown = m_inputState.previous.keysDown[index];
        const bool isDown = m_inputState.current.keysDown[index];
        m_inputState.keysPressed[index] = !wasDown && isDown;
        m_inputState.keysReleased[index] = wasDown && !isDown;
    }
}

void UIContext::DispatchInputEvents()
{
    UIEvent moveEvent;
    moveEvent.type = UIEventType::MouseMove;
    moveEvent.position = m_inputState.current.mousePosition;
    moveEvent.delta = m_inputState.mouseDelta;
    moveEvent.modifiers = m_inputState.current.modifiers;
    m_canvas.HandleEvent(moveEvent);

    for (size_t index = 0; index < RVX_UI_MOUSE_BUTTON_COUNT; ++index)
    {
        if (!m_inputState.mouseButtonsPressed[index] &&
            !m_inputState.mouseButtonsReleased[index])
        {
            continue;
        }

        UIEvent mouseEvent;
        mouseEvent.type = m_inputState.mouseButtonsPressed[index]
                              ? UIEventType::MouseDown
                              : UIEventType::MouseUp;
        mouseEvent.position = m_inputState.current.mousePosition;
        mouseEvent.button = static_cast<int>(index);
        mouseEvent.modifiers = m_inputState.current.modifiers;
        m_canvas.HandleEvent(mouseEvent);
    }

    if (m_inputState.current.scrollDelta.x != 0.0f ||
        m_inputState.current.scrollDelta.y != 0.0f)
    {
        UIEvent scrollEvent;
        scrollEvent.type = UIEventType::Scroll;
        scrollEvent.position = m_inputState.current.mousePosition;
        scrollEvent.delta = m_inputState.current.scrollDelta;
        scrollEvent.modifiers = m_inputState.current.modifiers;
        m_canvas.HandleEvent(scrollEvent);
    }

    for (size_t keyCode = 0; keyCode < RVX_UI_MAX_KEY_COUNT; ++keyCode)
    {
        if (!m_inputState.keysPressed[keyCode] &&
            !m_inputState.keysReleased[keyCode])
        {
            continue;
        }

        UIEvent keyEvent;
        keyEvent.type = m_inputState.keysPressed[keyCode]
                            ? UIEventType::KeyDown
                            : UIEventType::KeyUp;
        keyEvent.keyCode = static_cast<int>(keyCode);
        keyEvent.modifiers = m_inputState.current.modifiers;
        m_canvas.HandleEvent(keyEvent);
    }

    if (!m_inputState.current.textInput.empty())
    {
        UIEvent textEvent;
        textEvent.type = UIEventType::TextInput;
        textEvent.text = m_inputState.current.textInput;
        textEvent.modifiers = m_inputState.current.modifiers;
        m_canvas.HandleEvent(textEvent);
    }
}

} // namespace RVX::UI
