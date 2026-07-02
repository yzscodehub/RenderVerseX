/**
 * @file EditorModalDialog.cpp
 * @brief Native editor modal dialog service implementation
 */

#include "Editor/UI/EditorModalDialog.h"

#include "Editor/UI/EditorPopupLayer.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "UI/UIContext.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_MODAL_ROOT = "Editor.ModalDialog";
    constexpr const char* RVX_EDITOR_MODAL_DEFAULT_BUTTON_ID = "ok";

    std::string SanitizeWidgetSegment(const std::string& text)
    {
        std::string result;
        result.reserve(text.size());
        for (unsigned char character : text)
        {
            if (std::isalnum(character) != 0 || character == '_' ||
                character == '-' || character == '.')
            {
                result.push_back(static_cast<char>(character));
            }
            else if (result.empty() || result.back() != '_')
            {
                result.push_back('_');
            }
        }

        while (!result.empty() && result.back() == '_')
        {
            result.pop_back();
        }
        return result.empty() ? "Default" : result;
    }

    std::string DialogWidgetPrefix(const EditorModalDialogDesc& desc)
    {
        return std::string(RVX_EDITOR_MODAL_ROOT) + "." +
               SanitizeWidgetSegment(desc.id);
    }

    UI::UIColor SeverityAccent(EditorModalDialogSeverity severity,
                               const UI::UITheme& theme)
    {
        switch (severity)
        {
            case EditorModalDialogSeverity::Warning:
                return theme.colors.warning;
            case EditorModalDialogSeverity::Error:
                return theme.colors.error;
            case EditorModalDialogSeverity::Question:
                return theme.colors.accent;
            case EditorModalDialogSeverity::Info:
            default:
                return theme.colors.accent;
        }
    }

    float EstimateTextWidth(const std::string& text,
                            const UI::UITheme& theme,
                            EditorTypographyRole role)
    {
        return EditorTypography::MeasureTextWidth(text, theme, role);
    }

    uint32 EstimateLineCount(const std::string& text,
                             float availableWidth,
                             const UI::UITheme& theme,
                             EditorTypographyRole role)
    {
        if (text.empty())
        {
            return 1u;
        }

        const EditorTypographyStyle style = EditorTypography::Resolve(theme, role);
        const float charactersPerLine =
            std::max(8.0f, availableWidth /
                               std::max(1.0f, style.fontSize * 0.56f));
        uint32 lines = 1u;
        uint32 currentLineLength = 0u;
        for (char character : text)
        {
            if (character == '\n')
            {
                ++lines;
                currentLineLength = 0u;
                continue;
            }

            ++currentLineLength;
            if (static_cast<float>(currentLineLength) >= charactersPerLine)
            {
                ++lines;
                currentLineLength = 0u;
            }
        }
        return lines;
    }

    float ClampDialogWidth(const EditorModalDialogDesc& desc,
                           const UI::UITheme& theme,
                           float surfaceWidth)
    {
        const float availableWidth = std::max(1.0f, surfaceWidth - 32.0f);
        const float maxWidth = std::clamp(desc.maxWidth, 1.0f, availableWidth);
        const float minWidth = std::clamp(desc.minWidth, 1.0f, maxWidth);
        float preferredWidth =
            std::max(EstimateTextWidth(desc.title,
                                       theme,
                                       EditorTypographyRole::DialogTitle) +
                         96.0f,
                     EstimateTextWidth(desc.message,
                                       theme,
                                       EditorTypographyRole::DialogBody) *
                             0.42f +
                         96.0f);
        preferredWidth = std::max(preferredWidth, minWidth);
        return std::clamp(preferredWidth, minWidth, maxWidth);
    }

    void ApplyModalButtonStyle(UI::Button& button,
                               const UI::UITheme& theme,
                               bool enabled,
                               bool isDefault)
    {
        button.GetStyle().fontSize = EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::Control);
        button.GetStyle().textColor = enabled ? theme.colors.text : theme.colors.textMuted;
        if (isDefault)
        {
            button.SetNormalColor(theme.colors.accent);
            button.SetHoverColor(theme.colors.accentHover);
            button.SetPressedColor(theme.colors.surfaceActive);
        }
        else
        {
            button.SetNormalColor(theme.colors.surface);
            button.SetHoverColor(theme.colors.surfaceHover);
            button.SetPressedColor(theme.colors.surfaceActive);
        }
        button.SetDisabledColor(theme.colors.surface.WithAlpha(0.35f));
        button.SetEnabled(enabled);
    }

    class EditorModalOverlayPanel final : public UI::Panel
    {
    public:
        EditorModalOverlayPanel(EditorModalDialog& dialog,
                                EditorUIHost& host,
                                UI::Rect dialogBounds,
                                bool closeOnOutsideClick)
            : m_dialog(dialog)
            , m_host(host)
            , m_dialogBounds(dialogBounds)
            , m_closeOnOutsideClick(closeOnOutsideClick)
        {
            SetInteractive(true);
        }

        bool HandleEvent(const UI::UIEvent& event) override
        {
            if (event.type == UI::UIEventType::KeyDown)
            {
                return m_dialog.HandleKeyDown(static_cast<uint32>(event.keyCode),
                                              m_host);
            }

            if (event.type == UI::UIEventType::MouseDown &&
                !m_dialogBounds.Contains(event.position))
            {
                if (m_closeOnOutsideClick)
                {
                    m_dialog.Close();
                }
                return true;
            }

            return UI::Panel::HandleEvent(event);
        }

    private:
        EditorModalDialog& m_dialog;
        EditorUIHost& m_host;
        UI::Rect m_dialogBounds;
        bool m_closeOnOutsideClick = false;
    };
}

EditorModalDialogButton EditorModalDialogButton::Action(
    std::string id,
    std::string text,
    EditorModalDialogAction action,
    bool enabled,
    bool closesDialog)
{
    EditorModalDialogButton button;
    button.id = std::move(id);
    button.text = std::move(text);
    button.action = std::move(action);
    button.enabled = enabled;
    button.closesDialog = closesDialog;
    return button;
}

void EditorModalDialog::Open(EditorModalDialogDesc desc)
{
    if (desc.id.empty())
    {
        desc.id = "Default";
    }

    m_desc = std::move(desc);
    NormalizeButtons();
    m_desc.defaultButtonId = ResolveDefaultButtonId();
    m_desc.cancelButtonId = ResolveCancelButtonId();
    m_selectedButtonIndex = FindButtonIndex(m_desc.defaultButtonId);
    if (m_selectedButtonIndex < 0)
    {
        m_selectedButtonIndex = FindFirstEnabledButtonIndex();
    }
    m_lastExecutedButtonId.clear();
    m_open = true;
}

void EditorModalDialog::Close()
{
    m_open = false;
}

void EditorModalDialog::Clear()
{
    m_desc = {};
    m_lastBuildStats = {};
    m_lastExecutedButtonId.clear();
    m_selectedButtonIndex = -1;
    m_open = false;
}

std::string EditorModalDialog::GetRootWidgetName() const
{
    return RVX_EDITOR_MODAL_ROOT;
}

void EditorModalDialog::Build(UI::UIContext& ui,
                              EditorPopupLayer& popupLayer,
                              EditorUIHost& host)
{
    m_lastBuildStats = {};
    if (!m_open)
    {
        return;
    }

    NormalizeSelection();

    const UI::UITheme& theme = ui.GetTheme();
    const float surfaceWidth = static_cast<float>(ui.GetWidth());
    const float surfaceHeight = static_cast<float>(ui.GetHeight());
    if (surfaceWidth <= 0.0f || surfaceHeight <= 0.0f)
    {
        return;
    }

    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.dialogPadding;
    const float spacing = shellMetrics.dialogSpacing;
    const float titleHeight = shellMetrics.dialogTitleHeight;
    const float buttonHeight = shellMetrics.dialogControlHeight;
    const float buttonSpacing = shellMetrics.dialogSpacing;
    const float dialogWidth = ClampDialogWidth(m_desc, theme, surfaceWidth);
    const float contentWidth = std::max(1.0f, dialogWidth - padding * 2.0f);
    const float messageLineHeight = shellMetrics.dialogBodyLineHeight;
    const uint32 messageLineCount = EstimateLineCount(
        m_desc.message,
        contentWidth,
        theme,
        EditorTypographyRole::DialogBody);
    const float messageHeight = std::max(messageLineHeight + 4.0f,
                                         static_cast<float>(messageLineCount) *
                                             messageLineHeight);
    const float dialogHeight =
        std::min(std::max(1.0f, surfaceHeight - 32.0f),
                 padding * 3.0f + titleHeight + spacing +
                     messageHeight + buttonHeight);
    const float dialogX = std::max(0.0f, (surfaceWidth - dialogWidth) * 0.5f);
    const float dialogY = std::max(0.0f, (surfaceHeight - dialogHeight) * 0.5f);
    const UI::Rect overlayBounds(0.0f, 0.0f, surfaceWidth, surfaceHeight);
    const UI::Rect dialogBounds(dialogX, dialogY, dialogWidth, dialogHeight);
    const std::string widgetPrefix = DialogWidgetPrefix(m_desc);

    std::shared_ptr<EditorModalOverlayPanel> overlay =
        std::make_shared<EditorModalOverlayPanel>(*this,
                                                  host,
                                                  dialogBounds,
                                                  m_desc.closeOnOutsideClick);
    overlay->SetName(RVX_EDITOR_MODAL_ROOT);
    overlay->SetPosition(overlayBounds.x, overlayBounds.y);
    overlay->SetSize(overlayBounds.width, overlayBounds.height);
    overlay->SetBackgroundColor(UI::UIColor(0.0f, 0.0f, 0.0f, 0.52f));
    overlay->SetBorderWidth(0.0f);

    UI::Panel::Ptr dialog = UI::Panel::Create();
    dialog->SetName(widgetPrefix + ".Panel");
    dialog->SetPosition(dialogX, dialogY);
    dialog->SetSize(dialogWidth, dialogHeight);
    dialog->SetBackgroundColor(theme.colors.panelBackground);
    dialog->SetBorderColor(theme.colors.border);
    dialog->SetBorderWidth(theme.metrics.borderWidth);
    dialog->SetClipChildren(true);

    UI::Panel::Ptr accent = UI::Panel::Create();
    accent->SetName(widgetPrefix + ".Accent");
    accent->SetPosition(0.0f, 0.0f);
    accent->SetSize(4.0f, dialogHeight);
    accent->SetBackgroundColor(SeverityAccent(m_desc.severity, theme));
    accent->SetBorderWidth(0.0f);
    accent->SetInteractive(false);
    dialog->AddChild(accent);

    UI::Label::Ptr title = UI::Label::Create(m_desc.title.empty() ? "Message"
                                                                  : m_desc.title);
    title->SetName(widgetPrefix + ".Title");
    title->SetPosition(padding, padding);
    title->SetSize(contentWidth, titleHeight);
    title->SetFontSize(EditorTypography::GetFontSize(
        theme,
        EditorTypographyRole::DialogTitle));
    title->SetTextColor(theme.colors.text);
    title->SetTextAlign(UI::TextAlign::Left);
    title->SetVerticalAlign(UI::VerticalAlign::Middle);
    dialog->AddChild(title);

    UI::Label::Ptr message = UI::Label::Create(m_desc.message);
    message->SetName(widgetPrefix + ".Message");
    message->SetPosition(padding, padding + titleHeight + spacing);
    message->SetSize(contentWidth, messageHeight);
    message->SetFontSize(EditorTypography::GetFontSize(
        theme,
        EditorTypographyRole::DialogBody));
    message->SetTextColor(theme.colors.textMuted);
    message->SetTextAlign(UI::TextAlign::Left);
    message->SetVerticalAlign(UI::VerticalAlign::Top);
    message->SetWordWrap(true);
    dialog->AddChild(message);

    const float buttonRowY = dialogHeight - padding - buttonHeight;
    float buttonRowWidth = 0.0f;
    for (const EditorModalDialogButton& button : m_desc.buttons)
    {
        buttonRowWidth += std::max(72.0f,
                                   EstimateTextWidth(button.text,
                                                     theme,
                                                     EditorTypographyRole::Control) +
                                       28.0f);
    }
    if (!m_desc.buttons.empty())
    {
        buttonRowWidth += buttonSpacing *
                          static_cast<float>(m_desc.buttons.size() - 1u);
    }

    float cursorX = std::max(padding, dialogWidth - padding - buttonRowWidth);
    for (const EditorModalDialogButton& buttonDesc : m_desc.buttons)
    {
        const float buttonWidth =
            std::max(72.0f,
                     EstimateTextWidth(buttonDesc.text,
                                       theme,
                                       EditorTypographyRole::Control) +
                         28.0f);
        UI::Button::Ptr button = UI::Button::Create(buttonDesc.text);
        button->SetName(widgetPrefix + ".Button." +
                        SanitizeWidgetSegment(buttonDesc.id));
        button->SetPosition(cursorX, buttonRowY);
        button->SetSize(buttonWidth, buttonHeight);
        const bool isDefault = buttonDesc.id == m_desc.defaultButtonId;
        ApplyModalButtonStyle(*button, theme, buttonDesc.enabled, isDefault);
        if (buttonDesc.enabled)
        {
            button->SetOnClick([this,
                                buttonId = buttonDesc.id,
                                hostPtr = &host](const UI::UIEvent& event) {
                (void)event;
                if (hostPtr)
                {
                    ExecuteButton(buttonId, *hostPtr);
                }
            });
        }
        dialog->AddChild(button);
        cursorX += buttonWidth + buttonSpacing;
    }

    overlay->AddChild(dialog);
    popupLayer.AddPopup(overlay);

    m_lastBuildStats.open = true;
    m_lastBuildStats.buttonCount = static_cast<uint32>(m_desc.buttons.size());
    m_lastBuildStats.selectedButtonIndex = m_selectedButtonIndex;
    m_lastBuildStats.defaultButtonId = m_desc.defaultButtonId;
    m_lastBuildStats.cancelButtonId = m_desc.cancelButtonId;
    m_lastBuildStats.overlayBounds = overlayBounds;
    m_lastBuildStats.dialogBounds = dialogBounds;
    for (const EditorModalDialogButton& button : m_desc.buttons)
    {
        if (button.enabled)
        {
            ++m_lastBuildStats.enabledButtonCount;
        }
    }
}

bool EditorModalDialog::HandleKeyDown(uint32 keyCode, EditorUIHost& host)
{
    if (!m_open)
    {
        return false;
    }

    if (keyCode == UI::RVX_UI_KEY_ESCAPE)
    {
        if (!m_desc.closeOnEscape)
        {
            return true;
        }

        if (!m_desc.cancelButtonId.empty())
        {
            ExecuteButton(m_desc.cancelButtonId, host);
        }
        else
        {
            Close();
        }
        return true;
    }

    if (keyCode == UI::RVX_UI_KEY_LEFT || keyCode == UI::RVX_UI_KEY_UP)
    {
        MoveSelection(-1);
        return true;
    }

    if (keyCode == UI::RVX_UI_KEY_RIGHT || keyCode == UI::RVX_UI_KEY_DOWN)
    {
        MoveSelection(1);
        return true;
    }

    if (keyCode == UI::RVX_UI_KEY_ENTER || keyCode == UI::RVX_UI_KEY_SPACE)
    {
        NormalizeSelection();
        if (m_selectedButtonIndex >= 0)
        {
            return ExecuteButton(
                m_desc.buttons[static_cast<size_t>(m_selectedButtonIndex)].id,
                host);
        }
        return true;
    }

    return false;
}

bool EditorModalDialog::ExecuteButton(const std::string& buttonId,
                                      EditorUIHost& host)
{
    const int32 buttonIndex = FindButtonIndex(buttonId);
    if (buttonIndex < 0)
    {
        return false;
    }

    EditorModalDialogButton& button =
        m_desc.buttons[static_cast<size_t>(buttonIndex)];
    if (!button.enabled)
    {
        return false;
    }

    m_selectedButtonIndex = buttonIndex;
    m_lastExecutedButtonId = button.id;
    EditorModalDialogAction action = button.action;
    const bool closesDialog = button.closesDialog;
    if (closesDialog)
    {
        Close();
    }
    if (action)
    {
        action(host);
    }
    return true;
}

void EditorModalDialog::NormalizeButtons()
{
    if (m_desc.buttons.empty())
    {
        m_desc.buttons.push_back(
            EditorModalDialogButton::Action(RVX_EDITOR_MODAL_DEFAULT_BUTTON_ID,
                                            "OK",
                                            {}));
    }

    uint32 unnamedIndex = 0;
    for (EditorModalDialogButton& button : m_desc.buttons)
    {
        if (button.id.empty())
        {
            button.id = "button" + std::to_string(unnamedIndex++);
        }
        if (button.text.empty())
        {
            button.text = button.id;
        }
    }
}

void EditorModalDialog::NormalizeSelection()
{
    if (m_selectedButtonIndex >= 0 &&
        static_cast<size_t>(m_selectedButtonIndex) < m_desc.buttons.size() &&
        m_desc.buttons[static_cast<size_t>(m_selectedButtonIndex)].enabled)
    {
        return;
    }

    m_selectedButtonIndex = FindButtonIndex(m_desc.defaultButtonId);
    if (m_selectedButtonIndex < 0 ||
        !m_desc.buttons[static_cast<size_t>(m_selectedButtonIndex)].enabled)
    {
        m_selectedButtonIndex = FindFirstEnabledButtonIndex();
    }
}

bool EditorModalDialog::MoveSelection(int32 delta)
{
    if (m_desc.buttons.empty())
    {
        m_selectedButtonIndex = -1;
        return false;
    }

    NormalizeSelection();
    const int32 count = static_cast<int32>(m_desc.buttons.size());
    int32 cursor = m_selectedButtonIndex;
    if (cursor < 0)
    {
        cursor = delta < 0 ? 0 : count - 1;
    }

    for (int32 step = 0; step < count; ++step)
    {
        cursor = (cursor + delta + count) % count;
        if (m_desc.buttons[static_cast<size_t>(cursor)].enabled)
        {
            const bool changed = cursor != m_selectedButtonIndex;
            m_selectedButtonIndex = cursor;
            return changed;
        }
    }

    m_selectedButtonIndex = -1;
    return false;
}

int32 EditorModalDialog::FindButtonIndex(const std::string& buttonId) const
{
    if (buttonId.empty())
    {
        return -1;
    }

    for (size_t index = 0; index < m_desc.buttons.size(); ++index)
    {
        if (m_desc.buttons[index].id == buttonId)
        {
            return static_cast<int32>(index);
        }
    }
    return -1;
}

int32 EditorModalDialog::FindFirstEnabledButtonIndex() const
{
    for (size_t index = 0; index < m_desc.buttons.size(); ++index)
    {
        if (m_desc.buttons[index].enabled)
        {
            return static_cast<int32>(index);
        }
    }
    return -1;
}

std::string EditorModalDialog::ResolveDefaultButtonId() const
{
    const int32 requestedDefaultIndex = FindButtonIndex(m_desc.defaultButtonId);
    if (requestedDefaultIndex >= 0 &&
        m_desc.buttons[static_cast<size_t>(requestedDefaultIndex)].enabled)
    {
        return m_desc.defaultButtonId;
    }

    const int32 firstEnabledIndex = FindFirstEnabledButtonIndex();
    return firstEnabledIndex >= 0
               ? m_desc.buttons[static_cast<size_t>(firstEnabledIndex)].id
               : std::string();
}

std::string EditorModalDialog::ResolveCancelButtonId() const
{
    if (FindButtonIndex(m_desc.cancelButtonId) >= 0)
    {
        return m_desc.cancelButtonId;
    }

    for (const EditorModalDialogButton& button : m_desc.buttons)
    {
        std::string lowerId = button.id;
        std::transform(lowerId.begin(),
                       lowerId.end(),
                       lowerId.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (lowerId == "cancel" || lowerId == "no")
        {
            return button.id;
        }
    }

    return {};
}

} // namespace RVX::Editor
