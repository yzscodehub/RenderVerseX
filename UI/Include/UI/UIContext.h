/**
 * @file UIContext.h
 * @brief Application-level UI frame, input, theme, and canvas state
 */

#pragma once

#include "UI/UICanvas.h"

#include <array>
#include <string>

namespace RVX::UI
{

constexpr uint32 RVX_UI_MAX_KEY_COUNT = 512;
constexpr uint32 RVX_UI_MOUSE_BUTTON_COUNT = 5;
constexpr uint32 RVX_UI_KEY_BACKSPACE = 8;
constexpr uint32 RVX_UI_KEY_TAB = 9;
constexpr uint32 RVX_UI_KEY_ENTER = 13;
constexpr uint32 RVX_UI_KEY_ESCAPE = 27;
constexpr uint32 RVX_UI_KEY_SPACE = 32;
constexpr uint32 RVX_UI_KEY_DELETE = 127;
constexpr uint32 RVX_UI_KEY_LEFT = 256;
constexpr uint32 RVX_UI_KEY_RIGHT = 257;
constexpr uint32 RVX_UI_KEY_UP = 258;
constexpr uint32 RVX_UI_KEY_DOWN = 259;
constexpr uint32 RVX_UI_KEY_F10 = 260;
constexpr uint32 RVX_UI_KEY_HOME = 261;
constexpr uint32 RVX_UI_KEY_END = 262;

enum class UIMouseButton : uint8
{
    Left = 0,
    Right,
    Middle,
    X1,
    X2
};

enum class UIInputModifier : uint8
{
    Ctrl = 1 << 0,
    Shift = 1 << 1,
    Alt = 1 << 2,
    Super = 1 << 3
};

inline uint32 ToMask(UIInputModifier modifier)
{
    return static_cast<uint32>(modifier);
}

struct UIInputSnapshot
{
    Vec2 mousePosition{0.0f};
    Vec2 scrollDelta{0.0f};
    std::array<bool, RVX_UI_MOUSE_BUTTON_COUNT> mouseButtonsDown{};
    std::array<bool, RVX_UI_MAX_KEY_COUNT> keysDown{};
    std::string textInput;
    uint32 modifiers = 0;
    bool wantsMouseCapture = false;
    bool wantsKeyboardCapture = false;

    void SetMouseButtonDown(UIMouseButton button, bool down);
    bool IsMouseButtonDown(UIMouseButton button) const;
    void SetKeyDown(uint32 keyCode, bool down);
    bool IsKeyDown(uint32 keyCode) const;
};

struct UIInputState
{
    UIInputSnapshot current;
    UIInputSnapshot previous;
    Vec2 mouseDelta{0.0f};
    std::array<bool, RVX_UI_MOUSE_BUTTON_COUNT> mouseButtonsPressed{};
    std::array<bool, RVX_UI_MOUSE_BUTTON_COUNT> mouseButtonsReleased{};
    std::array<bool, RVX_UI_MOUSE_BUTTON_COUNT> mouseButtonsDoubleClicked{};
    std::array<bool, RVX_UI_MAX_KEY_COUNT> keysPressed{};
    std::array<bool, RVX_UI_MAX_KEY_COUNT> keysReleased{};

    bool WasMouseButtonPressed(UIMouseButton button) const;
    bool WasMouseButtonReleased(UIMouseButton button) const;
    bool WasMouseButtonDoubleClicked(UIMouseButton button) const;
    bool WasKeyPressed(uint32 keyCode) const;
    bool WasKeyReleased(uint32 keyCode) const;
};

struct UIThemeColors
{
    UIColor windowBackground{0.10f, 0.11f, 0.12f, 1.0f};
    UIColor panelBackground{0.13f, 0.14f, 0.15f, 1.0f};
    UIColor surface{0.17f, 0.18f, 0.20f, 1.0f};
    UIColor surfaceHover{0.21f, 0.23f, 0.25f, 1.0f};
    UIColor surfaceActive{0.24f, 0.27f, 0.30f, 1.0f};
    UIColor text{0.90f, 0.92f, 0.94f, 1.0f};
    UIColor textMuted{0.66f, 0.70f, 0.75f, 1.0f};
    UIColor accent{0.18f, 0.55f, 0.95f, 1.0f};
    UIColor accentHover{0.28f, 0.65f, 1.0f, 1.0f};
    UIColor border{0.26f, 0.28f, 0.31f, 1.0f};
    UIColor warning{0.92f, 0.68f, 0.20f, 1.0f};
    UIColor error{0.90f, 0.25f, 0.22f, 1.0f};
};

struct UIThemeMetrics
{
    float scale = 1.0f;
    float fontSize = 14.0f;
    float smallFontSize = 12.0f;
    float largeFontSize = 18.0f;
    float controlHeight = 28.0f;
    float toolbarHeight = 34.0f;
    float statusBarHeight = 24.0f;
    float spacing = 6.0f;
    float padding = 8.0f;
    float borderWidth = 1.0f;
    float cornerRadius = 3.0f;
};

struct UITheme
{
    std::string name = "Runtime Dark";
    UIThemeColors colors;
    UIThemeMetrics metrics;

    static UITheme RuntimeDark();
};

struct UIFrameDesc
{
    uint32 width = 0;
    uint32 height = 0;
    float deltaTime = 0.0f;
    float scaleFactor = 1.0f;
    float inputScaleFactor = 0.0f;
    UIInputSnapshot input;
};

struct UIContextDesc
{
    uint32 width = 0;
    uint32 height = 0;
    float scaleFactor = 1.0f;
    float inputScaleFactor = 0.0f;
    std::string debugName = "UIContext";
    UITheme theme = UITheme::RuntimeDark();
};

class UILayoutRegion
{
public:
    UILayoutRegion() = default;
    UILayoutRegion(const Rect& bounds, const EdgeInsets& padding, float spacing);

    void Reset(const Rect& bounds, const EdgeInsets& padding, float spacing);
    Rect AllocateRow(float height);
    Rect AllocateColumn(float width, float height);

    const Rect& GetBounds() const { return m_bounds; }
    const Vec2& GetCursor() const { return m_cursor; }

private:
    Rect m_bounds;
    EdgeInsets m_padding;
    float m_spacing = 0.0f;
    Vec2 m_cursor{0.0f};
};

class UIContext
{
public:
    bool Initialize(const UIContextDesc& desc);
    void Shutdown();

    bool BeginFrame(const UIFrameDesc& desc);
    void EndFrame();

    bool IsInitialized() const { return m_initialized; }
    bool IsFrameActive() const { return m_frameActive; }
    const std::string& GetDebugName() const { return m_debugName; }

    uint64 GetFrameIndex() const { return m_frameIndex; }
    float GetDeltaTime() const { return m_deltaTime; }
    Vec2 GetSurfaceSize() const { return Vec2(static_cast<float>(m_width), static_cast<float>(m_height)); }
    uint32 GetWidth() const { return m_width; }
    uint32 GetHeight() const { return m_height; }
    float GetScaleFactor() const { return m_scaleFactor; }
    float GetInputScaleFactor() const { return m_inputScaleFactor; }

    UICanvas& GetCanvas() { return m_canvas; }
    const UICanvas& GetCanvas() const { return m_canvas; }

    const UIInputState& GetInput() const { return m_inputState; }

    const UITheme& GetTheme() const { return m_theme; }
    void SetTheme(const UITheme& theme);

private:
    void UpdateInputState(const UIInputSnapshot& input);
    void DispatchInputEvents();

    bool m_initialized = false;
    bool m_frameActive = false;
    std::string m_debugName;
    uint32 m_width = 0;
    uint32 m_height = 0;
    float m_scaleFactor = 1.0f;
    float m_inputScaleFactor = 1.0f;
    float m_deltaTime = 0.0f;
    uint64 m_frameIndex = 0;
    UITheme m_baseTheme;
    UITheme m_theme;
    UIInputState m_inputState;
    std::array<float, RVX_UI_MOUSE_BUTTON_COUNT> m_lastClickAge{};
    std::array<Vec2, RVX_UI_MOUSE_BUTTON_COUNT> m_lastClickPosition{};
    UICanvas m_canvas;
};

} // namespace RVX::UI
