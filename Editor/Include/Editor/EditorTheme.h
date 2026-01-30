/**
 * @file EditorTheme.h
 * @brief Editor visual theme and styling
 */

#pragma once

#include "Core/MathTypes.h"

namespace RVX::Editor
{

/**
 * @brief Editor color scheme
 */
struct EditorColors
{
    Vec4 background{0.1f, 0.1f, 0.1f, 1.0f};
    Vec4 backgroundDark{0.08f, 0.08f, 0.08f, 1.0f};
    Vec4 backgroundLight{0.15f, 0.15f, 0.15f, 1.0f};
    
    Vec4 text{0.9f, 0.9f, 0.9f, 1.0f};
    Vec4 textDisabled{0.5f, 0.5f, 0.5f, 1.0f};
    Vec4 textHighlight{1.0f, 1.0f, 1.0f, 1.0f};
    
    Vec4 accent{0.26f, 0.59f, 0.98f, 1.0f};
    Vec4 accentHover{0.36f, 0.69f, 1.0f, 1.0f};
    Vec4 accentActive{0.16f, 0.49f, 0.88f, 1.0f};
    
    Vec4 selection{0.26f, 0.59f, 0.98f, 0.35f};
    Vec4 selectionHover{0.26f, 0.59f, 0.98f, 0.5f};
    
    Vec4 success{0.2f, 0.8f, 0.2f, 1.0f};
    Vec4 warning{0.9f, 0.7f, 0.1f, 1.0f};
    Vec4 error{0.9f, 0.2f, 0.2f, 1.0f};
    Vec4 info{0.2f, 0.6f, 0.9f, 1.0f};
    
    Vec4 border{0.25f, 0.25f, 0.25f, 1.0f};
    Vec4 borderHighlight{0.4f, 0.4f, 0.4f, 1.0f};
};

/**
 * @brief Editor theme manager
 */
class EditorTheme
{
public:
    static EditorTheme& Get();

    /**
     * @brief Apply the editor theme to ImGui
     */
    void ApplyTheme();

    /**
     * @brief Get current colors
     */
    const EditorColors& GetColors() const { return m_colors; }
    EditorColors& GetColors() { return m_colors; }

    /**
     * @brief Set color scheme
     */
    void SetColors(const EditorColors& colors);

    /**
     * @brief Apply dark theme preset
     */
    void ApplyDarkTheme();

    /**
     * @brief Apply light theme preset
     */
    void ApplyLightTheme();

private:
    EditorTheme() = default;
    EditorColors m_colors;
};

} // namespace RVX::Editor
