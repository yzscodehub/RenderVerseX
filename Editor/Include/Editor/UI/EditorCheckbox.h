/**
 * @file EditorCheckbox.h
 * @brief Native editor checkbox widget
 */

#pragma once

#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <functional>
#include <memory>
#include <string>

namespace RVX::Editor
{

class EditorCheckbox final : public UI::Widget
{
public:
    using Ptr = std::shared_ptr<EditorCheckbox>;
    using CheckedChangedCallback = std::function<void(bool)>;

    EditorCheckbox();

    const char* GetTypeName() const override { return "EditorCheckbox"; }

    bool IsChecked() const { return m_checked; }
    void SetChecked(bool checked) { m_checked = checked; }

    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled);

    const std::string& GetLabel() const { return m_label; }
    void SetLabel(const std::string& label) { m_label = label; }

    void SetOnCheckedChanged(CheckedChangedCallback callback)
    {
        m_onCheckedChanged = std::move(callback);
    }

    void ApplyTheme(const UI::UITheme& theme);

    static Ptr Create(const std::string& label = {})
    {
        Ptr checkbox = std::make_shared<EditorCheckbox>();
        checkbox->SetLabel(label);
        return checkbox;
    }

protected:
    void OnRender(UI::UIRenderer& renderer) override;
    bool HandleEvent(const UI::UIEvent& event) override;

private:
    void Toggle();

    bool m_checked = false;
    bool m_enabled = true;
    std::string m_label;
    CheckedChangedCallback m_onCheckedChanged;

    UI::UIColor m_backgroundColor{0.12f, 0.13f, 0.14f, 1.0f};
    UI::UIColor m_hoverColor{0.16f, 0.17f, 0.19f, 1.0f};
    UI::UIColor m_pressedColor{0.18f, 0.20f, 0.23f, 1.0f};
    UI::UIColor m_borderColor{0.26f, 0.28f, 0.31f, 1.0f};
    UI::UIColor m_accentColor{0.18f, 0.55f, 0.95f, 1.0f};
    UI::UIColor m_disabledColor{0.20f, 0.21f, 0.22f, 0.55f};
    UI::UIColor m_textColor{0.90f, 0.92f, 0.94f, 1.0f};
    UI::UIColor m_disabledTextColor{0.58f, 0.63f, 0.68f, 0.65f};
    float m_fontSize = 12.0f;
};

} // namespace RVX::Editor
