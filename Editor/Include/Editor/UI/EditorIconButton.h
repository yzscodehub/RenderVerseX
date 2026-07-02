/**
 * @file EditorIconButton.h
 * @brief Native editor icon button widget
 */

#pragma once

#include "Editor/UI/EditorVectorIcon.h"
#include "UI/Widgets/Button.h"

#include <memory>
#include <string>

namespace RVX::UI
{
struct UITheme;
}

namespace RVX::Editor
{

struct EditorIconButtonActionStyleDesc
{
    bool active = false;
    bool usePanelBackground = false;
    float iconScale = 0.50f;
    float pressedAlpha = 0.75f;
};

/**
 * @brief Button variant that keeps a semantic text label and renders an icon.
 */
class EditorIconButton final : public UI::Button
{
public:
    using Ptr = std::shared_ptr<EditorIconButton>;

    EditorIconButton();
    explicit EditorIconButton(const std::string& text);

    const char* GetTypeName() const override { return "EditorIconButton"; }

    // =========================================================================
    // Icon
    // =========================================================================

    const std::string& GetIconName() const { return m_iconName; }
    void SetIconName(const std::string& iconName) { m_iconName = iconName; }

    bool GetShowText() const { return m_showText; }
    void SetShowText(bool showText) { m_showText = showText; }

    float GetIconSize() const { return m_iconSize; }
    void SetIconSize(float iconSize) { m_iconSize = iconSize; }
    const EditorVectorIconDrawStats& GetLastIconDrawStats() const
    {
        return m_lastIconDrawStats;
    }

    // =========================================================================
    // Styling
    // =========================================================================

    void SetSurfaceColors(const UI::UIColor& normal,
                          const UI::UIColor& hover,
                          const UI::UIColor& pressed,
                          const UI::UIColor& disabled);
    void SetIconColors(const UI::UIColor& normal,
                       const UI::UIColor& hover,
                       const UI::UIColor& pressed,
                       const UI::UIColor& disabled);
    void SetFocusIndicator(const UI::UIColor& color, float width);
    void ApplyActionStyle(const UI::UITheme& theme,
                          const EditorIconButtonActionStyleDesc& desc = {});

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr Create(const std::string& text = {},
                      const std::string& iconName = {})
    {
        Ptr button = std::make_shared<EditorIconButton>(text);
        button->SetIconName(iconName);
        return button;
    }

protected:
    void OnRender(UI::UIRenderer& renderer) override;

private:
    UI::UIColor ResolveSurfaceColor() const;
    UI::UIColor ResolveIconColor() const;

    std::string m_iconName;
    bool m_showText = false;
    float m_iconSize = 16.0f;
    EditorVectorIconDrawStats m_lastIconDrawStats;

    UI::UIColor m_normalColor{0.17f, 0.18f, 0.20f, 1.0f};
    UI::UIColor m_hoverColor{0.21f, 0.23f, 0.25f, 1.0f};
    UI::UIColor m_pressedColor{0.24f, 0.27f, 0.30f, 1.0f};
    UI::UIColor m_disabledColor{0.17f, 0.18f, 0.20f, 0.35f};

    UI::UIColor m_iconColor{0.90f, 0.92f, 0.94f, 1.0f};
    UI::UIColor m_iconHoverColor{0.90f, 0.92f, 0.94f, 1.0f};
    UI::UIColor m_iconPressedColor{0.90f, 0.92f, 0.94f, 1.0f};
    UI::UIColor m_iconDisabledColor{0.58f, 0.63f, 0.68f, 0.55f};

    UI::UIColor m_focusColor{0.18f, 0.55f, 0.95f, 1.0f};
    float m_focusWidth = 2.0f;
};

} // namespace RVX::Editor
