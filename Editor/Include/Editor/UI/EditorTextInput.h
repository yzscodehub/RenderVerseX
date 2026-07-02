/**
 * @file EditorTextInput.h
 * @brief Native editor single-line text input widget
 */

#pragma once

#include "UI/UIContext.h"
#include "UI/Widget.h"

#include <functional>
#include <memory>
#include <string>

namespace RVX::Editor
{

class EditorTextInput final : public UI::Widget
{
public:
    using Ptr = std::shared_ptr<EditorTextInput>;
    using TextChangedCallback = std::function<void(const std::string&)>;
    using KeyDownCallback = std::function<bool(uint32)>;

    EditorTextInput();

    const char* GetTypeName() const override { return "EditorTextInput"; }

    const std::string& GetText() const { return m_text; }
    void SetText(const std::string& text);
    size_t GetCaretByteOffset() const { return m_caretByteOffset; }
    size_t GetScrollByteOffset() const { return m_scrollByteOffset; }
    size_t GetSelectionAnchorByteOffset() const
    {
        return m_selectionAnchorByteOffset;
    }
    bool HasSelection() const;
    size_t GetSelectionStartByteOffset() const;
    size_t GetSelectionEndByteOffset() const;
    std::string GetSelectedText() const;

    const std::string& GetPlaceholder() const { return m_placeholder; }
    void SetPlaceholder(const std::string& placeholder) { m_placeholder = placeholder; }

    void SetOnTextChanged(TextChangedCallback callback)
    {
        m_onTextChanged = std::move(callback);
    }

    void SetOnUnhandledKeyDown(KeyDownCallback callback)
    {
        m_onUnhandledKeyDown = std::move(callback);
    }

    void ApplyTheme(const UI::UITheme& theme);

    static Ptr Create()
    {
        return std::make_shared<EditorTextInput>();
    }

protected:
    void OnRender(UI::UIRenderer& renderer) override;
    bool HandleEvent(const UI::UIEvent& event) override;

private:
    void NotifyTextChanged();
    void AppendText(const std::string& text);
    void Backspace();
    void DeleteForward();
    void EnsureCaretVisible(UI::UIRenderer& renderer, float visibleWidth);
    void MoveCaretLeft(bool extendSelection, bool word);
    void MoveCaretRight(bool extendSelection, bool word);
    void MoveCaretHome(bool extendSelection);
    void MoveCaretEnd(bool extendSelection);
    void MoveCaretTo(size_t offset, bool extendSelection);
    void SelectAll();
    bool DeleteSelection(bool notify = true);
    void ClearSelection();
    void CopySelectionToClipboard();
    void CutSelectionToClipboard();
    void PasteFromClipboard();
    size_t PreviousWordBoundary(size_t offset) const;
    size_t NextWordBoundary(size_t offset) const;
    size_t ByteOffsetFromMousePosition(float mouseX) const;

    std::string m_text;
    std::string m_placeholder = "Search";
    size_t m_caretByteOffset = 0u;
    size_t m_selectionAnchorByteOffset = 0u;
    size_t m_scrollByteOffset = 0u;
    bool m_dragSelecting = false;
    TextChangedCallback m_onTextChanged;
    KeyDownCallback m_onUnhandledKeyDown;

    UI::UIColor m_backgroundColor{0.12f, 0.13f, 0.14f, 1.0f};
    UI::UIColor m_hoverColor{0.16f, 0.17f, 0.19f, 1.0f};
    UI::UIColor m_focusColor{0.18f, 0.20f, 0.23f, 1.0f};
    UI::UIColor m_borderColor{0.26f, 0.28f, 0.31f, 1.0f};
    UI::UIColor m_focusBorderColor{0.18f, 0.55f, 0.95f, 1.0f};
    UI::UIColor m_textColor{0.90f, 0.92f, 0.94f, 1.0f};
    UI::UIColor m_placeholderColor{0.58f, 0.63f, 0.68f, 1.0f};
    float m_fontSize = 12.0f;
};

} // namespace RVX::Editor
