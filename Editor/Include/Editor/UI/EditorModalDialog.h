/**
 * @file EditorModalDialog.h
 * @brief Native editor modal dialog service
 */

#pragma once

#include "Core/Types.h"
#include "UI/UITypes.h"

#include <functional>
#include <string>
#include <vector>

namespace RVX::UI
{
    class UIContext;
}

namespace RVX::Editor
{

class EditorPopupLayer;
class EditorUIHost;

enum class EditorModalDialogSeverity : uint8
{
    Info = 0,
    Warning,
    Error,
    Question
};

using EditorModalDialogAction = std::function<void(EditorUIHost&)>;

struct EditorModalDialogButton
{
    std::string id;
    std::string text;
    EditorModalDialogAction action;
    bool enabled = true;
    bool closesDialog = true;

    static EditorModalDialogButton Action(std::string id,
                                          std::string text,
                                          EditorModalDialogAction action,
                                          bool enabled = true,
                                          bool closesDialog = true);
};

struct EditorModalDialogDesc
{
    std::string id = "Default";
    std::string title;
    std::string message;
    EditorModalDialogSeverity severity = EditorModalDialogSeverity::Info;
    std::vector<EditorModalDialogButton> buttons;
    std::string defaultButtonId;
    std::string cancelButtonId;
    float minWidth = 320.0f;
    float maxWidth = 560.0f;
    bool closeOnEscape = true;
    bool closeOnOutsideClick = false;
    bool focusOnOpen = true;
};

struct EditorModalDialogStats
{
    bool open = false;
    uint32 buttonCount = 0;
    uint32 enabledButtonCount = 0;
    int32 selectedButtonIndex = -1;
    std::string defaultButtonId;
    std::string cancelButtonId;
    UI::Rect overlayBounds;
    UI::Rect dialogBounds;
};

/**
 * @brief Builds native modal dialogs through the editor popup layer.
 */
class EditorModalDialog
{
public:
    void Open(EditorModalDialogDesc desc);
    void Close();
    void Clear();

    bool IsOpen() const { return m_open; }
    const std::string& GetOpenDialogId() const { return m_desc.id; }
    std::string GetRootWidgetName() const;
    bool WantsKeyboardFocus() const { return m_open && m_desc.focusOnOpen; }

    void Build(UI::UIContext& ui, EditorPopupLayer& popupLayer, EditorUIHost& host);
    bool HandleKeyDown(uint32 keyCode, EditorUIHost& host);
    bool ExecuteButton(const std::string& buttonId, EditorUIHost& host);

    const std::string& GetLastExecutedButtonId() const
    {
        return m_lastExecutedButtonId;
    }

    const EditorModalDialogStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    void NormalizeButtons();
    void NormalizeSelection();
    bool MoveSelection(int32 delta);
    int32 FindButtonIndex(const std::string& buttonId) const;
    int32 FindFirstEnabledButtonIndex() const;
    std::string ResolveDefaultButtonId() const;
    std::string ResolveCancelButtonId() const;

    EditorModalDialogDesc m_desc;
    EditorModalDialogStats m_lastBuildStats;
    std::string m_lastExecutedButtonId;
    int32 m_selectedButtonIndex = -1;
    bool m_open = false;
};

} // namespace RVX::Editor
