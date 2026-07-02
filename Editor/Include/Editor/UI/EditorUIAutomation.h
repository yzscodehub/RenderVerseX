/**
 * @file EditorUIAutomation.h
 * @brief Deterministic native editor UI event automation helpers
 */

#pragma once

#include "Core/Types.h"
#include "UI/UITypes.h"

#include <string>

namespace RVX::Editor
{

class EditorUIHost;

struct EditorUIAutomationResult
{
    bool succeeded = false;
    std::string error;

    static EditorUIAutomationResult Success();
    static EditorUIAutomationResult Failure(std::string reason);

    explicit operator bool() const { return succeeded; }
};

struct EditorUIAutomationWidgetSnapshot
{
    bool found = false;
    std::string name;
    std::string typeName;
    std::string text;
    std::string tooltipText;
    UI::Rect globalRect;
    bool visible = false;
    bool interactive = false;
    bool enabled = false;
    bool focused = false;
};

/**
 * @brief Drives named native editor widgets through the real UI event path.
 */
class EditorUIAutomationDriver
{
public:
    explicit EditorUIAutomationDriver(EditorUIHost& host);

    EditorUIAutomationResult InspectWidget(
        const std::string& widgetName,
        EditorUIAutomationWidgetSnapshot& snapshot) const;
    EditorUIAutomationResult RequireWidgetVisible(
        const std::string& widgetName) const;
    EditorUIAutomationResult RequireWidgetEnabled(
        const std::string& widgetName) const;
    EditorUIAutomationResult RequireWidgetTextContains(
        const std::string& widgetName,
        const std::string& expectedText) const;
    EditorUIAutomationResult ClickWidget(const std::string& widgetName);
    EditorUIAutomationResult SendKeyDown(uint32 keyCode, uint32 modifiers = 0u);
    EditorUIAutomationResult SendText(const std::string& text);
    EditorUIAutomationResult ReplaceTextInput(const std::string& widgetName,
                                              const std::string& text);

private:
    EditorUIHost* m_host = nullptr;
};

} // namespace RVX::Editor
