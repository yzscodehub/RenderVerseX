/**
 * @file UITypes.h
 * @brief Core UI types and enumerations
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include <functional>
#include <string>

namespace RVX::UI
{

/**
 * @brief UI anchor presets
 */
enum class Anchor : uint8
{
    TopLeft,
    TopCenter,
    TopRight,
    MiddleLeft,
    MiddleCenter,
    MiddleRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
    StretchTop,
    StretchMiddle,
    StretchBottom,
    StretchLeft,
    StretchCenter,
    StretchRight,
    StretchAll
};

/**
 * @brief Text alignment
 */
enum class TextAlign : uint8
{
    Left,
    Center,
    Right
};

/**
 * @brief Vertical alignment
 */
enum class VerticalAlign : uint8
{
    Top,
    Middle,
    Bottom
};

/**
 * @brief Size mode for widgets
 */
enum class SizeMode : uint8
{
    Fixed,          ///< Fixed pixel size
    Relative,       ///< Percentage of parent
    FitContent,     ///< Size to content
    Expand          ///< Fill available space
};

/**
 * @brief Visibility state
 */
enum class Visibility : uint8
{
    Visible,        ///< Visible and interactive
    Hidden,         ///< Hidden but takes space
    Collapsed       ///< Hidden and takes no space
};

/**
 * @brief Mouse cursor style
 */
enum class CursorStyle : uint8
{
    Arrow,
    Hand,
    IBeam,
    ResizeH,
    ResizeV,
    ResizeDiag,
    Move,
    NotAllowed
};

/**
 * @brief 2D rectangle
 */
struct Rect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    Rect() = default;
    Rect(float x, float y, float w, float h) : x(x), y(y), width(w), height(h) {}

    float Left() const { return x; }
    float Right() const { return x + width; }
    float Top() const { return y; }
    float Bottom() const { return y + height; }

    Vec2 Position() const { return Vec2(x, y); }
    Vec2 Size() const { return Vec2(width, height); }
    Vec2 Center() const { return Vec2(x + width * 0.5f, y + height * 0.5f); }

    bool Contains(float px, float py) const
    {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    bool Contains(const Vec2& point) const { return Contains(point.x, point.y); }

    bool Overlaps(const Rect& other) const
    {
        return x < other.Right() && Right() > other.x &&
               y < other.Bottom() && Bottom() > other.y;
    }

    Rect Expand(float amount) const
    {
        return Rect(x - amount, y - amount, width + amount * 2, height + amount * 2);
    }
};

/**
 * @brief Edge insets (margin/padding)
 */
struct EdgeInsets
{
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;

    EdgeInsets() = default;
    EdgeInsets(float all) : left(all), right(all), top(all), bottom(all) {}
    EdgeInsets(float horizontal, float vertical)
        : left(horizontal), right(horizontal), top(vertical), bottom(vertical) {}
    EdgeInsets(float l, float r, float t, float b)
        : left(l), right(r), top(t), bottom(b) {}

    float Horizontal() const { return left + right; }
    float Vertical() const { return top + bottom; }
};

/**
 * @brief UI color (RGBA)
 */
struct UIColor
{
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    UIColor() = default;
    UIColor(float r, float g, float b, float a = 1.0f) : r(r), g(g), b(b), a(a) {}
    explicit UIColor(const Vec4& v) : r(v.r), g(v.g), b(v.b), a(v.a) {}

    Vec4 ToVec4() const { return Vec4(r, g, b, a); }

    UIColor WithAlpha(float alpha) const { return UIColor(r, g, b, alpha); }

    static UIColor White() { return UIColor(1, 1, 1, 1); }
    static UIColor Black() { return UIColor(0, 0, 0, 1); }
    static UIColor Red() { return UIColor(1, 0, 0, 1); }
    static UIColor Green() { return UIColor(0, 1, 0, 1); }
    static UIColor Blue() { return UIColor(0, 0, 1, 1); }
    static UIColor Yellow() { return UIColor(1, 1, 0, 1); }
    static UIColor Transparent() { return UIColor(0, 0, 0, 0); }
    
    static UIColor FromHex(uint32 hex)
    {
        return UIColor(
            ((hex >> 24) & 0xFF) / 255.0f,
            ((hex >> 16) & 0xFF) / 255.0f,
            ((hex >> 8) & 0xFF) / 255.0f,
            (hex & 0xFF) / 255.0f
        );
    }
};

/**
 * @brief UI style properties
 */
struct Style
{
    UIColor backgroundColor = UIColor::Transparent();
    UIColor textColor = UIColor::White();
    UIColor borderColor = UIColor::Transparent();
    
    float borderWidth = 0.0f;
    float borderRadius = 0.0f;
    
    EdgeInsets padding;
    EdgeInsets margin;

    float fontSize = 14.0f;
    std::string fontFamily;
};

/**
 * @brief Input event types
 */
enum class UIEventType : uint8
{
    None,
    MouseEnter,
    MouseLeave,
    MouseDown,
    MouseUp,
    MouseMove,
    Click,
    DoubleClick,
    Scroll,
    KeyDown,
    KeyUp,
    TextInput,
    Focus,
    Blur,
    DragStart,
    Drag,
    DragEnd
};

/**
 * @brief UI input event
 */
struct UIEvent
{
    UIEventType type = UIEventType::None;
    Vec2 position{0.0f};
    Vec2 delta{0.0f};
    int button = 0;
    int keyCode = 0;
    uint32 modifiers = 0;
    std::string text;
    bool handled = false;
};

/**
 * @brief Event callback type
 */
using EventCallback = std::function<void(const UIEvent&)>;

} // namespace RVX::UI
