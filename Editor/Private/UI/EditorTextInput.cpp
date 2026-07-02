/**
 * @file EditorTextInput.cpp
 * @brief Native editor single-line text input widget implementation
 */

#include "Editor/UI/EditorTextInput.h"

#include "Editor/UI/EditorTypography.h"
#include "UI/UIClipboard.h"
#include "UI/UIRenderer.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <vector>

namespace RVX::Editor
{
namespace
{
    bool IsControlCode(char value)
    {
        const unsigned char code = static_cast<unsigned char>(value);
        return code < 32u && value != '\t';
    }

    bool IsWordByte(char value)
    {
        const unsigned char code = static_cast<unsigned char>(value);
        return std::isalnum(code) != 0 || value == '_';
    }

    uint32 NormalizeAsciiKey(int keyCode)
    {
        if (keyCode >= 'a' && keyCode <= 'z')
        {
            return static_cast<uint32>('A' + (keyCode - 'a'));
        }
        return static_cast<uint32>(keyCode);
    }

    size_t GetUTF8SequenceLength(const std::string& text, size_t index)
    {
        if (index >= text.size())
        {
            return 0u;
        }

        const unsigned char lead =
            static_cast<unsigned char>(text[index]);
        if (lead < 0x80u)
        {
            return 1u;
        }

        size_t length = 1u;
        if ((lead & 0xE0u) == 0xC0u)
        {
            length = 2u;
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            length = 3u;
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            length = 4u;
        }

        if (index + length > text.size())
        {
            return 1u;
        }

        for (size_t offset = 1u; offset < length; ++offset)
        {
            const unsigned char next =
                static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0u) != 0x80u)
            {
                return 1u;
            }
        }
        return length;
    }

    std::vector<size_t> BuildUTF8StartOffsets(const std::string& text)
    {
        std::vector<size_t> offsets;
        offsets.reserve(text.size() + 1u);

        size_t index = 0u;
        while (index < text.size())
        {
            offsets.push_back(index);
            index += std::max<size_t>(1u, GetUTF8SequenceLength(text, index));
        }
        offsets.push_back(text.size());
        return offsets;
    }

    size_t ClampToUTF8Boundary(const std::string& text, size_t offset)
    {
        if (offset >= text.size())
        {
            return text.size();
        }

        while (offset > 0u &&
               (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u)
        {
            --offset;
        }
        return offset;
    }

    size_t PreviousUTF8Boundary(const std::string& text, size_t offset)
    {
        if (offset == 0u || text.empty())
        {
            return 0u;
        }

        offset = std::min(offset, text.size()) - 1u;
        while (offset > 0u &&
               (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u)
        {
            --offset;
        }
        return offset;
    }

    size_t NextUTF8Boundary(const std::string& text, size_t offset)
    {
        offset = ClampToUTF8Boundary(text, offset);
        if (offset >= text.size())
        {
            return text.size();
        }

        return std::min(text.size(),
                        offset + std::max<size_t>(
                                     1u,
                                     GetUTF8SequenceLength(text, offset)));
    }
}

EditorTextInput::EditorTextInput()
{
    SetInteractive(true);
    SetTabStop(true);
}

void EditorTextInput::SetText(const std::string& text)
{
    if (m_text == text)
    {
        return;
    }

    m_text = text;
    m_caretByteOffset = m_text.size();
    m_selectionAnchorByteOffset = m_caretByteOffset;
    m_scrollByteOffset = 0u;
    MarkLayoutDirty();
    NotifyTextChanged();
}

void EditorTextInput::ApplyTheme(const UI::UITheme& theme)
{
    m_backgroundColor = theme.colors.windowBackground;
    m_hoverColor = theme.colors.surfaceHover;
    m_focusColor = theme.colors.surface;
    m_borderColor = theme.colors.border;
    m_focusBorderColor = theme.colors.accent;
    m_textColor = theme.colors.text;
    m_placeholderColor = theme.colors.textMuted;
    m_fontSize = EditorTypography::GetFontSize(theme, EditorTypographyRole::Input);
}

bool EditorTextInput::HasSelection() const
{
    return m_caretByteOffset != m_selectionAnchorByteOffset;
}

size_t EditorTextInput::GetSelectionStartByteOffset() const
{
    return std::min(m_caretByteOffset, m_selectionAnchorByteOffset);
}

size_t EditorTextInput::GetSelectionEndByteOffset() const
{
    return std::max(m_caretByteOffset, m_selectionAnchorByteOffset);
}

std::string EditorTextInput::GetSelectedText() const
{
    if (!HasSelection())
    {
        return {};
    }

    const size_t selectionStart =
        ClampToUTF8Boundary(m_text, GetSelectionStartByteOffset());
    const size_t selectionEnd =
        ClampToUTF8Boundary(m_text, GetSelectionEndByteOffset());
    if (selectionEnd <= selectionStart)
    {
        return {};
    }

    return m_text.substr(selectionStart, selectionEnd - selectionStart);
}

void EditorTextInput::OnRender(UI::UIRenderer& renderer)
{
    const UI::Rect bounds = GetGlobalRect();
    const UI::UIColor background = IsFocused() ? m_focusColor
                                  : IsHovered() ? m_hoverColor
                                                : m_backgroundColor;
    renderer.DrawRect(bounds, background);
    renderer.DrawBorder(bounds,
                        IsFocused() ? m_focusBorderColor : m_borderColor,
                        IsFocused() ? 2.0f : 1.0f);

    const float horizontalPadding = 7.0f;
    const UI::Rect textBounds(bounds.x + horizontalPadding,
                              bounds.y,
                              std::max(0.0f, bounds.width - horizontalPadding * 2.0f),
                              bounds.height);

    std::string text;
    UI::UIColor textColor = m_text.empty() ? m_placeholderColor : m_textColor;
    if (m_text.empty())
    {
        m_caretByteOffset = 0u;
        m_scrollByteOffset = 0u;
        text = m_placeholder;
    }
    else
    {
        EnsureCaretVisible(renderer, textBounds.width);
        text = m_text.substr(std::min(m_scrollByteOffset, m_text.size()));
    }

    renderer.PushClipRect(textBounds);
    if (!m_text.empty() && HasSelection())
    {
        const size_t selectionStart =
            ClampToUTF8Boundary(m_text, GetSelectionStartByteOffset());
        const size_t selectionEnd =
            ClampToUTF8Boundary(m_text, GetSelectionEndByteOffset());
        const size_t visibleStart =
            std::min(m_scrollByteOffset, m_text.size());
        const size_t visibleSelectionStart =
            std::clamp(selectionStart, visibleStart, m_text.size());
        const size_t visibleSelectionEnd =
            std::clamp(selectionEnd, visibleStart, m_text.size());
        if (visibleSelectionEnd > visibleSelectionStart)
        {
            const std::string beforeSelection =
                m_text.substr(visibleStart,
                              visibleSelectionStart - visibleStart);
            const std::string selected =
                m_text.substr(visibleSelectionStart,
                              visibleSelectionEnd - visibleSelectionStart);
            const float selectionX = textBounds.x +
                renderer.MeasureText(beforeSelection, m_fontSize).width;
            const float selectionWidth =
                renderer.MeasureText(selected, m_fontSize).width;
            const float selectionHeight = std::max(12.0f, m_fontSize + 5.0f);
            const float selectionY = textBounds.y +
                std::max(0.0f, (textBounds.height - selectionHeight) * 0.5f);
            renderer.DrawRect(
                UI::Rect(selectionX,
                         selectionY,
                         selectionWidth,
                         std::min(selectionHeight, textBounds.height)),
                UI::UIColor{m_focusBorderColor.r,
                            m_focusBorderColor.g,
                            m_focusBorderColor.b,
                            0.45f});
        }
    }
    renderer.DrawText(text,
                      textBounds,
                      m_fontSize,
                      textColor,
                      UI::TextAlign::Left,
                      UI::VerticalAlign::Middle);
    if (IsFocused())
    {
        const size_t caretOffset =
            std::clamp(m_caretByteOffset, m_scrollByteOffset, m_text.size());
        const std::string beforeCaret =
            m_text.substr(m_scrollByteOffset, caretOffset - m_scrollByteOffset);
        const float caretX = textBounds.x +
            renderer.MeasureText(beforeCaret, m_fontSize).width;
        const float caretHeight = std::max(12.0f, m_fontSize + 4.0f);
        const float caretY = textBounds.y +
            std::max(0.0f, (textBounds.height - caretHeight) * 0.5f);
        renderer.DrawRect(UI::Rect(caretX,
                                   caretY,
                                   1.5f,
                                   std::min(caretHeight, textBounds.height)),
                          m_textColor);
    }
    renderer.PopClipRect();
}

bool EditorTextInput::HandleEvent(const UI::UIEvent& event)
{
    if (event.type == UI::UIEventType::MouseDown &&
        event.button == static_cast<int>(UI::UIMouseButton::Left))
    {
        const bool shift =
            (event.modifiers & UI::ToMask(UI::UIInputModifier::Shift)) != 0u;
        MoveCaretTo(ByteOffsetFromMousePosition(event.position.x), shift);
        m_dragSelecting = true;
        UI::Widget::HandleEvent(event);
        return true;
    }

    if (event.type == UI::UIEventType::MouseMove && m_dragSelecting)
    {
        MoveCaretTo(ByteOffsetFromMousePosition(event.position.x), true);
        return true;
    }

    if (event.type == UI::UIEventType::MouseUp &&
        event.button == static_cast<int>(UI::UIMouseButton::Left))
    {
        m_dragSelecting = false;
        UI::Widget::HandleEvent(event);
        return true;
    }

    if (event.type == UI::UIEventType::TextInput)
    {
        if (!IsFocused())
        {
            return false;
        }
        AppendText(event.text);
        return true;
    }

    if (event.type == UI::UIEventType::KeyDown)
    {
        if (!IsFocused())
        {
            return false;
        }

        const bool ctrl =
            (event.modifiers & UI::ToMask(UI::UIInputModifier::Ctrl)) != 0u;
        const bool shift =
            (event.modifiers & UI::ToMask(UI::UIInputModifier::Shift)) != 0u;
        const uint32 asciiKey = NormalizeAsciiKey(event.keyCode);
        if (ctrl && asciiKey == static_cast<uint32>('A'))
        {
            SelectAll();
            return true;
        }
        if (ctrl && asciiKey == static_cast<uint32>('C'))
        {
            CopySelectionToClipboard();
            return true;
        }
        if (ctrl && asciiKey == static_cast<uint32>('X'))
        {
            CutSelectionToClipboard();
            return true;
        }
        if (ctrl && asciiKey == static_cast<uint32>('V'))
        {
            PasteFromClipboard();
            return true;
        }

        if (event.keyCode == static_cast<int>(UI::RVX_UI_KEY_BACKSPACE))
        {
            Backspace();
            return true;
        }
        if (event.keyCode == static_cast<int>(UI::RVX_UI_KEY_LEFT))
        {
            MoveCaretLeft(shift, ctrl);
            return true;
        }
        if (event.keyCode == static_cast<int>(UI::RVX_UI_KEY_RIGHT))
        {
            MoveCaretRight(shift, ctrl);
            return true;
        }
        if (event.keyCode == static_cast<int>(UI::RVX_UI_KEY_HOME))
        {
            MoveCaretHome(shift);
            return true;
        }
        if (event.keyCode == static_cast<int>(UI::RVX_UI_KEY_END))
        {
            MoveCaretEnd(shift);
            return true;
        }
        if (event.keyCode == static_cast<int>(UI::RVX_UI_KEY_DELETE))
        {
            DeleteForward();
            return true;
        }
        if (m_onUnhandledKeyDown &&
            m_onUnhandledKeyDown(static_cast<uint32>(event.keyCode)))
        {
            return true;
        }
    }

    return UI::Widget::HandleEvent(event);
}

void EditorTextInput::NotifyTextChanged()
{
    if (m_onTextChanged)
    {
        m_onTextChanged(m_text);
    }
}

void EditorTextInput::AppendText(const std::string& text)
{
    std::string filtered;
    filtered.reserve(text.size());
    for (char value : text)
    {
        if (!IsControlCode(value))
        {
            filtered.push_back(value);
        }
    }

    if (filtered.empty())
    {
        return;
    }

    DeleteSelection(false);
    const size_t insertAt = ClampToUTF8Boundary(m_text, m_caretByteOffset);
    m_text.insert(insertAt, filtered);
    m_caretByteOffset = insertAt + filtered.size();
    m_selectionAnchorByteOffset = m_caretByteOffset;
    MarkLayoutDirty();
    NotifyTextChanged();
}

void EditorTextInput::Backspace()
{
    if (DeleteSelection())
    {
        return;
    }

    if (m_text.empty())
    {
        return;
    }

    m_caretByteOffset = ClampToUTF8Boundary(m_text, m_caretByteOffset);
    if (m_caretByteOffset == 0u)
    {
        return;
    }

    const size_t eraseFrom = PreviousUTF8Boundary(m_text, m_caretByteOffset);
    m_text.erase(eraseFrom, m_caretByteOffset - eraseFrom);
    m_caretByteOffset = eraseFrom;
    m_selectionAnchorByteOffset = m_caretByteOffset;
    m_scrollByteOffset = std::min(m_scrollByteOffset, m_text.size());
    MarkLayoutDirty();
    NotifyTextChanged();
}

void EditorTextInput::DeleteForward()
{
    if (DeleteSelection())
    {
        return;
    }

    if (m_text.empty())
    {
        return;
    }

    m_caretByteOffset = ClampToUTF8Boundary(m_text, m_caretByteOffset);
    if (m_caretByteOffset >= m_text.size())
    {
        return;
    }

    const size_t eraseTo = NextUTF8Boundary(m_text, m_caretByteOffset);
    m_text.erase(m_caretByteOffset, eraseTo - m_caretByteOffset);
    m_selectionAnchorByteOffset = m_caretByteOffset;
    m_scrollByteOffset = std::min(m_scrollByteOffset, m_text.size());
    MarkLayoutDirty();
    NotifyTextChanged();
}

void EditorTextInput::EnsureCaretVisible(UI::UIRenderer& renderer,
                                         float visibleWidth)
{
    if (m_text.empty() || visibleWidth <= 0.0f)
    {
        m_scrollByteOffset = 0u;
        return;
    }

    m_caretByteOffset = ClampToUTF8Boundary(m_text, m_caretByteOffset);
    m_scrollByteOffset = ClampToUTF8Boundary(m_text, m_scrollByteOffset);
    if (m_caretByteOffset < m_scrollByteOffset)
    {
        m_scrollByteOffset = m_caretByteOffset;
        return;
    }

    if (renderer.MeasureText(m_text, m_fontSize).width <= visibleWidth + 0.5f)
    {
        m_scrollByteOffset = 0u;
        return;
    }

    const std::string currentVisibleBeforeCaret =
        m_text.substr(m_scrollByteOffset,
                      m_caretByteOffset - m_scrollByteOffset);
    if (renderer.MeasureText(currentVisibleBeforeCaret, m_fontSize).width <=
        visibleWidth - 1.5f + 0.5f)
    {
        return;
    }

    const std::vector<size_t> offsets = BuildUTF8StartOffsets(m_text);
    if (offsets.size() <= 1u)
    {
        m_scrollByteOffset = 0u;
        return;
    }

    const auto caretIt = std::find(offsets.begin(), offsets.end(), m_caretByteOffset);
    const size_t maxStartIndex =
        caretIt == offsets.end()
            ? offsets.size() - 1u
            : static_cast<size_t>(std::distance(offsets.begin(), caretIt));
    auto fits = [this, &renderer, visibleWidth, &offsets](size_t offsetIndex) {
        const std::string candidate =
            m_text.substr(offsets[offsetIndex],
                          m_caretByteOffset - offsets[offsetIndex]);
        return renderer.MeasureText(candidate, m_fontSize).width <= visibleWidth + 0.5f;
    };

    size_t low = 0u;
    size_t high = maxStartIndex;
    while (low < high)
    {
        const size_t mid = (low + high) / 2u;
        if (fits(mid))
        {
            high = mid;
        }
        else
        {
            low = mid + 1u;
        }
    }

    m_scrollByteOffset = offsets[low];
}

void EditorTextInput::MoveCaretLeft(bool extendSelection, bool word)
{
    if (!extendSelection && HasSelection())
    {
        MoveCaretTo(GetSelectionStartByteOffset(), false);
        return;
    }

    MoveCaretTo(word ? PreviousWordBoundary(m_caretByteOffset)
                     : PreviousUTF8Boundary(m_text, m_caretByteOffset),
                extendSelection);
}

void EditorTextInput::MoveCaretRight(bool extendSelection, bool word)
{
    if (!extendSelection && HasSelection())
    {
        MoveCaretTo(GetSelectionEndByteOffset(), false);
        return;
    }

    MoveCaretTo(word ? NextWordBoundary(m_caretByteOffset)
                     : NextUTF8Boundary(m_text, m_caretByteOffset),
                extendSelection);
}

void EditorTextInput::MoveCaretHome(bool extendSelection)
{
    MoveCaretTo(0u, extendSelection);
}

void EditorTextInput::MoveCaretEnd(bool extendSelection)
{
    MoveCaretTo(m_text.size(), extendSelection);
}

void EditorTextInput::MoveCaretTo(size_t offset, bool extendSelection)
{
    const size_t previousCaret = m_caretByteOffset;
    m_caretByteOffset = ClampToUTF8Boundary(m_text, offset);
    if (extendSelection)
    {
        if (!HasSelection())
        {
            m_selectionAnchorByteOffset = previousCaret;
        }
    }
    else
    {
        m_selectionAnchorByteOffset = m_caretByteOffset;
    }

    if (m_caretByteOffset < m_scrollByteOffset)
    {
        m_scrollByteOffset = m_caretByteOffset;
    }
    MarkLayoutDirty();
}

void EditorTextInput::SelectAll()
{
    m_selectionAnchorByteOffset = 0u;
    m_caretByteOffset = m_text.size();
    MarkLayoutDirty();
}

bool EditorTextInput::DeleteSelection(bool notify)
{
    if (!HasSelection())
    {
        return false;
    }

    const size_t selectionStart =
        ClampToUTF8Boundary(m_text, GetSelectionStartByteOffset());
    const size_t selectionEnd =
        ClampToUTF8Boundary(m_text, GetSelectionEndByteOffset());
    if (selectionEnd <= selectionStart)
    {
        ClearSelection();
        return false;
    }

    m_text.erase(selectionStart, selectionEnd - selectionStart);
    m_caretByteOffset = selectionStart;
    m_selectionAnchorByteOffset = m_caretByteOffset;
    m_scrollByteOffset = std::min(m_scrollByteOffset, m_text.size());
    MarkLayoutDirty();
    if (notify)
    {
        NotifyTextChanged();
    }
    return true;
}

void EditorTextInput::ClearSelection()
{
    m_selectionAnchorByteOffset = m_caretByteOffset;
    MarkLayoutDirty();
}

void EditorTextInput::CopySelectionToClipboard()
{
    if (HasSelection())
    {
        UI::UIClipboard::SetText(GetSelectedText());
    }
}

void EditorTextInput::CutSelectionToClipboard()
{
    if (!HasSelection())
    {
        return;
    }

    UI::UIClipboard::SetText(GetSelectedText());
    DeleteSelection();
}

void EditorTextInput::PasteFromClipboard()
{
    std::string clipboardText;
    if (UI::UIClipboard::GetText(clipboardText) && !clipboardText.empty())
    {
        AppendText(clipboardText);
    }
}

size_t EditorTextInput::PreviousWordBoundary(size_t offset) const
{
    offset = ClampToUTF8Boundary(m_text, offset);
    if (offset == 0u)
    {
        return 0u;
    }

    std::vector<size_t> offsets = BuildUTF8StartOffsets(m_text);
    auto it = std::lower_bound(offsets.begin(), offsets.end(), offset);
    if (it == offsets.end() || *it >= offset)
    {
        if (it == offsets.begin())
        {
            return 0u;
        }
        --it;
    }

    while (it != offsets.begin() && !IsWordByte(m_text[*it]))
    {
        --it;
    }
    while (it != offsets.begin())
    {
        auto previous = it;
        --previous;
        if (!IsWordByte(m_text[*previous]))
        {
            break;
        }
        it = previous;
    }
    return *it;
}

size_t EditorTextInput::NextWordBoundary(size_t offset) const
{
    offset = ClampToUTF8Boundary(m_text, offset);
    if (offset >= m_text.size())
    {
        return m_text.size();
    }

    std::vector<size_t> offsets = BuildUTF8StartOffsets(m_text);
    auto it = std::lower_bound(offsets.begin(), offsets.end(), offset);
    if (it == offsets.end())
    {
        return m_text.size();
    }

    if (it != offsets.end() && IsWordByte(m_text[*it]))
    {
        while (it != offsets.end() && *it < m_text.size() &&
               IsWordByte(m_text[*it]))
        {
            ++it;
        }
    }
    while (it != offsets.end() && *it < m_text.size() &&
           !IsWordByte(m_text[*it]))
    {
        ++it;
    }

    return it == offsets.end() ? m_text.size() : *it;
}

size_t EditorTextInput::ByteOffsetFromMousePosition(float mouseX) const
{
    if (m_text.empty())
    {
        return 0u;
    }

    const float horizontalPadding = 7.0f;
    const UI::Rect bounds = GetGlobalRect();
    const float localX = mouseX - (bounds.x + horizontalPadding);
    const size_t visibleStart =
        ClampToUTF8Boundary(m_text, m_scrollByteOffset);
    if (localX <= 0.0f)
    {
        return visibleStart;
    }

    const std::vector<size_t> offsets = BuildUTF8StartOffsets(m_text);
    auto it = std::lower_bound(offsets.begin(), offsets.end(), visibleStart);
    if (it == offsets.end())
    {
        return m_text.size();
    }

    size_t previousOffset = *it;
    float previousWidth = 0.0f;
    if (previousOffset >= m_text.size())
    {
        return m_text.size();
    }

    ++it;
    for (; it != offsets.end(); ++it)
    {
        const size_t currentOffset = *it;
        const std::string beforeCaret =
            m_text.substr(visibleStart, currentOffset - visibleStart);
        const float currentWidth =
            UI::UIFontFallbackChain::Default()
                .MeasureText(beforeCaret, m_fontSize)
                .width;
        const float midpoint = (previousWidth + currentWidth) * 0.5f;
        if (localX < midpoint)
        {
            return previousOffset;
        }

        previousOffset = currentOffset;
        previousWidth = currentWidth;
    }

    return m_text.size();
}

} // namespace RVX::Editor
