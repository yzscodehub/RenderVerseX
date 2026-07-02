/**
 * @file EditorUIAutomation.cpp
 * @brief Deterministic native editor UI event automation helpers
 */

#include "Editor/UI/EditorUIAutomation.h"
#include "Editor/UI/EditorTextInput.h"
#include "Editor/UI/EditorUIHost.h"
#include "UI/UIContext.h"
#include "UI/Widget.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"

#include <sstream>
#include <utility>

namespace RVX::Editor
{
namespace
{
    bool IsVisibleInHierarchy(const UI::Widget& widget)
    {
        const UI::Widget* current = &widget;
        while (current)
        {
            if (!current->IsVisible())
            {
                return false;
            }
            current = current->GetParent();
        }
        return true;
    }

    std::string GetWidgetText(const UI::Widget& widget)
    {
        if (const auto* button = dynamic_cast<const UI::Button*>(&widget))
        {
            return button->GetText();
        }
        if (const auto* label = dynamic_cast<const UI::Label*>(&widget))
        {
            return label->GetText();
        }
        if (const auto* input = dynamic_cast<const EditorTextInput*>(&widget))
        {
            return input->GetText();
        }
        return {};
    }

    bool IsWidgetEnabled(const UI::Widget& widget)
    {
        if (const auto* button = dynamic_cast<const UI::Button*>(&widget))
        {
            return button->IsEnabled();
        }
        return widget.IsInteractive();
    }

    bool IsWidgetOrDescendant(const UI::Widget* root, const UI::Widget* widget)
    {
        if (!root || !widget)
        {
            return false;
        }
        if (root == widget)
        {
            return true;
        }

        for (const UI::Widget::Ptr& child : root->GetChildren())
        {
            if (IsWidgetOrDescendant(child.get(), widget))
            {
                return true;
            }
        }
        return false;
    }

    std::string FormatHitWidget(const UI::Widget* widget)
    {
        if (!widget)
        {
            return "<none>";
        }
        if (!widget->GetName().empty())
        {
            return widget->GetName();
        }
        return widget->GetTypeName();
    }

    EditorUIAutomationWidgetSnapshot CaptureSnapshot(
        const UI::Widget& widget)
    {
        EditorUIAutomationWidgetSnapshot snapshot;
        snapshot.found = true;
        snapshot.name = widget.GetName();
        snapshot.typeName = widget.GetTypeName();
        snapshot.text = GetWidgetText(widget);
        snapshot.tooltipText = widget.GetTooltipText();
        snapshot.globalRect = widget.GetGlobalRect();
        snapshot.visible = IsVisibleInHierarchy(widget);
        snapshot.interactive = widget.IsInteractive();
        snapshot.enabled = IsWidgetEnabled(widget);
        snapshot.focused = widget.IsFocused();
        return snapshot;
    }

    std::string FormatWidgetSnapshot(
        const EditorUIAutomationWidgetSnapshot& snapshot)
    {
        std::ostringstream stream;
        stream << "name='" << snapshot.name << "'"
               << ", type='" << snapshot.typeName << "'"
               << ", text='" << snapshot.text << "'"
               << ", tooltip='" << snapshot.tooltipText << "'"
               << ", visible=" << (snapshot.visible ? "true" : "false")
               << ", interactive=" << (snapshot.interactive ? "true" : "false")
               << ", enabled=" << (snapshot.enabled ? "true" : "false")
               << ", rect=" << snapshot.globalRect.x << ","
               << snapshot.globalRect.y << ","
               << snapshot.globalRect.width << "x"
               << snapshot.globalRect.height;
        return stream.str();
    }
}

EditorUIAutomationResult EditorUIAutomationResult::Success()
{
    EditorUIAutomationResult result;
    result.succeeded = true;
    return result;
}

EditorUIAutomationResult EditorUIAutomationResult::Failure(std::string reason)
{
    EditorUIAutomationResult result;
    result.succeeded = false;
    result.error = std::move(reason);
    return result;
}

EditorUIAutomationDriver::EditorUIAutomationDriver(EditorUIHost& host)
    : m_host(&host)
{
}

EditorUIAutomationResult EditorUIAutomationDriver::InspectWidget(
    const std::string& widgetName,
    EditorUIAutomationWidgetSnapshot& snapshot) const
{
    snapshot = {};
    if (!m_host)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation has no host");
    }

    UI::Widget::Ptr widget =
        m_host->GetUIContext().GetCanvas().FindWidget(widgetName);
    if (!widget)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation widget not found: " + widgetName);
    }

    snapshot = CaptureSnapshot(*widget);
    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorUIAutomationDriver::RequireWidgetVisible(
    const std::string& widgetName) const
{
    EditorUIAutomationWidgetSnapshot snapshot;
    EditorUIAutomationResult result = InspectWidget(widgetName, snapshot);
    if (!result)
    {
        return result;
    }
    if (!snapshot.visible || snapshot.globalRect.width <= 0.0f ||
        snapshot.globalRect.height <= 0.0f)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation widget is not visible: " +
            FormatWidgetSnapshot(snapshot));
    }
    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorUIAutomationDriver::RequireWidgetEnabled(
    const std::string& widgetName) const
{
    EditorUIAutomationWidgetSnapshot snapshot;
    EditorUIAutomationResult result = InspectWidget(widgetName, snapshot);
    if (!result)
    {
        return result;
    }
    if (!snapshot.visible || !snapshot.enabled)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation widget is not enabled: " +
            FormatWidgetSnapshot(snapshot));
    }
    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorUIAutomationDriver::RequireWidgetTextContains(
    const std::string& widgetName,
    const std::string& expectedText) const
{
    EditorUIAutomationWidgetSnapshot snapshot;
    EditorUIAutomationResult result = InspectWidget(widgetName, snapshot);
    if (!result)
    {
        return result;
    }
    if (snapshot.text.find(expectedText) == std::string::npos)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation widget text mismatch: expected '" +
            expectedText + "' in " + FormatWidgetSnapshot(snapshot));
    }
    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorUIAutomationDriver::ClickWidget(
    const std::string& widgetName)
{
    if (!m_host)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation has no host");
    }

    UI::UICanvas& canvas = m_host->GetUIContext().GetCanvas();
    UI::Widget::Ptr widget = canvas.FindWidget(widgetName);
    if (!widget)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation widget not found: " + widgetName);
    }

    const EditorUIAutomationWidgetSnapshot snapshot = CaptureSnapshot(*widget);
    if (!snapshot.visible || !snapshot.enabled)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation widget cannot be clicked: " +
            FormatWidgetSnapshot(snapshot));
    }

    const Vec2 center = widget->GetGlobalRect().Center();
    UI::Widget* hitWidget = canvas.HitTest(center);
    if (!IsWidgetOrDescendant(widget.get(), hitWidget))
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation widget center is intercepted by '" +
            FormatHitWidget(hitWidget) + "' while clicking: " +
            FormatWidgetSnapshot(snapshot));
    }
    const Vec2 inputPosition = center * canvas.GetScaleFactor();

    UI::UIEvent mouseDown;
    mouseDown.type = UI::UIEventType::MouseDown;
    mouseDown.position = inputPosition;
    mouseDown.button = static_cast<int>(UI::UIMouseButton::Left);
    if (!canvas.HandleEvent(mouseDown))
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation mouse down was not handled: " + widgetName);
    }

    UI::UIEvent mouseUp = mouseDown;
    mouseUp.type = UI::UIEventType::MouseUp;
    if (!canvas.HandleEvent(mouseUp))
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation mouse up was not handled: " + widgetName);
    }

    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorUIAutomationDriver::SendKeyDown(uint32 keyCode,
                                                               uint32 modifiers)
{
    if (!m_host)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation has no host");
    }

    UI::UIEvent keyDown;
    keyDown.type = UI::UIEventType::KeyDown;
    keyDown.keyCode = static_cast<int>(keyCode);
    keyDown.modifiers = modifiers;

    if (!m_host->GetUIContext().GetCanvas().HandleEvent(keyDown))
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation key down was not handled");
    }

    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorUIAutomationDriver::SendText(
    const std::string& text)
{
    if (!m_host)
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation has no host");
    }

    UI::UIEvent textInput;
    textInput.type = UI::UIEventType::TextInput;
    textInput.text = text;

    if (!m_host->GetUIContext().GetCanvas().HandleEvent(textInput))
    {
        return EditorUIAutomationResult::Failure(
            "Editor UI automation text input was not handled");
    }

    return EditorUIAutomationResult::Success();
}

EditorUIAutomationResult EditorUIAutomationDriver::ReplaceTextInput(
    const std::string& widgetName,
    const std::string& text)
{
    EditorUIAutomationResult result = ClickWidget(widgetName);
    if (!result)
    {
        return result;
    }

    result = SendKeyDown(static_cast<uint32>('A'),
                         UI::ToMask(UI::UIInputModifier::Ctrl));
    if (!result)
    {
        return result;
    }

    result = SendKeyDown(UI::RVX_UI_KEY_DELETE);
    if (!result)
    {
        return result;
    }

    return SendText(text);
}

} // namespace RVX::Editor
