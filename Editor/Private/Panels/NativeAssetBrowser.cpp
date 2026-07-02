/**
 * @file NativeAssetBrowser.cpp
 * @brief Native UI asset browser panel implementation
 */

#include "Editor/Panels/NativeAssetBrowser.h"

#include "Editor/EditorContext.h"
#include "Editor/UI/EditorContextMenu.h"
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

namespace RVX::Editor
{
namespace
{
    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    const char* AssetTypeLabel(Tools::AssetType type)
    {
        switch (type)
        {
            case Tools::AssetType::Texture:
                return "Texture";
            case Tools::AssetType::Mesh:
                return "Mesh";
            case Tools::AssetType::Material:
                return "Material";
            case Tools::AssetType::Shader:
                return "Shader";
            case Tools::AssetType::Animation:
                return "Animation";
            case Tools::AssetType::Audio:
                return "Audio";
            case Tools::AssetType::Font:
                return "Font";
            case Tools::AssetType::Prefab:
                return "Prefab";
            case Tools::AssetType::Scene:
                return "Scene";
            case Tools::AssetType::Script:
                return "Script";
            case Tools::AssetType::Unknown:
                break;
        }
        return "Unknown";
    }

    UI::Button::Ptr CreateAssetButton(const std::string& name,
                                      const std::string& text,
                                      const UI::UITheme& theme,
                                      bool active = false)
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        EditorPanelButtonStyleDesc styleDesc;
        styleDesc.active = active;
        styleDesc.overflowMode = UI::TextOverflowMode::Clip;
        styleDesc.setTooltipFromText = false;
        EditorPanelButtonStyle::Apply(*button, theme, styleDesc);
        return button;
    }

    float EstimateAssetButtonWidth(const std::string& text,
                                   const UI::UITheme& theme,
                                   float minWidth,
                                   float maxWidth)
    {
        return EditorTypography::EstimatePaddedTextWidth(text,
                                                         theme,
                                                         EditorTypographyRole::Control,
                                                         minWidth,
                                                         maxWidth);
    }

    UI::Label::Ptr CreateAssetLabel(const std::string& name,
                                    const std::string& text,
                                    const UI::UITheme& theme,
                                    const UI::UIColor& color)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::ListItem));
        label->SetTextColor(color);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetInteractive(false);
        return label;
    }
}

NativeAssetBrowserPanel::NativeAssetBrowserPanel()
{
    m_desc.id = "native.assetBrowser";
    m_desc.title = "Assets";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Bottom;
    m_desc.visibleByDefault = true;
    m_desc.closable = true;
}

void NativeAssetBrowserPanel::BuildUI(EditorUIPanelFrameContext& context)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    m_lastBuildStats = {};
    EnsureAssetSnapshots(context);

    AddToolbar(context);
    AddAssetRows(context);
}

bool NativeAssetBrowserPanel::SelectAssetGuid(const Tools::AssetGUID& guid)
{
    if (!guid.IsValid())
    {
        return false;
    }

    const Tools::AssetEntry* entry = EditorContext::Get().GetAssetDatabase().GetAsset(guid);
    if (!entry)
    {
        return false;
    }

    EditorContext::Get().SelectAsset(guid);
    return true;
}

void NativeAssetBrowserPanel::AddToolbar(EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float buttonHeight = shellMetrics.panelControlHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const float refreshWidth =
        EstimateAssetButtonWidth("Refresh", theme, 82.0f, 128.0f);
    const float clearWidth =
        EstimateAssetButtonWidth("Clear", theme, 68.0f, 112.0f);
    const float searchWidth =
        std::max(80.0f, width - refreshWidth - clearWidth - padding * 2.0f);

    EditorTextInput::Ptr search = EditorTextInput::Create();
    search->SetName("NativeAssetBrowser.Search");
    search->ApplyTheme(theme);
    search->SetPlaceholder("Search assets");
    search->SetText(m_searchFilter);
    search->SetPosition(padding, padding);
    search->SetSize(searchWidth, buttonHeight);
    search->SetOnTextChanged([this](const std::string& text) {
        m_searchFilter = text;
        m_scrollOffsetY = 0.0f;
    });
    context.contentContainer->AddChild(search);

    UI::Button::Ptr refresh = CreateAssetButton("NativeAssetBrowser.Refresh",
                                                "Refresh",
                                                theme);
    refresh->SetPosition(padding + searchWidth + padding, padding);
    refresh->SetSize(refreshWidth, buttonHeight);
    refresh->SetOnClick([](const UI::UIEvent& event) {
        (void)event;
        EditorContext::Get().RefreshAssetDatabase();
    });
    context.contentContainer->AddChild(refresh);

    UI::Button::Ptr clear = CreateAssetButton("NativeAssetBrowser.ClearSelection",
                                              "Clear",
                                              theme);
    clear->SetPosition(padding + searchWidth + padding + refreshWidth + padding, padding);
    clear->SetSize(clearWidth, buttonHeight);
    clear->SetOnClick([](const UI::UIEvent& event) {
        (void)event;
        EditorContext::Get().ClearSelection();
    });
    context.contentContainer->AddChild(clear);
}

void NativeAssetBrowserPanel::AddAssetRows(EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.panelListRowHeight;
    const float startY = shellMetrics.singleRowPanelToolbarHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const float listHeight =
        std::max(0.0f, context.contentContainer->GetHeight() - startY - padding);

    if (m_visibleAssetIndices.empty())
    {
        const std::string text = m_lastBuildStats.totalAssetCount == 0u
                                     ? "No assets indexed."
                                     : "No matching assets.";
        UI::Label::Ptr empty = CreateAssetLabel("NativeAssetBrowser.Empty",
                                                text,
                                                theme,
                                                theme.colors.textMuted);
        empty->SetPosition(padding, startY);
        empty->SetSize(rowWidth, rowHeight);
        context.contentContainer->AddChild(empty);
        return;
    }

    EditorListLayoutDesc listDesc;
    listDesc.context = &context;
    listDesc.namePrefix = "NativeAssetBrowser";
    listDesc.bounds = UI::Rect(padding, startY, rowWidth, listHeight);
    listDesc.rowHeight = rowHeight;
    listDesc.contentHeight =
        static_cast<float>(m_visibleAssetIndices.size()) * rowHeight;
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

    const float typeWidth = 84.0f;
    const float dirtyWidth = 48.0f;
    const float nameWidth = std::max(80.0f, rowWidth - typeWidth - dirtyWidth - 24.0f);
    const bool assetSelection =
        EditorContext::Get().GetSelectionType() == SelectionType::Asset;
    const Tools::AssetGUID selectedGuid =
        assetSelection ? EditorContext::Get().GetSelectedAsset()
                       : Tools::AssetGUID{};

    for (size_t index = 0; index < m_visibleAssetIndices.size(); ++index)
    {
        const size_t assetIndex = m_visibleAssetIndices[index];
        if (assetIndex >= m_assetSnapshots.size())
        {
            continue;
        }

        const NativeAssetBrowserAssetSnapshot& entry = m_assetSnapshots[assetIndex];
        const bool selected = selectedGuid == entry.guid;
        if (selected)
        {
            m_lastBuildStats.selectedAssetVisible = true;
        }

        UI::Panel::Ptr row = UI::Panel::Create();
        row->SetName("NativeAssetBrowser.Row." + entry.guid.ToString());
        row->SetPosition(0.0f, static_cast<float>(index) * rowHeight);
        row->SetSize(rowWidth, rowHeight);
        row->SetBackgroundColor(selected
                                    ? theme.colors.surfaceActive
                                    : ((index % 2u == 0u)
                                           ? theme.colors.panelBackground
                                           : theme.colors.windowBackground.WithAlpha(0.35f)));
        row->SetBorderColor(selected ? theme.colors.accent : UI::UIColor::Transparent());
        row->SetBorderWidth(selected ? 1.0f : 0.0f);
        row->SetOnClick([this, guid = entry.guid, host = context.host](const UI::UIEvent& event) {
            if (event.button == static_cast<int>(UI::UIMouseButton::Right))
            {
                SelectAssetGuid(guid);
                if (host)
                {
                    OpenAssetContextMenu(*host, guid, event.position);
                }
                return;
            }

            if (event.button == static_cast<int>(UI::UIMouseButton::Left))
            {
                SelectAssetGuid(guid);
            }
        });

        UI::Label::Ptr name = CreateAssetLabel(row->GetName() + ".Name",
                                               entry.path,
                                               theme,
                                               theme.colors.text);
        name->SetPosition(8.0f, 0.0f);
        name->SetSize(nameWidth, rowHeight);
        row->AddChild(name);

        UI::Label::Ptr type = CreateAssetLabel(row->GetName() + ".Type",
                                               AssetTypeLabel(entry.type),
                                               theme,
                                               theme.colors.textMuted);
        type->SetPosition(12.0f + nameWidth, 0.0f);
        type->SetSize(typeWidth, rowHeight);
        row->AddChild(type);

        UI::Label::Ptr dirty = CreateAssetLabel(row->GetName() + ".Dirty",
                                                entry.isDirty ? "Dirty" : "Ready",
                                                theme,
                                                entry.isDirty ? theme.colors.warning
                                                              : theme.colors.textMuted);
        dirty->SetPosition(12.0f + nameWidth + typeWidth, 0.0f);
        dirty->SetSize(dirtyWidth, rowHeight);
        row->AddChild(dirty);

        list.contentPanel->AddChild(row);
    }

    EditorListLayout::EndScrollableList(list);
}

void NativeAssetBrowserPanel::EnsureAssetSnapshots(EditorUIPanelFrameContext& context)
{
    EditorContext& editorContext = EditorContext::Get();
    const uint64 assetRevision = editorContext.GetAssetDatabaseRevision();

    const bool refreshAssets =
        m_assetSnapshotCache.NeedsRefresh(context, assetRevision);
    if (refreshAssets)
    {
        RefreshAssetSnapshot(context);
        m_visibleSnapshotCache.Invalidate();
    }

    const bool refreshVisible =
        m_visibleSnapshotCache.NeedsRefresh(context, assetRevision, m_searchFilter);
    if (refreshVisible)
    {
        RefreshVisibleAssetSnapshot(context);
    }

    m_lastBuildStats.totalAssetCount =
        static_cast<uint32>(m_assetSnapshots.size());
    m_lastBuildStats.visibleAssetCount =
        static_cast<uint32>(m_visibleAssetIndices.size());
    m_lastBuildStats.assetSnapshotRefreshCount =
        m_assetSnapshotCache.GetRefreshCount();
    m_lastBuildStats.visibleSnapshotRefreshCount =
        m_visibleSnapshotCache.GetRefreshCount();
    m_lastBuildStats.assetDatabaseRevision = assetRevision;
    m_lastBuildStats.reusedAssetSnapshot = !refreshAssets && m_assetSnapshotCache.IsValid();
    m_lastBuildStats.reusedVisibleSnapshot =
        !refreshVisible && m_visibleSnapshotCache.IsValid();

    for (const NativeAssetBrowserAssetSnapshot& asset : m_assetSnapshots)
    {
        if (asset.isDirty)
        {
            ++m_lastBuildStats.dirtyAssetCount;
        }
    }
}

void NativeAssetBrowserPanel::OpenAssetContextMenu(EditorUIHost& host,
                                                   const Tools::AssetGUID& guid,
                                                   const Vec2& anchor)
{
    if (!guid.IsValid())
    {
        return;
    }

    EditorContextMenuDesc menu;
    menu.id = "NativeAssetBrowser.Asset";
    menu.anchor = anchor;
    menu.minWidth = 170.0f;
    menu.items.push_back(EditorContextMenuItem::Action(
        "asset.select",
        "Select",
        [this, guid](EditorUIHost&) {
            SelectAssetGuid(guid);
        }));
    menu.items.push_back(EditorContextMenuItem::Action(
        "asset.clearSelection",
        "Clear Selection",
        [](EditorUIHost&) {
            EditorContext::Get().ClearSelection();
        }));
    menu.items.push_back(EditorContextMenuItem::Separator("asset.separator"));
    menu.items.push_back(EditorContextMenuItem::Action(
        "asset.refresh",
        "Refresh Database",
        [](EditorUIHost&) {
            EditorContext::Get().RefreshAssetDatabase();
        }));

    host.OpenContextMenu(std::move(menu));
}

void NativeAssetBrowserPanel::RefreshAssetSnapshot(EditorUIPanelFrameContext& context)
{
    const auto& databaseAssets = EditorContext::Get().GetAssetDatabase().GetAllAssets();
    m_assetSnapshots.clear();
    m_assetSnapshots.reserve(databaseAssets.size());

    for (const auto& [hash, entry] : databaseAssets)
    {
        (void)hash;
        NativeAssetBrowserAssetSnapshot snapshot;
        snapshot.guid = entry.guid;
        snapshot.path = entry.path;
        snapshot.name = entry.name;
        snapshot.type = entry.type;
        snapshot.isDirty = entry.isDirty;
        m_assetSnapshots.push_back(std::move(snapshot));
    }

    m_assetSnapshotCache.MarkRefreshed(
        context,
        EditorContext::Get().GetAssetDatabaseRevision());
}

void NativeAssetBrowserPanel::RefreshVisibleAssetSnapshot(EditorUIPanelFrameContext& context)
{
    m_visibleAssetIndices.clear();
    const std::string filter = ToLower(m_searchFilter);
    m_visibleAssetIndices.reserve(m_assetSnapshots.size());

    for (size_t index = 0; index < m_assetSnapshots.size(); ++index)
    {
        const NativeAssetBrowserAssetSnapshot& entry = m_assetSnapshots[index];
        if (!filter.empty())
        {
            const std::string haystack =
                ToLower(entry.path + " " + entry.name + " " + AssetTypeLabel(entry.type));
            if (haystack.find(filter) == std::string::npos)
            {
                continue;
            }
        }
        m_visibleAssetIndices.push_back(index);
    }

    std::sort(m_visibleAssetIndices.begin(),
              m_visibleAssetIndices.end(),
              [this](size_t lhsIndex, size_t rhsIndex) {
        if (lhsIndex >= m_assetSnapshots.size() || rhsIndex >= m_assetSnapshots.size())
        {
            return lhsIndex < rhsIndex;
        }
        const NativeAssetBrowserAssetSnapshot& lhs = m_assetSnapshots[lhsIndex];
        const NativeAssetBrowserAssetSnapshot& rhs = m_assetSnapshots[rhsIndex];
        if (lhs.type != rhs.type)
        {
            return static_cast<uint8>(lhs.type) < static_cast<uint8>(rhs.type);
        }
        return lhs.path < rhs.path;
    });

    m_visibleSnapshotCache.MarkRefreshed(
        context,
        EditorContext::Get().GetAssetDatabaseRevision(),
        m_searchFilter);
}

} // namespace RVX::Editor
