/**
 * @file EditorComboBox.h
 * @brief Native editor combo box widget
 */

#pragma once

#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RVX::Editor
{

class EditorComboBox final : public UI::Widget
{
public:
    using Ptr = std::shared_ptr<EditorComboBox>;
    using OpenRequestedCallback = std::function<void(const UI::Rect&)>;

    EditorComboBox();

    const char* GetTypeName() const override { return "EditorComboBox"; }

    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled);

    const std::vector<std::string>& GetOptions() const { return m_options; }
    void SetOptions(std::vector<std::string> options);

    int32 GetSelectedIndex() const { return m_selectedIndex; }
    void SetSelectedIndex(int32 selectedIndex) { m_selectedIndex = selectedIndex; }

    const std::string& GetPlaceholder() const { return m_placeholder; }
    void SetPlaceholder(const std::string& placeholder) { m_placeholder = placeholder; }

    void SetOnOpenRequested(OpenRequestedCallback callback)
    {
        m_onOpenRequested = std::move(callback);
    }

    void ApplyTheme(const UI::UITheme& theme);

    static Ptr Create()
    {
        return std::make_shared<EditorComboBox>();
    }

protected:
    void OnRender(UI::UIRenderer& renderer) override;
    bool HandleEvent(const UI::UIEvent& event) override;

private:
    void RequestOpen();
    std::string GetSelectedText() const;

    bool m_enabled = true;
    std::vector<std::string> m_options;
    int32 m_selectedIndex = -1;
    std::string m_placeholder = "Select";
    OpenRequestedCallback m_onOpenRequested;

    UI::UIColor m_backgroundColor{0.12f, 0.13f, 0.14f, 1.0f};
    UI::UIColor m_hoverColor{0.16f, 0.17f, 0.19f, 1.0f};
    UI::UIColor m_focusColor{0.18f, 0.20f, 0.23f, 1.0f};
    UI::UIColor m_borderColor{0.26f, 0.28f, 0.31f, 1.0f};
    UI::UIColor m_focusBorderColor{0.18f, 0.55f, 0.95f, 1.0f};
    UI::UIColor m_textColor{0.90f, 0.92f, 0.94f, 1.0f};
    UI::UIColor m_placeholderColor{0.58f, 0.63f, 0.68f, 1.0f};
    UI::UIColor m_disabledColor{0.20f, 0.21f, 0.22f, 0.55f};
    UI::UIColor m_disabledTextColor{0.58f, 0.63f, 0.68f, 0.65f};
    float m_fontSize = 12.0f;
};

} // namespace RVX::Editor
