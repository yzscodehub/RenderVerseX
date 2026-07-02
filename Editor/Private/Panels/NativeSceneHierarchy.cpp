/**
 * @file NativeSceneHierarchy.cpp
 * @brief Native UI scene hierarchy panel implementation
 */

#include "Editor/Panels/NativeSceneHierarchy.h"

#include "Editor/EditorContext.h"
#include "Editor/UI/EditorContextMenu.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTextInput.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Editor
{
namespace
{
    UI::Label::Ptr CreateHierarchyLabel(const std::string& name,
                                        const std::string& text,
                                        const UI::UITheme& theme,
                                        const UI::UIColor& textColor)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::ListItem));
        label->SetTextColor(textColor);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetInteractive(false);
        return label;
    }

    std::vector<SceneEntity*> CollectRootEntities(SceneManager& sceneManager)
    {
        std::vector<SceneEntity*> roots;
        sceneManager.ForEachEntity([&roots](SceneEntity* entity) {
            if (entity && entity->IsRoot())
            {
                roots.push_back(entity);
            }
        });
        std::sort(roots.begin(), roots.end(), [](const SceneEntity* lhs, const SceneEntity* rhs) {
            if (lhs->GetName() != rhs->GetName())
            {
                return lhs->GetName() < rhs->GetName();
            }
            return lhs->GetHandle() < rhs->GetHandle();
        });
        return roots;
    }

    std::string NormalizeFilter(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return text;
    }

    std::string MakeSceneSnapshotKey(const SceneManager* sceneManager, size_t entityCount)
    {
        return std::to_string(reinterpret_cast<std::uintptr_t>(sceneManager)) +
               ":" + std::to_string(entityCount);
    }

    bool SnapshotNameMatchesFilter(const NativeSceneHierarchyNodeSnapshot& snapshot,
                                   const std::string& normalizedFilter)
    {
        if (normalizedFilter.empty())
        {
            return true;
        }

        std::string name = NormalizeFilter(snapshot.name);
        return name.find(normalizedFilter) != std::string::npos;
    }
}

NativeSceneHierarchyPanel::NativeSceneHierarchyPanel()
{
    m_desc.id = "native.sceneHierarchy";
    m_desc.title = "Hierarchy";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Left;
    m_desc.visibleByDefault = true;
    m_desc.closable = true;
}

void NativeSceneHierarchyPanel::BuildUI(EditorUIPanelFrameContext& context)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    m_lastBuildStats = {};
    EnsureSceneSnapshots(context);

    AddToolbar(context);
    AddSceneRows(context);
}

void NativeSceneHierarchyPanel::AddToolbar(EditorUIPanelFrameContext& context)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float buttonHeight = shellMetrics.panelControlHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const bool hasScene = EditorContext::Get().GetActiveSceneManager() != nullptr;

    if (hasScene)
    {
        const float buttonWidth = std::min(98.0f, std::max(72.0f, width * 0.38f));
        const float searchWidth = std::max(60.0f, width - buttonWidth - padding);
        EditorTextInput::Ptr search = EditorTextInput::Create();
        search->SetName("NativeSceneHierarchy.Search");
        search->ApplyTheme(theme);
        search->SetPlaceholder("Search entities");
        search->SetText(m_searchFilter);
        search->SetPosition(padding, padding);
        search->SetSize(searchWidth, buttonHeight);
        search->SetOnTextChanged([this](const std::string& text) {
            m_searchFilter = text;
            m_treeList.SetScrollOffsetRows(0u);
        });
        context.contentContainer->AddChild(search);

        UI::Button::Ptr create = UI::Button::Create("Create");
        create->SetName("NativeSceneHierarchy.CreateEmpty");
        create->SetPosition(padding + searchWidth + padding, padding);
        create->SetSize(buttonWidth, buttonHeight);
        create->GetStyle().fontSize = EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::Control);
        create->GetStyle().textColor = theme.colors.text;
        create->SetNormalColor(theme.colors.surface);
        create->SetHoverColor(theme.colors.surfaceHover);
        create->SetPressedColor(theme.colors.accent.WithAlpha(0.75f));
        create->SetDisabledColor(theme.colors.surface.WithAlpha(0.35f));
        create->SetOnClick([](const UI::UIEvent& event) {
            (void)event;
            if (SceneEntity* entity = EditorContext::Get().CreateEntityUndoable("Entity"))
            {
                EditorContext::Get().SelectEntity(entity);
            }
        });
        context.contentContainer->AddChild(create);
        return;
    }

    UI::Button::Ptr create = UI::Button::Create(hasScene ? "Create Empty" : "New Scene");
    create->SetName(hasScene ? "NativeSceneHierarchy.CreateEmpty"
                             : "NativeSceneHierarchy.NewScene");
    create->SetPosition(padding, padding);
    create->SetSize(width, buttonHeight);
    create->GetStyle().fontSize = EditorTypography::GetFontSize(
        theme,
        EditorTypographyRole::Control);
    create->GetStyle().textColor = theme.colors.text;
    create->SetNormalColor(theme.colors.surface);
    create->SetHoverColor(theme.colors.surfaceHover);
    create->SetPressedColor(theme.colors.accent.WithAlpha(0.75f));
    create->SetDisabledColor(theme.colors.surface.WithAlpha(0.35f));
    create->SetOnClick([hasScene](const UI::UIEvent& event) {
        (void)event;
        if (!hasScene)
        {
            EditorContext::Get().NewScene();
            return;
        }

    });
    context.contentContainer->AddChild(create);
}

void NativeSceneHierarchyPanel::AddSceneRows(EditorUIPanelFrameContext& context)
{
    auto& editorContext = EditorContext::Get();
    SceneManager* sceneManager = editorContext.GetActiveSceneManager();
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.compactPanelListRowHeight;
    const float startY = shellMetrics.singleRowPanelToolbarHeight;
    const float rowWidth =
        std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    const float listHeight =
        std::max(0.0f, context.contentContainer->GetHeight() - startY - padding);

    if (!sceneManager)
    {
        UI::Label::Ptr empty = CreateHierarchyLabel("NativeSceneHierarchy.NoScene",
                                                    "No scene loaded.",
                                                    theme,
                                                    theme.colors.textMuted);
        empty->SetPosition(padding, startY);
        empty->SetSize(rowWidth, rowHeight);
        context.contentContainer->AddChild(empty);
        return;
    }

    if (m_sceneSnapshots.empty())
    {
        UI::Label::Ptr empty = CreateHierarchyLabel("NativeSceneHierarchy.Empty",
                                                    "No entities.",
                                                    theme,
                                                    theme.colors.textMuted);
        empty->SetPosition(padding, startY);
        empty->SetSize(rowWidth, rowHeight);
        context.contentContainer->AddChild(empty);
        return;
    }

    if (m_visibleSnapshotIndices.empty())
    {
        UI::Label::Ptr empty = CreateHierarchyLabel("NativeSceneHierarchy.NoMatches",
                                                    "No matching entities.",
                                                    theme,
                                                    theme.colors.textMuted);
        empty->SetPosition(padding, startY);
        empty->SetSize(rowWidth, rowHeight);
        context.contentContainer->AddChild(empty);
        return;
    }

    auto isSelectedHandle = [&editorContext](uint32 handle) {
        if (editorContext.GetSelectionType() != SelectionType::Entity)
        {
            return false;
        }

        for (const SceneEntity* selected : editorContext.GetSelectedEntities())
        {
            if (selected && selected->GetHandle() == handle)
            {
                return true;
            }
        }
        return false;
    };

    std::vector<EditorTreeListRowDesc> rows;
    rows.reserve(m_visibleSnapshotIndices.size());
    for (size_t snapshotIndex : m_visibleSnapshotIndices)
    {
        if (snapshotIndex >= m_sceneSnapshots.size())
        {
            continue;
        }

        const NativeSceneHierarchyNodeSnapshot& snapshot =
            m_sceneSnapshots[snapshotIndex];
        EditorTreeListRowDesc row;
        row.id = std::to_string(snapshot.handle);
        row.text = snapshot.name.empty() ? "Entity" : snapshot.name;
        row.depth = snapshot.depth;
        row.selected = isSelectedHandle(snapshot.handle);
        row.muted = !snapshot.active;
        row.expandable = snapshot.expandable;
        row.expanded = true;
        if (row.selected)
        {
            m_lastBuildStats.selectedRowVisible = true;
        }

        const uint32 handle = snapshot.handle;
        row.onClick = [sceneManager, handle]() {
            if (sceneManager)
            {
                if (SceneEntity* entity = sceneManager->GetEntity(handle))
                {
                    EditorContext::Get().SelectEntity(entity);
                }
            }
        };
        if (context.host)
        {
            row.onContextMenu = [this, host = context.host, sceneManager, handle](
                                    const Vec2& anchor) {
                if (!host || !sceneManager)
                {
                    return;
                }
                SceneEntity* entity = sceneManager->GetEntity(handle);
                if (!entity)
                {
                    return;
                }
                EditorContext::Get().SelectEntity(entity);
                OpenEntityContextMenu(*host, *entity, anchor);
            };
        }
        rows.push_back(std::move(row));
    }

    EditorTreeListBuildDesc listDesc;
    listDesc.ui = context.ui;
    listDesc.parent = context.contentContainer;
    listDesc.name = "NativeSceneHierarchy";
    listDesc.bounds = UI::Rect(padding, startY, rowWidth, listHeight);
    listDesc.rowHeight = rowHeight;
    listDesc.indentWidth = 14.0f;
    listDesc.rows = std::move(rows);
    m_treeList.Build(listDesc);
}

void NativeSceneHierarchyPanel::EnsureSceneSnapshots(EditorUIPanelFrameContext& context)
{
    EditorContext& editorContext = EditorContext::Get();
    SceneManager* sceneManager = editorContext.GetActiveSceneManager();
    const uint64 sceneRevision = editorContext.GetSceneRevision();
    const size_t entityCount = sceneManager ? sceneManager->GetEntityCount() : 0u;
    m_sceneSnapshotKey = MakeSceneSnapshotKey(sceneManager, entityCount);

    const bool refreshScene =
        m_sceneSnapshotCache.NeedsRefresh(context, sceneRevision, m_sceneSnapshotKey);
    if (refreshScene)
    {
        RefreshSceneSnapshot(context);
        m_visibleSnapshotCache.Invalidate();
    }

    const std::string visibleKey = m_sceneSnapshotKey + "|" + m_searchFilter;
    const bool refreshVisible =
        m_visibleSnapshotCache.NeedsRefresh(context, sceneRevision, visibleKey);
    if (refreshVisible)
    {
        RefreshVisibleSceneSnapshot(context);
    }

    m_lastBuildStats.totalEntityCount =
        static_cast<uint32>(m_sceneSnapshots.size());
    m_lastBuildStats.visibleRowCount =
        static_cast<uint32>(m_visibleSnapshotIndices.size());
    m_lastBuildStats.sceneSnapshotRefreshCount =
        m_sceneSnapshotCache.GetRefreshCount();
    m_lastBuildStats.visibleSnapshotRefreshCount =
        m_visibleSnapshotCache.GetRefreshCount();
    m_lastBuildStats.sceneRevision = sceneRevision;
    m_lastBuildStats.selectionRevision = editorContext.GetSelectionRevision();
    m_lastBuildStats.reusedSceneSnapshot =
        !refreshScene && m_sceneSnapshotCache.IsValid();
    m_lastBuildStats.reusedVisibleSnapshot =
        !refreshVisible && m_visibleSnapshotCache.IsValid();
}

void NativeSceneHierarchyPanel::RefreshSceneSnapshot(EditorUIPanelFrameContext& context)
{
    m_sceneSnapshots.clear();

    SceneManager* sceneManager = EditorContext::Get().GetActiveSceneManager();
    if (sceneManager)
    {
        std::vector<SceneEntity*> roots = CollectRootEntities(*sceneManager);
        m_sceneSnapshots.reserve(sceneManager->GetEntityCount());
        for (SceneEntity* root : roots)
        {
            AppendEntitySnapshot(root, 0u);
        }
    }

    m_sceneSnapshotCache.MarkRefreshed(
        context,
        EditorContext::Get().GetSceneRevision(),
        m_sceneSnapshotKey);
}

void NativeSceneHierarchyPanel::AppendEntitySnapshot(SceneEntity* entity, uint32 depth)
{
    if (!entity)
    {
        return;
    }

    NativeSceneHierarchyNodeSnapshot snapshot;
    snapshot.handle = entity->GetHandle();
    snapshot.name = entity->GetName();
    snapshot.depth = depth;
    snapshot.active = entity->IsActive();
    snapshot.expandable = entity->GetChildCount() > 0u;
    m_sceneSnapshots.push_back(std::move(snapshot));

    std::vector<SceneEntity*> children = entity->GetChildren();
    std::sort(children.begin(), children.end(), [](const SceneEntity* lhs, const SceneEntity* rhs) {
        if (lhs->GetName() != rhs->GetName())
        {
            return lhs->GetName() < rhs->GetName();
        }
        return lhs->GetHandle() < rhs->GetHandle();
    });
    for (SceneEntity* child : children)
    {
        AppendEntitySnapshot(child, depth + 1u);
    }
}

void NativeSceneHierarchyPanel::RefreshVisibleSceneSnapshot(
    EditorUIPanelFrameContext& context)
{
    m_visibleSnapshotIndices.clear();
    const std::string normalizedFilter = NormalizeFilter(m_searchFilter);
    m_visibleSnapshotIndices.reserve(m_sceneSnapshots.size());

    for (size_t index = 0; index < m_sceneSnapshots.size(); ++index)
    {
        if (SnapshotSubtreeMatchesFilter(index, normalizedFilter))
        {
            m_visibleSnapshotIndices.push_back(index);
        }
    }

    m_visibleSnapshotCache.MarkRefreshed(
        context,
        EditorContext::Get().GetSceneRevision(),
        m_sceneSnapshotKey + "|" + m_searchFilter);
}

bool NativeSceneHierarchyPanel::SnapshotSubtreeMatchesFilter(
    size_t index,
    const std::string& normalizedFilter) const
{
    if (index >= m_sceneSnapshots.size())
    {
        return false;
    }

    if (SnapshotNameMatchesFilter(m_sceneSnapshots[index], normalizedFilter))
    {
        return true;
    }

    const uint32 depth = m_sceneSnapshots[index].depth;
    for (size_t childIndex = index + 1u; childIndex < m_sceneSnapshots.size(); ++childIndex)
    {
        const NativeSceneHierarchyNodeSnapshot& child = m_sceneSnapshots[childIndex];
        if (child.depth <= depth)
        {
            break;
        }
        if (SnapshotNameMatchesFilter(child, normalizedFilter))
        {
            return true;
        }
    }

    return false;
}

void NativeSceneHierarchyPanel::OpenEntityContextMenu(EditorUIHost& host,
                                                      SceneEntity& entity,
                                                      const Vec2& anchor)
{
    SceneManager* sceneManager = entity.GetSceneManager();
    if (!sceneManager)
    {
        return;
    }

    const SceneEntity::Handle handle = entity.GetHandle();
    const bool nextActive = !entity.IsActive();

    EditorContextMenuDesc menu;
    menu.id = "NativeSceneHierarchy.Entity";
    menu.anchor = anchor;
    menu.minWidth = 170.0f;
    menu.items.push_back(EditorContextMenuItem::Action(
        "entity.createChild",
        "Create Child",
        [sceneManager, handle](EditorUIHost&) {
            SceneEntity* parent = sceneManager->GetEntity(handle);
            if (!parent)
            {
                return;
            }

            if (SceneEntity* child =
                    EditorContext::Get().CreateEntityUndoable("Entity", parent))
            {
                EditorContext::Get().SelectEntity(child);
            }
        }));
    menu.items.push_back(EditorContextMenuItem::Action(
        nextActive ? "entity.activate" : "entity.deactivate",
        nextActive ? "Activate" : "Deactivate",
        [sceneManager, handle, nextActive](EditorUIHost&) {
            if (SceneEntity* target = sceneManager->GetEntity(handle))
            {
                EditorContext::Get().SetEntityActiveUndoable(target, nextActive);
            }
        }));
    menu.items.push_back(EditorContextMenuItem::Separator("entity.separator"));
    menu.items.push_back(EditorContextMenuItem::Action(
        "entity.delete",
        "Delete",
        [sceneManager, handle](EditorUIHost&) {
            if (SceneEntity* target = sceneManager->GetEntity(handle))
            {
                EditorContext::Get().DestroyEntityUndoable(target);
            }
        }));

    host.OpenContextMenu(std::move(menu));
}

} // namespace RVX::Editor
