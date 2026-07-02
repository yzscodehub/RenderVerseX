/**
 * @file EditorFilePickerDialog.cpp
 * @brief Native editor file picker dialog service implementation
 */

#include "Editor/UI/EditorFilePickerDialog.h"

#include "Editor/UI/EditorPopupLayer.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTextInput.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "UI/UIContext.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_FILE_PICKER_ROOT = "Editor.FilePicker";

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

    std::string DisplayTitle(const EditorFilePickerDialogDesc& desc)
    {
        if (!desc.title.empty())
        {
            return desc.title;
        }
        return desc.picker.mode == EditorFilePickerMode::SaveFile ? "Save File"
                                                                  : "Open File";
    }

    std::string AcceptButtonText(const EditorFilePickerDialogDesc& desc)
    {
        if (!desc.acceptButtonText.empty())
        {
            return desc.acceptButtonText;
        }
        return desc.picker.mode == EditorFilePickerMode::SaveFile ? "Save"
                                                                  : "Open";
    }

    std::string EntryId(const EditorFilePickerEntry& entry)
    {
        const char* prefix = entry.directory ? "dir." : "file.";
        return std::string(prefix) + SanitizeWidgetSegment(entry.displayName);
    }

    std::string EntryText(const EditorFilePickerEntry& entry)
    {
        return entry.directory ? "[DIR] " + entry.displayName
                               : entry.displayName;
    }

    std::string EntryCategory(const EditorFilePickerEntry& entry)
    {
        return entry.directory ? "Directories" : "Files";
    }

    std::string DirectoryButtonText(const std::filesystem::path& path)
    {
        const std::string name = path.filename().string();
        if (!name.empty())
        {
            return name;
        }

        const std::string text = path.string();
        return text.empty() ? "/" : text;
    }

    float ClampDialogWidth(const EditorFilePickerDialogDesc& desc,
                           float surfaceWidth)
    {
        const float availableWidth = std::max(1.0f, surfaceWidth - 32.0f);
        const float maxWidth = std::clamp(desc.maxWidth, 1.0f, availableWidth);
        const float minWidth = std::clamp(desc.minWidth, 1.0f, maxWidth);
        return std::clamp(availableWidth * 0.64f, minWidth, maxWidth);
    }

    void ApplyButtonStyle(UI::Button& button,
                          const UI::UITheme& theme,
                          bool primary,
                          bool enabled)
    {
        button.GetStyle().fontSize = EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::Control);
        button.GetStyle().textColor = enabled ? theme.colors.text
                                              : theme.colors.textMuted;
        button.SetNormalColor(primary ? theme.colors.accent : theme.colors.surface);
        button.SetHoverColor(primary ? theme.colors.accentHover
                                     : theme.colors.surfaceHover);
        button.SetPressedColor(theme.colors.surfaceActive);
        button.SetDisabledColor(theme.colors.surface.WithAlpha(0.35f));
        button.SetEnabled(enabled);
        button.SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        button.SetTooltipText(button.GetText());
    }

    UI::Label::Ptr CreateLabel(const std::string& name,
                               const std::string& text,
                               float fontSize,
                               const UI::UIColor& color)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(fontSize);
        label->SetTextColor(color);
        label->SetTextAlign(UI::TextAlign::Left);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        label->SetTooltipText(text);
        label->SetInteractive(false);
        return label;
    }

    class EditorFilePickerOverlayPanel final : public UI::Panel
    {
    public:
        EditorFilePickerOverlayPanel(EditorFilePickerDialog& dialog,
                                     EditorUIHost& host,
                                     UI::Rect dialogBounds)
            : m_dialog(dialog)
            , m_host(host)
            , m_dialogBounds(dialogBounds)
        {
            SetInteractive(true);
        }

        bool HandleEvent(const UI::UIEvent& event) override
        {
            if (event.type == UI::UIEventType::KeyDown &&
                m_dialog.HandleKeyDown(static_cast<uint32>(event.keyCode),
                                       m_host))
            {
                return true;
            }

            if (event.type == UI::UIEventType::MouseDown &&
                !m_dialogBounds.Contains(event.position))
            {
                return true;
            }

            return UI::Panel::HandleEvent(event);
        }

    private:
        EditorFilePickerDialog& m_dialog;
        EditorUIHost& m_host;
        UI::Rect m_dialogBounds;
    };
}

void EditorFilePickerDialog::Open(EditorFilePickerDialogDesc desc)
{
    if (desc.id.empty())
    {
        desc.id = "FilePicker";
    }

    m_desc = std::move(desc);
    m_filterText.clear();
    m_pathText.clear();
    m_error.clear();
    m_visibleEntryIndices.clear();
    m_filterModel.SetItems({});
    m_filterModel.SetFilterText({});
    m_filterModel.Rebuild();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    if (!m_model.Open(m_desc.picker))
    {
        m_error = m_model.GetLastError();
    }
    ResetPathTextFromModel();
    m_open = true;
}

void EditorFilePickerDialog::Close()
{
    m_open = false;
}

void EditorFilePickerDialog::Clear()
{
    m_desc = {};
    m_filterText.clear();
    m_pathText.clear();
    m_error.clear();
    m_visibleEntryIndices.clear();
    m_filterModel.SetItems({});
    m_filterModel.SetFilterText({});
    m_filterModel.Rebuild();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    m_lastBuildStats = {};
    m_open = false;
}

std::string EditorFilePickerDialog::GetRootWidgetName() const
{
    return RVX_EDITOR_FILE_PICKER_ROOT;
}

std::string EditorFilePickerDialog::GetFocusWidgetName() const
{
    return MakeListWidgetName() + ".Search";
}

void EditorFilePickerDialog::RebuildEntryItems()
{
    const std::vector<EditorFilePickerEntry>& entries = m_model.GetVisibleEntries();

    std::vector<EditorPickerFilterItemDesc> filterItems;
    filterItems.reserve(entries.size());
    for (uint32 index = 0; index < static_cast<uint32>(entries.size()); ++index)
    {
        const EditorFilePickerEntry& entry = entries[index];
        EditorPickerFilterItemDesc item;
        item.id = EntryId(entry);
        item.text = EntryText(entry);
        item.category = EntryCategory(entry);
        item.keywords.push_back(entry.displayName);
        item.keywords.push_back(entry.path.filename().string());
        item.keywords.push_back(entry.path.extension().string());
        item.enabled = true;
        filterItems.push_back(std::move(item));
    }

    EditorPickerFilterOptions options;
    options.groupByCategory = true;
    options.sortMatchesByScore = true;
    m_filterModel.SetOptions(options);
    m_filterModel.SetItems(std::move(filterItems));
    m_filterModel.SetFilterText(m_filterText);
    m_filterModel.Rebuild();

    const std::vector<EditorPickerFilterResult>& results =
        m_filterModel.GetResults();
    m_visibleEntryIndices.clear();
    m_visibleEntryIndices.reserve(results.size());

    std::vector<EditorPickerListItemState> listItems;
    listItems.reserve(results.size());
    const std::vector<EditorPickerFilterItemDesc>& sourceItems =
        m_filterModel.GetItems();
    for (const EditorPickerFilterResult& result : results)
    {
        if (result.sourceIndex >= sourceItems.size())
        {
            continue;
        }

        m_visibleEntryIndices.push_back(result.sourceIndex);
        listItems.push_back({sourceItems[result.sourceIndex].id, true});
    }

    m_listModel.SetItems(std::move(listItems));
}

bool EditorFilePickerDialog::SelectVisibleEntry(uint32 listIndex)
{
    if (listIndex >= static_cast<uint32>(m_visibleEntryIndices.size()))
    {
        SetDialogError("File picker selection index is out of range");
        return false;
    }

    if (!m_model.SelectEntry(m_visibleEntryIndices[listIndex]))
    {
        SetDialogError(m_model.GetLastError());
        return false;
    }

    m_listModel.SetSelectedIndex(static_cast<int32>(listIndex));
    m_error.clear();
    return true;
}

bool EditorFilePickerDialog::ActivateSelectedEntry(EditorUIHost& host)
{
    m_listModel.NormalizeSelection();
    const int32 selectedIndex = m_listModel.GetSelectedIndex();
    if (selectedIndex < 0)
    {
        return Accept(host);
    }

    if (!SelectVisibleEntry(static_cast<uint32>(selectedIndex)))
    {
        return false;
    }

    const std::filesystem::path selectedPath = m_model.GetSelectedPath();
    const std::vector<EditorFilePickerEntry>& entries = m_model.GetVisibleEntries();
    auto it = std::find_if(entries.begin(),
                           entries.end(),
                           [&selectedPath](const EditorFilePickerEntry& entry) {
                               return entry.path == selectedPath;
                           });
    if (it != entries.end() && it->directory)
    {
        if (!m_model.EnterSelectedDirectory())
        {
            SetDialogError(m_model.GetLastError());
            return false;
        }

        m_filterText.clear();
        m_listModel.ClearItems();
        m_listView.SetScrollOffsetY(0.0f);
        ResetPathTextFromModel();
        m_error.clear();
        return true;
    }

    return Accept(host);
}

bool EditorFilePickerDialog::GoToRecentDirectory(uint32 recentIndex)
{
    if (!m_model.EnterRecentDirectory(recentIndex))
    {
        SetDialogError(m_model.GetLastError().empty()
                           ? "Cannot open recent directory"
                           : m_model.GetLastError());
        return false;
    }

    m_filterText.clear();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    ResetPathTextFromModel();
    m_error.clear();
    return true;
}

bool EditorFilePickerDialog::GoToBreadcrumb(uint32 breadcrumbIndex)
{
    if (!m_model.EnterBreadcrumb(breadcrumbIndex))
    {
        SetDialogError(m_model.GetLastError().empty()
                           ? "Cannot open breadcrumb directory"
                           : m_model.GetLastError());
        return false;
    }

    m_filterText.clear();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    ResetPathTextFromModel();
    m_error.clear();
    return true;
}

bool EditorFilePickerDialog::GoBackDirectory()
{
    if (!m_model.GoBackDirectory())
    {
        SetDialogError(m_model.GetLastError().empty()
                           ? "Cannot navigate back"
                           : m_model.GetLastError());
        return false;
    }

    m_filterText.clear();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    ResetPathTextFromModel();
    m_error.clear();
    return true;
}

bool EditorFilePickerDialog::GoForwardDirectory()
{
    if (!m_model.GoForwardDirectory())
    {
        SetDialogError(m_model.GetLastError().empty()
                           ? "Cannot navigate forward"
                           : m_model.GetLastError());
        return false;
    }

    m_filterText.clear();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    ResetPathTextFromModel();
    m_error.clear();
    return true;
}

bool EditorFilePickerDialog::GoToParentDirectory()
{
    if (!m_model.GoToParentDirectory())
    {
        SetDialogError(m_model.GetLastError().empty()
                           ? "Cannot navigate to parent directory"
                           : m_model.GetLastError());
        return false;
    }

    m_filterText.clear();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    ResetPathTextFromModel();
    m_error.clear();
    return true;
}

bool EditorFilePickerDialog::RefreshDirectory()
{
    if (!m_model.Refresh())
    {
        SetDialogError(m_model.GetLastError());
        return false;
    }

    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    ResetPathTextFromModel();
    m_error.clear();
    return true;
}

bool EditorFilePickerDialog::SubmitPathText()
{
    if (m_pathText.empty())
    {
        SetDialogError("Enter a directory path before navigating");
        return false;
    }

    if (!m_model.SetCurrentDirectory(std::filesystem::path(m_pathText)))
    {
        SetDialogError(m_model.GetLastError());
        return false;
    }

    m_filterText.clear();
    m_listModel.ClearItems();
    m_listView.SetScrollOffsetY(0.0f);
    ResetPathTextFromModel();
    m_error.clear();
    return true;
}

void EditorFilePickerDialog::ResetPathTextFromModel()
{
    m_pathText = m_model.GetCurrentDirectory().string();
}

bool EditorFilePickerDialog::Accept(EditorUIHost& host)
{
    if (!m_model.CanAcceptSelection())
    {
        SetDialogError("Select a valid file path before continuing");
        return false;
    }

    EditorFilePickerDialogResult result;
    result.accepted = true;
    result.path = m_model.ResolveAcceptedPath();
    EditorFilePickerDialogResultCallback onResult = m_desc.onResult;
    Close();
    if (onResult)
    {
        onResult(result, host);
    }
    return true;
}

bool EditorFilePickerDialog::Cancel(EditorUIHost& host)
{
    EditorFilePickerDialogResult result;
    result.accepted = false;
    EditorFilePickerDialogResultCallback onResult = m_desc.onResult;
    Close();
    if (onResult)
    {
        onResult(result, host);
    }
    return true;
}

bool EditorFilePickerDialog::HandleKeyDown(uint32 keyCode, EditorUIHost& host)
{
    if (!m_open)
    {
        return false;
    }

    if (keyCode == UI::RVX_UI_KEY_ESCAPE && m_desc.closeOnEscape)
    {
        return Cancel(host);
    }
    if (keyCode == UI::RVX_UI_KEY_DOWN)
    {
        m_listModel.MoveSelection(1);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_UP)
    {
        m_listModel.MoveSelection(-1);
        return true;
    }
    if (keyCode == UI::RVX_UI_KEY_BACKSPACE)
    {
        return GoToParentDirectory();
    }
    if (keyCode == UI::RVX_UI_KEY_ENTER || keyCode == UI::RVX_UI_KEY_SPACE)
    {
        return ActivateSelectedEntry(host);
    }

    return false;
}

void EditorFilePickerDialog::Build(UI::UIContext& ui,
                                   EditorPopupLayer& popupLayer,
                                   EditorUIHost& host)
{
    m_lastBuildStats = {};
    if (!m_open)
    {
        return;
    }

    RebuildEntryItems();

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
    const float pathHeight =
        std::max(shellMetrics.dialogControlHeight,
                 shellMetrics.dialogBodyLineHeight + 6.0f);
    const float controlHeight = shellMetrics.dialogControlHeight;
    const float buttonHeight = shellMetrics.dialogControlHeight;
    const float dialogWidth = ClampDialogWidth(m_desc, surfaceWidth);
    const float contentWidth = std::max(1.0f, dialogWidth - padding * 2.0f);
    const float maxListHeight =
        std::max(96.0f, std::min(260.0f, surfaceHeight * 0.34f));
    const float listHeight = maxListHeight;
    const float errorHeight = m_error.empty() ? 0.0f : pathHeight;
    const float breadcrumbRowHeight = controlHeight;
    const float recentRowHeight = controlHeight;
    const float bottomRowHeight = controlHeight + spacing + buttonHeight;
    const float dialogHeight = std::min(
        std::max(1.0f, surfaceHeight - 32.0f),
        padding * 4.0f + titleHeight + pathHeight + breadcrumbRowHeight +
            controlHeight + recentRowHeight + listHeight + errorHeight +
            bottomRowHeight + spacing * 7.0f);
    const float dialogX = std::max(0.0f, (surfaceWidth - dialogWidth) * 0.5f);
    const float dialogY = std::max(0.0f, (surfaceHeight - dialogHeight) * 0.5f);
    const UI::Rect overlayBounds(0.0f, 0.0f, surfaceWidth, surfaceHeight);
    const UI::Rect dialogBounds(dialogX, dialogY, dialogWidth, dialogHeight);
    const std::string widgetPrefix = MakeWidgetPrefix();

    std::shared_ptr<EditorFilePickerOverlayPanel> overlay =
        std::make_shared<EditorFilePickerOverlayPanel>(*this,
                                                       host,
                                                       dialogBounds);
    overlay->SetName(RVX_EDITOR_FILE_PICKER_ROOT);
    overlay->SetPosition(overlayBounds.x, overlayBounds.y);
    overlay->SetSize(overlayBounds.width, overlayBounds.height);
    overlay->SetBackgroundColor(UI::UIColor(0.0f, 0.0f, 0.0f, 0.50f));
    overlay->SetBorderWidth(0.0f);

    UI::Panel::Ptr dialog = UI::Panel::Create();
    dialog->SetName(widgetPrefix + ".Panel");
    dialog->SetPosition(dialogX, dialogY);
    dialog->SetSize(dialogWidth, dialogHeight);
    dialog->SetBackgroundColor(theme.colors.panelBackground);
    dialog->SetBorderColor(theme.colors.border);
    dialog->SetBorderWidth(theme.metrics.borderWidth);
    dialog->SetClipChildren(true);

    float localY = padding;
    UI::Label::Ptr title = CreateLabel(widgetPrefix + ".Title",
                                       DisplayTitle(m_desc),
                                       EditorTypography::GetFontSize(
                                           theme,
                                           EditorTypographyRole::DialogTitle),
                                       theme.colors.text);
    title->SetPosition(padding, localY);
    title->SetSize(contentWidth, titleHeight);
    dialog->AddChild(title);
    localY += titleHeight + spacing;

    const float pathLabelWidth = 42.0f;
    const float pathGoButtonWidth = 52.0f;
    UI::Label::Ptr pathLabel = CreateLabel(widgetPrefix + ".PathLabel",
                                           "Path",
                                           EditorTypography::GetFontSize(
                                               theme,
                                               EditorTypographyRole::PropertyLabel),
                                           theme.colors.textMuted);
    pathLabel->SetPosition(padding, localY);
    pathLabel->SetSize(pathLabelWidth, pathHeight);
    dialog->AddChild(pathLabel);

    EditorTextInput::Ptr path = EditorTextInput::Create();
    path->SetName(widgetPrefix + ".Path");
    path->ApplyTheme(theme);
    path->SetPlaceholder("Directory path");
    path->SetText(m_pathText);
    path->SetPosition(padding + pathLabelWidth + spacing, localY);
    path->SetSize(std::max(1.0f,
                           contentWidth - pathLabelWidth - pathGoButtonWidth -
                               spacing * 2.0f),
                  pathHeight);
    path->SetOnTextChanged([this](const std::string& text) {
        m_pathText = text;
    });
    path->SetOnUnhandledKeyDown([this](uint32 keyCode) {
        if (keyCode == UI::RVX_UI_KEY_ENTER)
        {
            SubmitPathText();
            return true;
        }
        return false;
    });
    dialog->AddChild(path);

    UI::Button::Ptr goButton = UI::Button::Create("Go");
    goButton->SetName(widgetPrefix + ".Button.go");
    goButton->SetPosition(dialogWidth - padding - pathGoButtonWidth, localY);
    goButton->SetSize(pathGoButtonWidth, pathHeight);
    ApplyButtonStyle(*goButton, theme, false, true);
    goButton->SetOnClick([this](const UI::UIEvent&) {
        SubmitPathText();
    });
    dialog->AddChild(goButton);
    localY += pathHeight + spacing;

    const float breadcrumbLabelWidth = 78.0f;
    UI::Label::Ptr breadcrumbLabel = CreateLabel(widgetPrefix + ".BreadcrumbLabel",
                                                 "Breadcrumb",
                                                 EditorTypography::GetFontSize(
                                                     theme,
                                                     EditorTypographyRole::PropertyLabel),
                                                 theme.colors.textMuted);
    breadcrumbLabel->SetPosition(padding, localY);
    breadcrumbLabel->SetSize(breadcrumbLabelWidth, breadcrumbRowHeight);
    dialog->AddChild(breadcrumbLabel);

    const std::vector<EditorFilePickerBreadcrumb>& breadcrumbs =
        m_model.GetBreadcrumbs();
    constexpr uint32 RVX_MAX_VISIBLE_FILE_PICKER_BREADCRUMBS = 5;
    const uint32 breadcrumbCount = static_cast<uint32>(breadcrumbs.size());
    float breadcrumbX = padding + breadcrumbLabelWidth + spacing;
    const float breadcrumbRight = dialogWidth - padding;

    struct VisibleBreadcrumbButton
    {
        uint32 index = 0;
        std::string text;
        float width = 0.0f;
    };

    auto measureBreadcrumbWidth = [](const std::string& text) {
        return std::clamp(42.0f + static_cast<float>(text.size()) * 5.0f,
                          46.0f,
                          112.0f);
    };

    std::vector<VisibleBreadcrumbButton> visibleBreadcrumbs;
    visibleBreadcrumbs.reserve(
        std::min(breadcrumbCount, RVX_MAX_VISIBLE_FILE_PICKER_BREADCRUMBS));
    float usedBreadcrumbWidth = 0.0f;
    const float availableBreadcrumbWidth =
        std::max(0.0f, breadcrumbRight - breadcrumbX);
    for (uint32 reverseIndex = 0u;
         reverseIndex < breadcrumbCount &&
         visibleBreadcrumbs.size() < RVX_MAX_VISIBLE_FILE_PICKER_BREADCRUMBS;
         ++reverseIndex)
    {
        const uint32 index = breadcrumbCount - 1u - reverseIndex;
        const EditorFilePickerBreadcrumb& breadcrumb = breadcrumbs[index];
        std::string text = breadcrumb.displayName;
        float buttonWidth = measureBreadcrumbWidth(text);
        const float additionalWidth =
            (visibleBreadcrumbs.empty() ? 0.0f : spacing) + buttonWidth;
        if (usedBreadcrumbWidth + additionalWidth > availableBreadcrumbWidth)
        {
            if (visibleBreadcrumbs.empty())
            {
                buttonWidth = std::max(1.0f, availableBreadcrumbWidth);
                visibleBreadcrumbs.push_back({index, text, buttonWidth});
            }
            break;
        }

        visibleBreadcrumbs.push_back({index, text, buttonWidth});
        usedBreadcrumbWidth += additionalWidth;
    }
    std::reverse(visibleBreadcrumbs.begin(), visibleBreadcrumbs.end());
    if (!visibleBreadcrumbs.empty() && visibleBreadcrumbs.front().index > 0u)
    {
        visibleBreadcrumbs.front().text = "...";
        visibleBreadcrumbs.front().width =
            std::min(visibleBreadcrumbs.front().width,
                     measureBreadcrumbWidth("..."));
    }

    for (const VisibleBreadcrumbButton& visibleBreadcrumb : visibleBreadcrumbs)
    {
        UI::Button::Ptr breadcrumbButton = UI::Button::Create(visibleBreadcrumb.text);
        breadcrumbButton->SetName(widgetPrefix + ".Breadcrumb." +
                                  std::to_string(visibleBreadcrumb.index));
        breadcrumbButton->SetPosition(breadcrumbX, localY);
        breadcrumbButton->SetSize(visibleBreadcrumb.width, breadcrumbRowHeight);
        ApplyButtonStyle(*breadcrumbButton,
                         theme,
                         visibleBreadcrumb.index + 1u == breadcrumbCount,
                         true);
        breadcrumbButton->SetOnClick([this, index = visibleBreadcrumb.index](
                                         const UI::UIEvent&) {
            GoToBreadcrumb(index);
        });
        dialog->AddChild(breadcrumbButton);
        breadcrumbX += visibleBreadcrumb.width + spacing;
    }
    localY += breadcrumbRowHeight + spacing;

    const float historyButtonWidth = 36.0f;
    const float navButtonWidth = 74.0f;
    UI::Button::Ptr backButton = UI::Button::Create("<");
    backButton->SetName(widgetPrefix + ".Button.back");
    backButton->SetPosition(padding, localY);
    backButton->SetSize(historyButtonWidth, controlHeight);
    ApplyButtonStyle(*backButton, theme, false, m_model.CanGoBackDirectory());
    backButton->SetOnClick([this](const UI::UIEvent&) {
        GoBackDirectory();
    });
    dialog->AddChild(backButton);

    UI::Button::Ptr forwardButton = UI::Button::Create(">");
    forwardButton->SetName(widgetPrefix + ".Button.forward");
    forwardButton->SetPosition(padding + historyButtonWidth + spacing, localY);
    forwardButton->SetSize(historyButtonWidth, controlHeight);
    ApplyButtonStyle(*forwardButton,
                     theme,
                     false,
                     m_model.CanGoForwardDirectory());
    forwardButton->SetOnClick([this](const UI::UIEvent&) {
        GoForwardDirectory();
    });
    dialog->AddChild(forwardButton);

    const float navStartX = padding + (historyButtonWidth + spacing) * 2.0f;
    UI::Button::Ptr upButton = UI::Button::Create("Up");
    upButton->SetName(widgetPrefix + ".Button.up");
    upButton->SetPosition(navStartX, localY);
    upButton->SetSize(navButtonWidth, controlHeight);
    ApplyButtonStyle(*upButton, theme, false, true);
    upButton->SetOnClick([this](const UI::UIEvent&) {
        GoToParentDirectory();
    });
    dialog->AddChild(upButton);

    UI::Button::Ptr refreshButton = UI::Button::Create("Refresh");
    refreshButton->SetName(widgetPrefix + ".Button.refresh");
    refreshButton->SetPosition(navStartX + navButtonWidth + spacing, localY);
    refreshButton->SetSize(navButtonWidth + 8.0f, controlHeight);
    ApplyButtonStyle(*refreshButton, theme, false, true);
    refreshButton->SetOnClick([this](const UI::UIEvent&) {
        RefreshDirectory();
    });
    dialog->AddChild(refreshButton);
    localY += controlHeight + spacing;

    const float recentLabelWidth = 54.0f;
    UI::Label::Ptr recentLabel = CreateLabel(widgetPrefix + ".RecentLabel",
                                             "Recent",
                                             EditorTypography::GetFontSize(
                                                 theme,
                                                 EditorTypographyRole::PropertyLabel),
                                             theme.colors.textMuted);
    recentLabel->SetPosition(padding, localY);
    recentLabel->SetSize(recentLabelWidth, recentRowHeight);
    dialog->AddChild(recentLabel);

    const std::vector<std::filesystem::path>& recentDirectories =
        m_model.GetRecentDirectories();
    constexpr uint32 RVX_MAX_VISIBLE_FILE_PICKER_RECENTS = 4;
    const uint32 recentCount = std::min(
        static_cast<uint32>(recentDirectories.size()),
        RVX_MAX_VISIBLE_FILE_PICKER_RECENTS);
    float recentX = padding + recentLabelWidth + spacing;
    const float recentRight = dialogWidth - padding;
    for (uint32 index = 0; index < recentCount; ++index)
    {
        const std::string text = DirectoryButtonText(recentDirectories[index]);
        const float buttonWidth =
            std::clamp(46.0f + static_cast<float>(text.size()) * 5.0f,
                       58.0f,
                       128.0f);
        if (recentX + buttonWidth > recentRight)
        {
            break;
        }

        UI::Button::Ptr recentButton = UI::Button::Create(text);
        recentButton->SetName(widgetPrefix + ".Recent." +
                              std::to_string(index));
        recentButton->SetPosition(recentX, localY);
        recentButton->SetSize(buttonWidth, recentRowHeight);
        ApplyButtonStyle(*recentButton, theme, index == 0u, true);
        recentButton->SetOnClick([this, index](const UI::UIEvent&) {
            GoToRecentDirectory(index);
        });
        dialog->AddChild(recentButton);
        recentX += buttonWidth + spacing;
    }
    localY += recentRowHeight + spacing;

    UI::Panel::Ptr listContainer = UI::Panel::Create();
    listContainer->SetName(widgetPrefix + ".ListContainer");
    listContainer->SetPosition(padding, localY);
    listContainer->SetSize(contentWidth, listHeight);
    listContainer->SetBackgroundColor(UI::UIColor::Transparent());
    listContainer->SetBorderWidth(0.0f);
    dialog->AddChild(listContainer);

    std::vector<EditorPickerListViewItemDesc> viewItems;
    const std::vector<EditorPickerFilterItemDesc>& sourceItems =
        m_filterModel.GetItems();
    const std::vector<EditorPickerFilterResult>& results =
        m_filterModel.GetResults();
    viewItems.reserve(results.size());
    for (uint32 listIndex = 0; listIndex < static_cast<uint32>(results.size()); ++listIndex)
    {
        const EditorPickerFilterResult& result = results[listIndex];
        if (result.sourceIndex >= sourceItems.size())
        {
            continue;
        }

        const EditorPickerFilterItemDesc& sourceItem = sourceItems[result.sourceIndex];
        EditorPickerListViewItemDesc item;
        item.id = sourceItem.id;
        item.text = sourceItem.text;
        item.category = sourceItem.category;
        item.enabled = true;
        item.onClick = [this, listIndex]() {
            SelectVisibleEntry(listIndex);
        };
        viewItems.push_back(std::move(item));
    }

    EditorPickerListViewDesc listDesc;
    listDesc.ui = &ui;
    listDesc.parent = listContainer.get();
    listDesc.model = &m_listModel;
    listDesc.name = MakeListWidgetName();
    listDesc.bounds = UI::Rect(0.0f, 0.0f, contentWidth, 0.0f);
    listDesc.searchText = m_filterText;
    listDesc.searchPlaceholder = "Search files";
    listDesc.emptyText = "No files";
    listDesc.items = std::move(viewItems);
    listDesc.onSearchChanged = [this](const std::string& text) {
        m_filterText = text;
        m_listModel.ClearSelection();
        m_listView.SetScrollOffsetY(0.0f);
    };
    listDesc.onKeyDown = [this, &host](uint32 keyCode) {
        return HandleKeyDown(keyCode, host);
    };
    listDesc.padding = shellMetrics.contentPadding;
    listDesc.rowHeight = shellMetrics.pickerRowHeight;
    listDesc.categoryHeight = shellMetrics.pickerCategoryHeight;
    listDesc.maxListHeight = std::max(40.0f, listHeight - 12.0f);
    m_listView.Build(listDesc);
    const EditorPickerListViewStats& listStats = m_listView.GetLastBuildStats();
    localY += std::max(1.0f, listStats.bounds.height) + spacing;

    UI::Label::Ptr fileNameLabel = CreateLabel(widgetPrefix + ".FileNameLabel",
                                               "File",
                                               EditorTypography::GetFontSize(
                                                   theme,
                                                   EditorTypographyRole::PropertyLabel),
                                               theme.colors.textMuted);
    fileNameLabel->SetPosition(padding, localY);
    fileNameLabel->SetSize(54.0f, controlHeight);
    dialog->AddChild(fileNameLabel);

    EditorTextInput::Ptr fileName = EditorTextInput::Create();
    fileName->SetName(widgetPrefix + ".FileName");
    fileName->ApplyTheme(theme);
    fileName->SetPlaceholder("File name");
    fileName->SetText(m_model.GetTypedFileName());
    fileName->SetPosition(padding + 58.0f, localY);
    fileName->SetSize(std::max(1.0f, contentWidth - 58.0f), controlHeight);
    fileName->SetOnTextChanged([this](const std::string& text) {
        m_model.SetTypedFileName(text);
        m_error.clear();
    });
    fileName->SetOnUnhandledKeyDown([this, &host](uint32 keyCode) {
        return HandleKeyDown(keyCode, host);
    });
    dialog->AddChild(fileName);
    localY += controlHeight + spacing;

    if (!m_error.empty())
    {
        UI::Label::Ptr error = CreateLabel(widgetPrefix + ".Error",
                                           m_error,
                                           EditorTypography::GetFontSize(
                                               theme,
                                               EditorTypographyRole::DialogBody),
                                           theme.colors.error);
        error->SetPosition(padding, localY);
        error->SetSize(contentWidth, pathHeight);
        dialog->AddChild(error);
        localY += pathHeight + spacing;
    }

    const float acceptWidth = 86.0f;
    const float cancelWidth = 86.0f;
    const float buttonY = dialogHeight - padding - buttonHeight;
    const bool canAccept = m_model.CanAcceptSelection();

    UI::Button::Ptr acceptButton = UI::Button::Create(AcceptButtonText(m_desc));
    acceptButton->SetName(widgetPrefix + ".Button.accept");
    acceptButton->SetPosition(dialogWidth - padding - acceptWidth, buttonY);
    acceptButton->SetSize(acceptWidth, buttonHeight);
    ApplyButtonStyle(*acceptButton, theme, true, canAccept);
    acceptButton->SetOnClick([this, &host](const UI::UIEvent&) {
        Accept(host);
    });
    dialog->AddChild(acceptButton);

    UI::Button::Ptr cancelButton = UI::Button::Create(m_desc.cancelButtonText);
    cancelButton->SetName(widgetPrefix + ".Button.cancel");
    cancelButton->SetPosition(dialogWidth - padding - acceptWidth -
                                  spacing - cancelWidth,
                              buttonY);
    cancelButton->SetSize(cancelWidth, buttonHeight);
    ApplyButtonStyle(*cancelButton, theme, false, true);
    cancelButton->SetOnClick([this, &host](const UI::UIEvent&) {
        Cancel(host);
    });
    dialog->AddChild(cancelButton);

    overlay->AddChild(dialog);
    popupLayer.AddPopup(std::move(overlay));

    m_lastBuildStats.open = true;
    m_lastBuildStats.canAccept = canAccept;
    m_lastBuildStats.canGoBackDirectory = m_model.CanGoBackDirectory();
    m_lastBuildStats.canGoForwardDirectory = m_model.CanGoForwardDirectory();
    m_lastBuildStats.breadcrumbCount =
        static_cast<uint32>(m_model.GetBreadcrumbs().size());
    m_lastBuildStats.recentDirectoryCount =
        static_cast<uint32>(m_model.GetRecentDirectories().size());
    m_lastBuildStats.entryCount = m_model.GetLastBuildStats().entryCount;
    m_lastBuildStats.visibleEntryCount =
        m_model.GetLastBuildStats().visibleEntryCount;
    m_lastBuildStats.filteredEntryCount =
        m_filterModel.GetLastBuildStats().resultCount;
    m_lastBuildStats.directoryCount = m_model.GetLastBuildStats().directoryCount;
    m_lastBuildStats.fileCount = m_model.GetLastBuildStats().fileCount;
    m_lastBuildStats.selectedItemIndex = m_listModel.GetSelectedIndex();
    m_lastBuildStats.currentDirectory = m_model.GetCurrentDirectory();
    m_lastBuildStats.acceptedPath = m_model.ResolveAcceptedPath();
    m_lastBuildStats.filterText = m_filterText;
    m_lastBuildStats.pathText = m_pathText;
    m_lastBuildStats.typedFileName = m_model.GetTypedFileName();
    m_lastBuildStats.error =
        m_error.empty() ? m_model.GetLastError() : m_error;
    m_lastBuildStats.overlayBounds = overlayBounds;
    m_lastBuildStats.dialogBounds = dialogBounds;
    m_lastBuildStats.modelStats = m_model.GetLastBuildStats();
    m_lastBuildStats.filterStats = m_filterModel.GetLastBuildStats();
    m_lastBuildStats.listStats = listStats;
}

void EditorFilePickerDialog::SetDialogError(std::string error)
{
    m_error = std::move(error);
}

std::string EditorFilePickerDialog::MakeWidgetPrefix() const
{
    return std::string(RVX_EDITOR_FILE_PICKER_ROOT) + "." +
           SanitizeWidgetSegment(m_desc.id);
}

std::string EditorFilePickerDialog::MakeListWidgetName() const
{
    return MakeWidgetPrefix() + ".List";
}

} // namespace RVX::Editor
