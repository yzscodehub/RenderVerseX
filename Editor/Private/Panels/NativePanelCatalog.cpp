/**
 * @file NativePanelCatalog.cpp
 * @brief Native UI panel catalog and visibility diagnostics panel implementation
 */

#include "Editor/Panels/NativePanelCatalog.h"

#include "Editor/UI/EditorCommandCatalog.h"
#include "Editor/UI/EditorCommandRegistry.h"
#include "Editor/UI/EditorIconButton.h"
#include "Editor/UI/EditorListLayout.h"
#include "Editor/UI/EditorPanelButtonStyle.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTextInput.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    const char* DockAreaLabel(EditorUIPanelDockArea area)
    {
        switch (area)
        {
            case EditorUIPanelDockArea::Center:
                return "Center";
            case EditorUIPanelDockArea::Left:
                return "Left";
            case EditorUIPanelDockArea::Right:
                return "Right";
            case EditorUIPanelDockArea::Bottom:
                return "Bottom";
            case EditorUIPanelDockArea::Floating:
                return "Floating";
        }

        return "Unknown";
    }

    std::string RebuildStateLabel(const EditorUIPanelRebuildState& state)
    {
        if (state.pending)
        {
            return "Pending r" + std::to_string(state.revision);
        }

        if (state.builtThisFrame)
        {
            return "Built r" + std::to_string(state.revision);
        }

        return "Idle r" + std::to_string(state.revision);
    }

    float PanelCatalogToolbarHeight(const EditorShellMetrics& shellMetrics)
    {
        return shellMetrics.twoRowPanelToolbarHeight +
               shellMetrics.compactGap + shellMetrics.panelControlHeight;
    }

    std::string ToLowerAscii(std::string text)
    {
        std::transform(text.begin(),
                       text.end(),
                       text.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        return text;
    }

    bool ContainsLowerToken(const std::string& text,
                            const std::string& lowerToken)
    {
        if (lowerToken.empty())
        {
            return true;
        }

        return ToLowerAscii(text).find(lowerToken) != std::string::npos;
    }

    bool MatchesPanelFilter(const EditorUIPanelSnapshot& snapshot,
                            const std::string& lowerFilter)
    {
        if (lowerFilter.empty())
        {
            return true;
        }

        return ContainsLowerToken(snapshot.id, lowerFilter) ||
               ContainsLowerToken(snapshot.title, lowerFilter) ||
               ContainsLowerToken(snapshot.visible ? "visible" : "hidden",
                                  lowerFilter) ||
               ContainsLowerToken(DockAreaLabel(snapshot.dockArea),
                                  lowerFilter) ||
               ContainsLowerToken(RebuildStateLabel(snapshot.rebuildState),
                                  lowerFilter);
    }

    std::string BuildPanelMetadataText(const EditorUIPanelSnapshot& snapshot)
    {
        return std::string(snapshot.visible ? "Visible" : "Hidden") +
               " | Dock: " + DockAreaLabel(snapshot.dockArea) +
               " | " + RebuildStateLabel(snapshot.rebuildState);
    }

    UI::Button::Ptr CreateCatalogButton(const std::string& name,
                                        const std::string& text,
                                        const UI::UITheme& theme)
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        EditorPanelButtonStyleDesc styleDesc;
        styleDesc.setTooltipFromText = false;
        EditorPanelButtonStyle::Apply(*button, theme, styleDesc);
        return button;
    }

    UI::Button::Ptr CreateCatalogIconButton(const std::string& name,
                                            const std::string& text,
                                            const std::string& iconName,
                                            const UI::UITheme& theme,
                                            bool active)
    {
        EditorIconButton::Ptr button =
            EditorIconButton::Create(text, iconName);
        button->SetName(name);
        EditorIconButtonActionStyleDesc styleDesc;
        styleDesc.active = active;
        button->ApplyActionStyle(theme, styleDesc);
        return button;
    }

    UI::Label::Ptr CreateCatalogLabel(const std::string& name,
                                      const std::string& text,
                                      const UI::UITheme& theme,
                                      const UI::UIColor& color)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        EditorTypography::ApplyToLabel(*label,
                                       theme,
                                       EditorTypographyRole::ListItem);
        label->SetTextColor(color);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetInteractive(false);
        return label;
    }

    bool IsCommandExecutable(EditorUIHost* host, const std::string& commandId)
    {
        if (!host)
        {
            return false;
        }

        const EditorCommand* command =
            host->GetCommandRegistry().FindCommand(commandId);
        return command && command->desc.enabled && command->desc.callback;
    }

    std::string BuildCommandTooltip(EditorUIHost* host,
                                    const std::string& commandId,
                                    const std::string& fallback)
    {
        if (!host)
        {
            return fallback;
        }

        const EditorCommand* command =
            host->GetCommandRegistry().FindCommand(commandId);
        if (!command)
        {
            return fallback + " Command is not registered.";
        }
        if (!command->desc.disabledReason.empty())
        {
            return fallback + " Unavailable: " + command->desc.disabledReason;
        }
        return command->desc.tooltip.empty() ? fallback : command->desc.tooltip;
    }

    UI::Button::Ptr CreateLayoutCommandButton(const std::string& name,
                                              const std::string& text,
                                              const std::string& iconName,
                                              const std::string& commandId,
                                              EditorUIPanelFrameContext& context,
                                              const UI::UITheme& theme)
    {
        UI::Button::Ptr button =
            iconName.empty()
                ? CreateCatalogButton(name, text, theme)
                : CreateCatalogIconButton(name, text, iconName, theme, false);
        const bool executable = IsCommandExecutable(context.host, commandId);
        button->SetEnabled(executable);
        button->SetTooltipText(BuildCommandTooltip(context.host,
                                                   commandId,
                                                   text));
        button->SetOnClick([host = context.host, commandId](
                               const UI::UIEvent& event) {
            (void)event;
            if (!host)
            {
                return;
            }

            host->ExecuteCommand(commandId);
            host->RequestPanelRebuild(NativePanelCatalogPanel::PanelId(),
                                      EditorUIPanelRebuildReason::Data);
        });
        return button;
    }
}

NativePanelCatalogPanel::NativePanelCatalogPanel()
{
    m_desc.id = PanelId();
    m_desc.title = "Panels";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Floating;
    m_desc.visibleByDefault = false;
    m_desc.closable = true;
}

void NativePanelCatalogPanel::BuildUI(EditorUIPanelFrameContext& context)
{
    m_lastBuildStats = {};
    m_lastBuildStats.built = true;
    m_lastBuildStats.hasHost = context.host != nullptr;
    m_lastBuildStats.filterText = m_searchFilter;
    m_lastBuildStats.filterActive = !m_searchFilter.empty();

    if (!context.ui || !context.contentContainer || !context.host)
    {
        return;
    }

    const std::vector<EditorUIPanelSnapshot> snapshots =
        context.host->GetPanelSnapshots();
    m_lastBuildStats.totalPanelCount = static_cast<uint32>(snapshots.size());
    for (const EditorUIPanelSnapshot& snapshot : snapshots)
    {
        if (snapshot.visible)
        {
            ++m_lastBuildStats.visiblePanelCount;
        }
        else
        {
            ++m_lastBuildStats.hiddenPanelCount;
        }

        if (snapshot.visibleByDefault)
        {
            ++m_lastBuildStats.defaultVisiblePanelCount;
        }
        if (snapshot.rebuildState.pending)
        {
            ++m_lastBuildStats.pendingRebuildPanelCount;
        }
    }

    AddToolbar(context);
    AddPanelRows(context);
}

void NativePanelCatalogPanel::AddToolbar(EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float buttonHeight = shellMetrics.panelControlHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);

    const std::string summary =
        std::to_string(m_lastBuildStats.totalPanelCount) + " panels | " +
        std::to_string(m_lastBuildStats.visiblePanelCount) + " visible | " +
        std::to_string(m_lastBuildStats.hiddenPanelCount) + " hidden | " +
        std::to_string(m_lastBuildStats.pendingRebuildPanelCount) + " pending";

    UI::Label::Ptr summaryLabel = CreateCatalogLabel("NativePanelCatalog.Summary",
                                                     summary,
                                                     theme,
                                                     theme.colors.textMuted);
    summaryLabel->SetPosition(padding, padding);
    summaryLabel->SetSize(rowWidth, buttonHeight);
    summaryLabel->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
    context.contentContainer->AddChild(summaryLabel);

    const float searchY = padding + buttonHeight + shellMetrics.compactGap;
    EditorTextInput::Ptr search = EditorTextInput::Create();
    search->SetName("NativePanelCatalog.Search");
    search->ApplyTheme(theme);
    search->SetPlaceholder("Search panels");
    search->SetText(m_searchFilter);
    search->SetPosition(padding, searchY);
    search->SetSize(rowWidth, buttonHeight);
    search->SetOnTextChanged([this](const std::string& text) {
        m_searchFilter = text;
        m_scrollOffsetY = 0.0f;
    });
    context.contentContainer->AddChild(search);
    ++m_lastBuildStats.searchInputCount;

    const float commandY = searchY + buttonHeight + shellMetrics.compactGap;
    const float commandGap = shellMetrics.compactGap;
    const float actionWidth = buttonHeight;
    float x = padding;

    auto addLayoutCommand = [&](const std::string& name,
                                const std::string& text,
                                const std::string& iconName,
                                const std::string& commandId,
                                float width) {
        UI::Button::Ptr button = CreateLayoutCommandButton(name,
                                                           text,
                                                           iconName,
                                                           commandId,
                                                           context,
                                                           theme);
        button->SetPosition(x, commandY);
        button->SetSize(width, buttonHeight);
        context.contentContainer->AddChild(button);
        ++m_lastBuildStats.layoutCommandButtonCount;
        if (dynamic_cast<EditorIconButton*>(button.get()))
        {
            ++m_lastBuildStats.layoutCommandIconButtonCount;
        }
        if (button->IsEnabled())
        {
            ++m_lastBuildStats.layoutCommandExecutableCount;
        }
        x += width + commandGap;
    };

    addLayoutCommand("NativePanelCatalog.Layout.Save",
                     "Save",
                     "layout-save",
                     EditorCommandIds::ViewSaveLayout,
                     actionWidth);
    addLayoutCommand("NativePanelCatalog.Layout.Reload",
                     "Reload",
                     "refresh",
                     EditorCommandIds::ViewReloadLayout,
                     actionWidth);
    addLayoutCommand("NativePanelCatalog.Layout.Reset",
                     "Reset",
                     "reset",
                     EditorCommandIds::ViewResetLayout,
                     actionWidth);
}

void NativePanelCatalogPanel::AddPanelRows(EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float titleLineHeight = EditorTypography::GetLineHeight(
        theme,
        EditorTypographyRole::ListItem);
    const float metadataLineHeight = EditorTypography::GetLineHeight(
        theme,
        EditorTypographyRole::Caption);
    const float rowHeight = std::max(shellMetrics.panelListRowHeight,
                                     titleLineHeight + metadataLineHeight + 10.0f);
    const float startY = PanelCatalogToolbarHeight(shellMetrics);
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const float listHeight =
        std::max(0.0f, context.contentContainer->GetHeight() - startY - padding);

    const std::vector<EditorUIPanelSnapshot> snapshots =
        context.host->GetPanelSnapshots();
    const std::string lowerFilter = ToLowerAscii(m_searchFilter);
    std::vector<size_t> visiblePanelIndices;
    visiblePanelIndices.reserve(snapshots.size());
    for (size_t index = 0; index < snapshots.size(); ++index)
    {
        if (MatchesPanelFilter(snapshots[index], lowerFilter))
        {
            visiblePanelIndices.push_back(index);
        }
    }

    m_lastBuildStats.filteredPanelCount =
        static_cast<uint32>(visiblePanelIndices.size());
    m_lastBuildStats.rowCount = m_lastBuildStats.filteredPanelCount;
    m_lastBuildStats.rowHeight = rowHeight;

    if (snapshots.empty())
    {
        UI::Label::Ptr empty = CreateCatalogLabel("NativePanelCatalog.Empty",
                                                  "No native panels registered.",
                                                  theme,
                                                  theme.colors.textMuted);
        empty->SetPosition(padding, startY);
        empty->SetSize(rowWidth, rowHeight);
        context.contentContainer->AddChild(empty);
        return;
    }
    if (visiblePanelIndices.empty())
    {
        UI::Label::Ptr empty = CreateCatalogLabel("NativePanelCatalog.Empty",
                                                  "No matching panels.",
                                                  theme,
                                                  theme.colors.textMuted);
        empty->SetPosition(padding, startY);
        empty->SetSize(rowWidth, rowHeight);
        context.contentContainer->AddChild(empty);
        return;
    }

    EditorListLayoutDesc listDesc;
    listDesc.context = &context;
    listDesc.namePrefix = "NativePanelCatalog";
    listDesc.bounds = UI::Rect(padding, startY, rowWidth, listHeight);
    listDesc.rowHeight = rowHeight;
    listDesc.contentHeight =
        static_cast<float>(visiblePanelIndices.size()) * rowHeight;
    listDesc.scrollOffsetY = m_scrollOffsetY;
    listDesc.onScrollChanged = [this](const Vec2& offset) {
        m_scrollOffsetY = offset.y;
    };

    EditorListLayoutFrame list =
        EditorListLayout::BeginScrollableList(std::move(listDesc));
    if (!list)
    {
        return;
    }

    m_lastBuildStats.hasListViewport = true;
    m_lastBuildStats.listViewportHeight = list.viewportHeight;
    m_lastBuildStats.listContentHeight = list.contentHeight;
    m_lastBuildStats.listScrollOffsetY = m_scrollOffsetY;

    const float toggleWidth = shellMetrics.panelControlHeight;
    const float columnGap = std::max(6.0f, shellMetrics.compactGap);
    const float rowPaddingX = 8.0f;
    const float rowPaddingY = 4.0f;
    const float actionX = std::max(rowPaddingX,
                                   rowWidth - toggleWidth - rowPaddingX);
    const float actionHeight = std::min(shellMetrics.panelControlHeight,
                                        std::max(18.0f, rowHeight - 8.0f));
    const float textWidth = std::max(60.0f,
                                     actionX - rowPaddingX - columnGap);

    for (size_t rowIndex = 0; rowIndex < visiblePanelIndices.size(); ++rowIndex)
    {
        const size_t snapshotIndex = visiblePanelIndices[rowIndex];
        if (snapshotIndex >= snapshots.size())
        {
            continue;
        }

        const EditorUIPanelSnapshot& snapshot = snapshots[snapshotIndex];
        const std::string rowName =
            "NativePanelCatalog.Row." + std::to_string(rowIndex);

        UI::Panel::Ptr row = UI::Panel::Create();
        row->SetName(rowName);
        row->SetPosition(0.0f, static_cast<float>(rowIndex) * rowHeight);
        row->SetSize(rowWidth, rowHeight);
        row->SetBackgroundColor(rowIndex % 2u == 0u
                                    ? theme.colors.panelBackground
                                    : theme.colors.windowBackground.WithAlpha(0.35f));
        row->SetBorderWidth(0.0f);
        row->SetInteractive(true);

        const std::string titleText =
            snapshot.title.empty() ? snapshot.id : snapshot.title;
        const std::string metadataText = BuildPanelMetadataText(snapshot);

        UI::Label::Ptr title = CreateCatalogLabel(rowName + ".Title",
                                                  titleText,
                                                  theme,
                                                  theme.colors.text);
        title->SetPosition(rowPaddingX, rowPaddingY);
        title->SetSize(textWidth, titleLineHeight);
        title->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        title->SetTooltipText(snapshot.id);
        row->AddChild(title);

        UI::Label::Ptr metadata = CreateCatalogLabel(
            rowName + ".Metadata",
            metadataText,
            theme,
            snapshot.rebuildState.pending ? theme.colors.warning
                                          : theme.colors.textMuted);
        EditorTypography::ApplyToLabel(*metadata,
                                       theme,
                                       EditorTypographyRole::Caption);
        metadata->SetPosition(rowPaddingX, rowPaddingY + titleLineHeight);
        metadata->SetSize(textWidth, metadataLineHeight);
        metadata->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        metadata->SetTooltipText(metadataText);
        row->AddChild(metadata);
        ++m_lastBuildStats.rowMetadataLabelCount;

        UI::Button::Ptr toggle = CreateCatalogIconButton(
            "NativePanelCatalog.Toggle." + snapshot.id,
            snapshot.visible ? "Hide" : "Show",
            snapshot.visible ? "eye-off" : "eye",
            theme,
            snapshot.visible);
        toggle->SetPosition(actionX, (rowHeight - actionHeight) * 0.5f);
        toggle->SetSize(actionHeight, actionHeight);
        toggle->SetEnabled(snapshot.closable || !snapshot.visible);
        toggle->SetTooltipText(std::string(snapshot.visible ? "Hide panel: " : "Show panel: ") +
                               titleText);
        toggle->SetOnClick([host = context.host,
                            panelId = snapshot.id,
                            visible = snapshot.visible](const UI::UIEvent& event) {
            (void)event;
            if (!host)
            {
                return;
            }

            host->SetPanelVisible(panelId, !visible);
            host->RequestPanelRebuild(NativePanelCatalogPanel::PanelId(),
                                      EditorUIPanelRebuildReason::Data);
        });
        row->AddChild(toggle);
        ++m_lastBuildStats.toggleButtonCount;
        ++m_lastBuildStats.toggleIconButtonCount;

        list.contentPanel->AddChild(row);
    }

    EditorListLayout::EndScrollableList(list);
}

} // namespace RVX::Editor
