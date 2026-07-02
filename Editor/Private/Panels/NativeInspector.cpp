/**
 * @file NativeInspector.cpp
 * @brief Native UI inspector panel implementation
 */

#include "Editor/Panels/NativeInspector.h"

#include "Core/Serialization/PropertyReflection.h"
#include "Editor/UI/EditorCheckbox.h"
#include "Editor/UI/EditorContextMenu.h"
#include "Editor/UI/EditorFormLayout.h"
#include "Editor/UI/EditorPanelButtonStyle.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTextInput.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "Scene/ActorComponent.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace RVX::Editor
{
namespace
{
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

    UI::Label::Ptr CreateInspectorLabel(const std::string& name,
                                        const std::string& text,
                                        const UI::UITheme& theme,
                                        const UI::UIColor& color)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::PropertyLabel));
        label->SetTextColor(color);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        label->SetTooltipText(text);
        label->SetInteractive(false);
        return label;
    }

    UI::Button::Ptr CreateInspectorButton(const std::string& name,
                                          const std::string& text,
                                          const UI::UITheme& theme)
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        EditorPanelButtonStyle::Apply(*button, theme);
        return button;
    }

    std::string FormatVec3(const Vec3& value)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2)
               << value.x << ", " << value.y << ", " << value.z;
        return stream.str();
    }

    std::string FormatQuat(const Quat& value)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2)
               << value.w << ", " << value.x << ", " << value.y << ", " << value.z;
        return stream.str();
    }

    SceneEntity* ResolveEntity(SceneManager* sceneManager, SceneEntity::Handle handle)
    {
        return sceneManager ? sceneManager->GetEntity(handle) : nullptr;
    }

    std::string MakeComponentPrefix(const ClassDescriptor& descriptor, uint32 componentIndex)
    {
        return "NativeInspector.Component." + std::to_string(componentIndex) + "." +
               descriptor.GetName() + ".Property";
    }

    std::string MakeActorComponentPrefix(const ClassDescriptor& descriptor,
                                         uint32 componentIndex)
    {
        return "NativeInspector.ActorComponent." + std::to_string(componentIndex) + "." +
               descriptor.GetName() + ".Property";
    }

    std::string PropertyDisplayName(const Property& property)
    {
        return property.GetMeta().displayName.empty()
                   ? property.GetName()
                   : property.GetMeta().displayName;
    }

    std::string ComponentRootWidgetName(const std::string& widgetPrefix,
                                        const std::string& fallbackName)
    {
        if (widgetPrefix.empty())
        {
            return fallbackName.empty() ? "NativeInspector.Component.Unknown" : fallbackName;
        }

        constexpr const char* propertySuffix = ".Property";
        const size_t suffixPosition = widgetPrefix.rfind(propertySuffix);
        if (suffixPosition != std::string::npos &&
            suffixPosition + std::char_traits<char>::length(propertySuffix) ==
                widgetPrefix.size())
        {
            return widgetPrefix.substr(0, suffixPosition);
        }
        return widgetPrefix;
    }

    bool EntityHasComponentClass(const SceneEntity& entity, const std::string& className)
    {
        if (className.empty())
        {
            return false;
        }

        for (const auto& component : entity.GetActorComponents())
        {
            if (component && component->GetClassName() == className)
            {
                return true;
            }
        }

        for (const auto& [typeIndex, component] : entity.GetComponents())
        {
            (void)typeIndex;
            if (component && component->GetTypeName() == className)
            {
                return true;
            }
        }

        return false;
    }

    bool CanAddComponentClass(const SceneEntity& entity,
                              const ComponentFactory::ComponentClassDesc& desc)
    {
        return desc.allowMultiple || !EntityHasComponentClass(entity, desc.className);
    }

    std::string MakeAssetSnapshotKey(const Tools::AssetGUID& guid)
    {
        return guid.ToString();
    }

    void AppendComponentKeyPart(std::ostringstream& stream,
                                ActorComponent::ComponentId componentId,
                                const std::string& className,
                                const std::string& instanceName)
    {
        stream << componentId << '@'
               << className.size() << ':' << className << '#'
               << instanceName.size() << ':' << instanceName << ';';
    }
}

NativeInspectorPanel::NativeInspectorPanel()
{
    m_desc.id = "native.inspector";
    m_desc.title = "Inspector";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Right;
    m_desc.visibleByDefault = true;
    m_desc.closable = true;
}

std::string NativeInspectorPanel::MakeComponentExpansionKey(
    const ActorComponent& component,
    const std::string& componentRootName) const
{
    const Component* legacyComponent = dynamic_cast<const Component*>(&component);
    const SceneEntity* entity = nullptr;
    if (legacyComponent)
    {
        entity = legacyComponent->GetOwner();
    }
    else
    {
        entity = dynamic_cast<const SceneEntity*>(component.GetOwner());
    }

    const std::string ownerKey =
        entity ? std::to_string(entity->GetHandle()) : std::string("NoEntity");
    return ownerKey + "." + componentRootName;
}

bool NativeInspectorPanel::IsComponentExpanded(
    const ActorComponent& component,
    const std::string& componentRootName) const
{
    const auto it =
        m_componentExpansion.find(MakeComponentExpansionKey(component, componentRootName));
    return it == m_componentExpansion.end() ? true : it->second;
}

void NativeInspectorPanel::SetComponentExpanded(const ActorComponent& component,
                                                const std::string& componentRootName,
                                                bool expanded)
{
    m_componentExpansion[MakeComponentExpansionKey(component, componentRootName)] = expanded;
}

void NativeInspectorPanel::BuildUI(EditorUIPanelFrameContext& context)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    m_lastBuildStats = {};
    EditorContext& editorContext = EditorContext::Get();
    m_lastBuildStats.selectionType = editorContext.GetSelectionType();
    m_lastBuildStats.selectionRevision = editorContext.GetSelectionRevision();
    m_lastBuildStats.sceneRevision = editorContext.GetSceneRevision();
    m_lastBuildStats.assetDatabaseRevision =
        editorContext.GetAssetDatabaseRevision();
    if (m_lastBuildStats.selectionType != SelectionType::Entity)
    {
        m_addComponentPickerOpen = false;
        m_addComponentPickerFocusRequested = false;
        m_addComponentPickerModel.ClearItems();
        m_addComponentFilter.clear();
    }

    EditorFormLayoutDesc formDesc;
    formDesc.context = &context;
    formDesc.namePrefix = "NativeInspector";
    formDesc.scrollOffsetY = m_scrollOffsetY;
    formDesc.onScrollChanged = [this](const Vec2& offset) {
        m_scrollOffsetY = offset.y;
    };

    EditorFormLayoutFrame form =
        EditorFormLayout::BeginScrollableFrame(std::move(formDesc));
    if (!form)
    {
        return;
    }

    m_lastBuildStats.hasScrollViewport = true;
    m_lastBuildStats.scrollViewportHeight = form.metrics.viewportHeight;
    m_lastBuildStats.scrollOffsetY = m_scrollOffsetY;

    EditorUIPanelFrameContext contentContext = form.contentContext;

    float contentHeight = 0.0f;
    switch (editorContext.GetSelectionType())
    {
        case SelectionType::Entity:
        {
            SceneEntity* entity = editorContext.GetSelectedEntity();
            if (entity)
            {
                EnsureComponentSnapshots(contentContext, *entity);
                contentHeight = AddEntityInspector(contentContext, *entity);
                break;
            }
            contentHeight =
                AddEmptyState(contentContext, "Selected entity is no longer available.");
            break;
        }
        case SelectionType::Asset:
        {
            const Tools::AssetGUID guid = editorContext.GetSelectedAsset();
            EnsureAssetSnapshot(contentContext, guid);
            if (m_assetSnapshot.valid)
            {
                contentHeight = AddAssetInspector(contentContext, m_assetSnapshot);
                break;
            }
            contentHeight =
                AddEmptyState(contentContext, "Selected asset is no longer indexed.");
            break;
        }
        case SelectionType::Component:
        case SelectionType::None:
            break;
    }

    if (contentHeight <= 0.0f)
    {
        contentHeight = AddEmptyState(contentContext, "Select an object to inspect.");
    }

    m_lastBuildStats.scrollContentHeight =
        std::max(form.metrics.viewportHeight, contentHeight);
    EditorFormLayout::EndScrollableFrame(form, contentHeight);
}

void NativeInspectorPanel::EnsureAssetSnapshot(EditorUIPanelFrameContext& context,
                                               const Tools::AssetGUID& guid)
{
    const uint64 assetRevision = EditorContext::Get().GetAssetDatabaseRevision();
    const std::string key = MakeAssetSnapshotKey(guid);
    const bool refresh =
        m_assetSnapshotCache.NeedsRefresh(context, assetRevision, key);
    if (refresh)
    {
        RefreshAssetSnapshot(context, guid, key);
    }

    m_lastBuildStats.assetSnapshotRefreshCount =
        m_assetSnapshotCache.GetRefreshCount();
    m_lastBuildStats.reusedAssetSnapshot =
        !refresh && m_assetSnapshotCache.IsValid();
}

void NativeInspectorPanel::RefreshAssetSnapshot(EditorUIPanelFrameContext& context,
                                                const Tools::AssetGUID& guid,
                                                const std::string& key)
{
    m_assetSnapshot = {};
    m_assetSnapshot.guid = guid;

    const Tools::AssetEntry* asset =
        EditorContext::Get().GetAssetDatabase().GetAsset(guid);
    if (asset)
    {
        m_assetSnapshot.guid = asset->guid;
        m_assetSnapshot.name = asset->name;
        m_assetSnapshot.path = asset->path;
        m_assetSnapshot.type = asset->type;
        m_assetSnapshot.isDirty = asset->isDirty;
        m_assetSnapshot.valid = true;
    }

    m_assetSnapshotCache.MarkRefreshed(
        context,
        EditorContext::Get().GetAssetDatabaseRevision(),
        key);
}

void NativeInspectorPanel::EnsureComponentSnapshots(EditorUIPanelFrameContext& context,
                                                    SceneEntity& entity)
{
    const uint64 selectionRevision = EditorContext::Get().GetSelectionRevision();
    const std::string key = MakeComponentSnapshotKey(entity);
    const bool refresh =
        m_componentSnapshotCache.NeedsRefresh(context, selectionRevision, key);
    if (refresh)
    {
        RefreshComponentSnapshots(context, entity, key);
    }

    m_lastBuildStats.componentSnapshotCount =
        static_cast<uint32>(m_legacyComponentSnapshots.size() +
                            m_actorComponentSnapshots.size());
    m_lastBuildStats.componentSnapshotRefreshCount =
        m_componentSnapshotCache.GetRefreshCount();
    m_lastBuildStats.reusedComponentSnapshot =
        !refresh && m_componentSnapshotCache.IsValid();
}

void NativeInspectorPanel::RefreshComponentSnapshots(EditorUIPanelFrameContext& context,
                                                     SceneEntity& entity,
                                                     const std::string& key)
{
    m_legacyComponentSnapshots.clear();
    m_actorComponentSnapshots.clear();

    m_legacyComponentSnapshots.reserve(entity.GetComponents().size());
    for (const auto& [typeIndex, component] : entity.GetComponents())
    {
        (void)typeIndex;
        if (!component)
        {
            continue;
        }

        NativeInspectorComponentSnapshot snapshot;
        snapshot.componentId = component->GetComponentId();
        snapshot.className = component->GetTypeName();
        snapshot.instanceName = component->GetName();
        m_legacyComponentSnapshots.push_back(std::move(snapshot));
    }

    std::sort(m_legacyComponentSnapshots.begin(),
              m_legacyComponentSnapshots.end(),
              [](const NativeInspectorComponentSnapshot& lhs,
                 const NativeInspectorComponentSnapshot& rhs) {
                  if (lhs.className != rhs.className)
                  {
                      return lhs.className < rhs.className;
                  }
                  return lhs.instanceName < rhs.instanceName;
              });

    m_actorComponentSnapshots.reserve(entity.GetActorComponents().size());
    for (const auto& component : entity.GetActorComponents())
    {
        if (!component)
        {
            continue;
        }

        NativeInspectorComponentSnapshot snapshot;
        snapshot.componentId = component->GetComponentId();
        snapshot.className = component->GetClassName();
        snapshot.instanceName = component->GetName();
        m_actorComponentSnapshots.push_back(std::move(snapshot));
    }

    std::sort(m_actorComponentSnapshots.begin(),
              m_actorComponentSnapshots.end(),
              [](const NativeInspectorComponentSnapshot& lhs,
                 const NativeInspectorComponentSnapshot& rhs) {
                  if (lhs.className != rhs.className)
                  {
                      return lhs.className < rhs.className;
                  }
                  return lhs.instanceName < rhs.instanceName;
              });

    m_componentSnapshotCache.MarkRefreshed(
        context,
        EditorContext::Get().GetSelectionRevision(),
        key);
}

std::string NativeInspectorPanel::MakeComponentSnapshotKey(
    const SceneEntity& entity) const
{
    std::ostringstream stream;
    stream << entity.GetHandle() << "|L" << entity.GetComponentCount() << '|';
    for (const auto& [typeIndex, component] : entity.GetComponents())
    {
        (void)typeIndex;
        if (component)
        {
            AppendComponentKeyPart(stream,
                                   component->GetComponentId(),
                                   component->GetTypeName(),
                                   component->GetName());
        }
    }

    stream << "|A" << entity.GetActorComponentCount() << '|';
    for (const auto& component : entity.GetActorComponents())
    {
        if (component)
        {
            AppendComponentKeyPart(stream,
                                   component->GetComponentId(),
                                   component->GetClassName(),
                                   component->GetName());
        }
    }
    return stream.str();
}

Component* NativeInspectorPanel::ResolveLegacyComponentSnapshot(
    SceneEntity& entity,
    const NativeInspectorComponentSnapshot& snapshot) const
{
    Component* classMatch = nullptr;
    for (const auto& [typeIndex, component] : entity.GetComponents())
    {
        (void)typeIndex;
        if (!component)
        {
            continue;
        }

        if (snapshot.componentId != ActorComponent::InvalidComponentId &&
            component->GetComponentId() == snapshot.componentId)
        {
            return component.get();
        }

        if (component->GetTypeName() != snapshot.className)
        {
            continue;
        }

        if (component->GetName() == snapshot.instanceName)
        {
            return component.get();
        }
        if (!classMatch)
        {
            classMatch = component.get();
        }
    }
    return classMatch;
}

ActorComponent* NativeInspectorPanel::ResolveActorComponentSnapshot(
    SceneEntity& entity,
    const NativeInspectorComponentSnapshot& snapshot) const
{
    ActorComponent* classMatch = nullptr;
    for (const auto& component : entity.GetActorComponents())
    {
        if (!component)
        {
            continue;
        }

        if (snapshot.componentId != ActorComponent::InvalidComponentId &&
            component->GetComponentId() == snapshot.componentId)
        {
            return component.get();
        }

        if (component->GetClassName() != snapshot.className)
        {
            continue;
        }

        if (component->GetName() == snapshot.instanceName)
        {
            return component.get();
        }
        if (!classMatch)
        {
            classMatch = component.get();
        }
    }
    return classMatch;
}

float NativeInspectorPanel::AddEmptyState(EditorUIPanelFrameContext& context,
                                          const std::string& text)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formLineHeight;

    UI::Label::Ptr empty = CreateInspectorLabel("NativeInspector.Empty",
                                                text,
                                                theme,
                                                theme.colors.textMuted);
    empty->SetPosition(padding, padding);
    empty->SetSize(std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f),
                   rowHeight);
    context.contentContainer->AddChild(empty);
    return padding + rowHeight + padding;
}

float NativeInspectorPanel::AddEntityInspector(EditorUIPanelFrameContext& context,
                                               SceneEntity& entity)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formRowHeight;
    const float labelHeight = shellMetrics.formLineHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    float y = padding;

    m_lastBuildStats.hasEntity = true;
    m_lastBuildStats.componentCount = static_cast<uint32>(entity.GetComponentCount());
    m_lastBuildStats.actorComponentCount =
        static_cast<uint32>(entity.GetActorComponentCount());

    const float activeWidth = 72.0f;
    EditorCheckbox::Ptr active = EditorCheckbox::Create("Active");
    active->SetName("NativeInspector.Entity.Active");
    active->ApplyTheme(theme);
    active->SetChecked(entity.IsActive());
    active->SetPosition(padding, y);
    active->SetSize(activeWidth, rowHeight);
    SceneManager* sceneManager = entity.GetSceneManager();
    const SceneEntity::Handle handle = entity.GetHandle();
    active->SetOnCheckedChanged([sceneManager, handle](bool checked) {
        if (SceneEntity* current = ResolveEntity(sceneManager, handle))
        {
            EditorContext::Get().SetEntityActiveUndoable(current, checked);
        }
    });
    context.contentContainer->AddChild(active);

    EditorTextInput::Ptr name = EditorTextInput::Create();
    name->SetName("NativeInspector.Entity.Name");
    name->ApplyTheme(theme);
    name->SetPlaceholder("Entity name");
    name->SetText(entity.GetName());
    name->SetPosition(padding + activeWidth + padding, y);
    name->SetSize(std::max(60.0f, width - activeWidth - padding), rowHeight);
    name->SetOnTextChanged([sceneManager, handle](const std::string& text) {
        if (SceneEntity* current = ResolveEntity(sceneManager, handle))
        {
            EditorContext::Get().SetEntityNameUndoable(current, text);
        }
    });
    context.contentContainer->AddChild(name);
    y += rowHeight + padding;

    UI::Label::Ptr classLabel =
        CreateInspectorLabel("NativeInspector.Entity.Class",
                             std::string("Class: ") + entity.GetClassName(),
                             theme,
                             theme.colors.textMuted);
    classLabel->SetPosition(padding, y);
    classLabel->SetSize(width, labelHeight);
    context.contentContainer->AddChild(classLabel);
    y += labelHeight;

    UI::Label::Ptr handleLabel =
        CreateInspectorLabel("NativeInspector.Entity.Handle",
                             "Handle: " + std::to_string(entity.GetHandle()),
                             theme,
                             theme.colors.textMuted);
    handleLabel->SetPosition(padding, y);
    handleLabel->SetSize(width, labelHeight);
    context.contentContainer->AddChild(handleLabel);
    y += labelHeight + padding;

    UI::Label::Ptr transformHeader =
        CreateInspectorLabel("NativeInspector.Entity.Transform.Header",
                             "Transform",
                             theme,
                             theme.colors.text);
    transformHeader->SetPosition(padding, y);
    transformHeader->SetSize(width, labelHeight);
    context.contentContainer->AddChild(transformHeader);
    y += labelHeight;

    UI::Label::Ptr position =
        CreateInspectorLabel("NativeInspector.Entity.Transform.Position",
                             "Position: " + FormatVec3(entity.GetPosition()),
                             theme,
                             theme.colors.textMuted);
    position->SetPosition(padding, y);
    position->SetSize(width, labelHeight);
    context.contentContainer->AddChild(position);
    y += labelHeight;

    UI::Label::Ptr rotation =
        CreateInspectorLabel("NativeInspector.Entity.Transform.Rotation",
                             "Rotation: " + FormatQuat(entity.GetRotation()),
                             theme,
                             theme.colors.textMuted);
    rotation->SetPosition(padding, y);
    rotation->SetSize(width, labelHeight);
    context.contentContainer->AddChild(rotation);
    y += labelHeight;

    UI::Label::Ptr scale =
        CreateInspectorLabel("NativeInspector.Entity.Transform.Scale",
                             "Scale: " + FormatVec3(entity.GetScale()),
                             theme,
                             theme.colors.textMuted);
    scale->SetPosition(padding, y);
    scale->SetSize(width, labelHeight);
    context.contentContainer->AddChild(scale);
    y += labelHeight + padding;

    UI::Label::Ptr componentHeader =
        CreateInspectorLabel("NativeInspector.Entity.Components.Header",
                             "Components",
                             theme,
                             theme.colors.text);
    componentHeader->SetPosition(padding, y);
    componentHeader->SetSize(width, labelHeight);
    context.contentContainer->AddChild(componentHeader);
    y += labelHeight;

    UI::Label::Ptr legacyCount =
        CreateInspectorLabel("NativeInspector.Entity.Components.LegacyCount",
                             "Legacy: " + std::to_string(m_lastBuildStats.componentCount),
                             theme,
                             theme.colors.textMuted);
    legacyCount->SetPosition(padding, y);
    legacyCount->SetSize(width, labelHeight);
    context.contentContainer->AddChild(legacyCount);
    y += labelHeight;

    UI::Label::Ptr actorCount =
        CreateInspectorLabel("NativeInspector.Entity.Components.ActorCount",
                             "Actor: " +
                                 std::to_string(m_lastBuildStats.actorComponentCount),
                             theme,
                             theme.colors.textMuted);
    actorCount->SetPosition(padding, y);
    actorCount->SetSize(width, labelHeight);
    context.contentContainer->AddChild(actorCount);
    y += labelHeight + padding;

    uint32 componentIndex = 0;
    for (const NativeInspectorComponentSnapshot& snapshot : m_legacyComponentSnapshots)
    {
        Component* component = ResolveLegacyComponentSnapshot(entity, snapshot);
        if (component)
        {
            AddLegacyComponentInspector(context, *component, y, componentIndex);
        }
        ++componentIndex;
    }

    uint32 actorComponentIndex = 0;
    for (const NativeInspectorComponentSnapshot& snapshot : m_actorComponentSnapshots)
    {
        ActorComponent* component = ResolveActorComponentSnapshot(entity, snapshot);
        if (component)
        {
            AddActorComponentInspector(context, *component, y, actorComponentIndex);
        }
        ++actorComponentIndex;
    }

    AddComponentActions(context, entity, y);
    return y;
}

float NativeInspectorPanel::AddAssetInspector(EditorUIPanelFrameContext& context,
                                              const NativeInspectorAssetSnapshot& asset)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formRowHeight;
    const float labelHeight = shellMetrics.formLineHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    float y = padding;

    m_lastBuildStats.hasAsset = true;

    UI::Label::Ptr title = CreateInspectorLabel("NativeInspector.Asset.Name",
                                                asset.name.empty() ? asset.path : asset.name,
                                                theme,
                                                theme.colors.text);
    title->SetPosition(padding, y);
    title->SetSize(width, rowHeight);
    context.contentContainer->AddChild(title);
    y += rowHeight + padding;

    UI::Label::Ptr type = CreateInspectorLabel("NativeInspector.Asset.Type",
                                               std::string("Type: ") +
                                                   AssetTypeLabel(asset.type),
                                               theme,
                                               theme.colors.textMuted);
    type->SetPosition(padding, y);
    type->SetSize(width, labelHeight);
    context.contentContainer->AddChild(type);
    y += labelHeight;

    UI::Label::Ptr path = CreateInspectorLabel("NativeInspector.Asset.Path",
                                               "Path: " + asset.path,
                                               theme,
                                               theme.colors.textMuted);
    path->SetPosition(padding, y);
    path->SetSize(width, labelHeight);
    context.contentContainer->AddChild(path);
    y += labelHeight;

    UI::Label::Ptr guid = CreateInspectorLabel("NativeInspector.Asset.Guid",
                                               "GUID: " + asset.guid.ToString(),
                                               theme,
                                               theme.colors.textMuted);
    guid->SetPosition(padding, y);
    guid->SetSize(width, labelHeight);
    context.contentContainer->AddChild(guid);
    y += labelHeight;

    UI::Label::Ptr status =
        CreateInspectorLabel("NativeInspector.Asset.Status",
                             asset.isDirty ? "Status: Dirty" : "Status: Ready",
                             theme,
                             asset.isDirty ? theme.colors.warning : theme.colors.textMuted);
    status->SetPosition(padding, y);
    status->SetSize(width, labelHeight);
    context.contentContainer->AddChild(status);
    y += labelHeight + padding;

    UI::Button::Ptr clear = CreateInspectorButton("NativeInspector.Asset.ClearSelection",
                                                  "Clear Selection",
                                                  theme);
    clear->SetPosition(padding, y);
    clear->SetSize(std::min(132.0f, width), rowHeight);
    clear->SetOnClick([](const UI::UIEvent& event) {
        (void)event;
        EditorContext::Get().ClearSelection();
    });
    context.contentContainer->AddChild(clear);
    y += rowHeight + padding;
    return y;
}

void NativeInspectorPanel::AddComponentActions(EditorUIPanelFrameContext& context,
                                               SceneEntity& entity,
                                               float& y)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formRowHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);

    UI::Button::Ptr addButton = CreateInspectorButton("NativeInspector.Entity.AddComponent",
                                                       m_addComponentPickerOpen
                                                           ? "Close Component Picker"
                                                           : "Add Component",
                                                       theme);
    addButton->SetPosition(padding, y);
    addButton->SetSize(width, rowHeight);
    addButton->SetEnabled(context.host != nullptr);

    if (context.host)
    {
        addButton->SetOnClick([this](const UI::UIEvent& event) {
            (void)event;
            m_addComponentPickerOpen = !m_addComponentPickerOpen;
            m_addComponentPickerModel.ClearSelection();
            if (!m_addComponentPickerOpen)
            {
                m_addComponentFilter.clear();
                m_addComponentPickerFocusRequested = false;
            }
            else
            {
                m_addComponentPickerFocusRequested = true;
            }
        });
    }

    context.contentContainer->AddChild(addButton);
    y += rowHeight + padding;

    if (m_addComponentPickerOpen)
    {
        AddComponentPicker(context, entity, y);
    }
}

void NativeInspectorPanel::AddComponentPicker(EditorUIPanelFrameContext& context,
                                              SceneEntity& entity,
                                              float& y)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formRowHeight;
    const float labelHeight = shellMetrics.formLineHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    std::vector<ComponentFactory::ComponentClassDesc> entries =
        ComponentFactory::GetRegisteredComponentClassDescs();

    std::vector<EditorPickerFilterItemDesc> filterItems;
    filterItems.reserve(entries.size());
    for (const ComponentFactory::ComponentClassDesc& entry : entries)
    {
        const std::string displayName =
            entry.displayName.empty() ? entry.className : entry.displayName;
        EditorPickerFilterItemDesc filterItem;
        filterItem.id = entry.className;
        filterItem.text = displayName;
        filterItem.category = entry.category;
        filterItem.keywords.push_back(entry.className);
        filterItem.keywords.push_back(entry.category);
        filterItem.enabled = CanAddComponentClass(entity, entry);
        filterItems.push_back(std::move(filterItem));
    }

    EditorPickerFilterOptions filterOptions;
    filterOptions.groupByCategory = true;
    filterOptions.sortMatchesByScore = true;
    m_addComponentPickerFilterModel.SetOptions(filterOptions);
    m_addComponentPickerFilterModel.SetItems(std::move(filterItems));
    m_addComponentPickerFilterModel.SetFilterText(m_addComponentFilter);
    m_addComponentPickerFilterModel.Rebuild();

    std::vector<ComponentFactory::ComponentClassDesc> visibleEntries;
    const std::vector<EditorPickerFilterResult>& filterResults =
        m_addComponentPickerFilterModel.GetResults();
    visibleEntries.reserve(filterResults.size());
    for (const EditorPickerFilterResult& result : filterResults)
    {
        if (result.sourceIndex < entries.size())
        {
            visibleEntries.push_back(entries[result.sourceIndex]);
        }
    }

    SceneManager* sceneManager = entity.GetSceneManager();
    const SceneEntity::Handle handle = entity.GetHandle();

    std::vector<EditorPickerListItemState> pickerItems;
    pickerItems.reserve(visibleEntries.size());
    for (const ComponentFactory::ComponentClassDesc& entry : visibleEntries)
    {
        pickerItems.push_back(
            {entry.className, CanAddComponentClass(entity, entry)});
    }
    m_addComponentPickerModel.SetItems(std::move(pickerItems));

    std::function<bool(uint32)> handlePickerKey =
        [this, sceneManager, handle, visibleEntries](uint32 keyCode) {
            if (keyCode == UI::RVX_UI_KEY_ESCAPE)
            {
                m_addComponentPickerOpen = false;
                m_addComponentFilter.clear();
                m_addComponentPickerModel.ClearItems();
                m_addComponentPickerFocusRequested = false;
                return true;
            }

            auto* current = ResolveEntity(sceneManager, handle);
            auto isEnabledIndex = [current, &visibleEntries](int32 index) {
                if (!current || index < 0 ||
                    static_cast<size_t>(index) >= visibleEntries.size())
                {
                    return false;
                }
                return CanAddComponentClass(*current,
                                            visibleEntries[static_cast<size_t>(index)]);
            };

            if (keyCode == UI::RVX_UI_KEY_DOWN)
            {
                m_addComponentPickerModel.MoveSelection(1);
                return true;
            }
            if (keyCode == UI::RVX_UI_KEY_UP)
            {
                m_addComponentPickerModel.MoveSelection(-1);
                return true;
            }
            if (keyCode == UI::RVX_UI_KEY_ENTER || keyCode == UI::RVX_UI_KEY_SPACE)
            {
                m_addComponentPickerModel.NormalizeSelection();
                const int32 selectedIndex =
                    m_addComponentPickerModel.GetSelectedIndex();
                if (current && isEnabledIndex(selectedIndex))
                {
                    const std::string className =
                        visibleEntries[static_cast<size_t>(selectedIndex)]
                            .className;
                    if (EditorContext::Get().AddComponentUndoable(current, className))
                    {
                        m_addComponentPickerOpen = false;
                        m_addComponentFilter.clear();
                        m_addComponentPickerModel.ClearItems();
                        m_addComponentPickerFocusRequested = false;
                    }
                }
                return true;
            }

            return false;
        };

    std::vector<EditorPickerListViewItemDesc> pickerViewItems;
    pickerViewItems.reserve(visibleEntries.size());
    for (const ComponentFactory::ComponentClassDesc& entry : visibleEntries)
    {
        const bool canAddToEntity = CanAddComponentClass(entity, entry);
        const bool canClick = context.host != nullptr && canAddToEntity;
        const std::string displayName =
            entry.displayName.empty() ? entry.className : entry.displayName;

        EditorPickerListViewItemDesc itemDesc;
        itemDesc.id = entry.className;
        itemDesc.text = canAddToEntity ? displayName
                                       : displayName + " (Already added)";
        itemDesc.category = entry.category;
        itemDesc.enabled = canClick;
        if (canClick)
        {
            itemDesc.onClick = [this,
                                sceneManager,
                                handle,
                                className = entry.className]() {
                if (SceneEntity* current = ResolveEntity(sceneManager, handle))
                {
                    if (!EditorContext::Get().AddComponentUndoable(current, className))
                    {
                        return;
                    }
                }
                m_addComponentPickerOpen = false;
                m_addComponentFilter.clear();
                m_addComponentPickerModel.ClearItems();
                m_addComponentPickerFocusRequested = false;
            };
        }
        pickerViewItems.push_back(std::move(itemDesc));
    }

    const float preferredMaxListHeight = rowHeight * 8.0f + labelHeight * 3.0f;
    const float availableHeight =
        std::max(rowHeight, context.contentContainer->GetHeight() - y - padding);
    const float maxListHeight = std::min(preferredMaxListHeight, availableHeight);

    EditorPickerListViewDesc pickerDesc;
    pickerDesc.ui = context.ui;
    pickerDesc.parent = context.contentContainer;
    pickerDesc.model = &m_addComponentPickerModel;
    pickerDesc.name = "NativeInspector.Entity.AddComponentPicker";
    pickerDesc.bounds = UI::Rect(padding, y, width, 0.0f);
    pickerDesc.searchText = m_addComponentFilter;
    pickerDesc.searchPlaceholder = "Search components";
    pickerDesc.emptyText = entries.empty() ? "No registered components"
                                           : "No matching components";
    pickerDesc.items = std::move(pickerViewItems);
    pickerDesc.onSearchChanged = [this](const std::string& text) {
        m_addComponentFilter = text;
        m_addComponentPickerModel.ClearSelection();
        m_addComponentPickerView.SetScrollOffsetY(0.0f);
    };
    pickerDesc.onKeyDown = handlePickerKey;
    pickerDesc.padding = padding;
    pickerDesc.rowHeight = rowHeight;
    pickerDesc.categoryHeight = labelHeight;
    pickerDesc.maxListHeight = maxListHeight;
    m_addComponentPickerView.Build(pickerDesc);

    if (m_addComponentPickerFocusRequested && context.host)
    {
        context.host->RequestFocusAfterBuild("NativeInspector.Entity.AddComponentPicker");
        m_addComponentPickerFocusRequested = false;
    }
    y += m_addComponentPickerView.GetLastBuildStats().bounds.height + padding;
}

void NativeInspectorPanel::AddLegacyComponentInspector(EditorUIPanelFrameContext& context,
                                                       Component& component,
                                                       float& y,
                                                       uint32 componentIndex)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;

    ClassDescriptor* descriptor = ReflectionRegistry::Get().GetClass(typeid(component));
    const std::string componentName =
        descriptor ? descriptor->GetName() : component.GetTypeName();
    const std::string widgetPrefix = descriptor ? MakeComponentPrefix(*descriptor, componentIndex)
                                                : std::string{};
    AddReflectedComponentInspector(context, component, componentName, widgetPrefix, y);
    y += padding;
}

void NativeInspectorPanel::AddActorComponentInspector(EditorUIPanelFrameContext& context,
                                                      ActorComponent& component,
                                                      float& y,
                                                      uint32 componentIndex)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;

    ClassDescriptor* descriptor = ReflectionRegistry::Get().GetClass(typeid(component));
    const std::string componentName =
        descriptor ? descriptor->GetName() : component.GetClassName();
    const std::string widgetPrefix =
        descriptor ? MakeActorComponentPrefix(*descriptor, componentIndex) : std::string{};
    AddReflectedComponentInspector(context, component, componentName, widgetPrefix, y);
    y += padding;
}

void NativeInspectorPanel::AddReflectedComponentInspector(EditorUIPanelFrameContext& context,
                                                          ActorComponent& component,
                                                          const std::string& componentName,
                                                          const std::string& widgetPrefix,
                                                          float& y)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float labelHeight = shellMetrics.formLineHeight;
    const float rowHeight = shellMetrics.formRowHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    ClassDescriptor* descriptor = ReflectionRegistry::Get().GetClass(typeid(component));

    const std::string componentRootName = ComponentRootWidgetName(widgetPrefix, componentName);
    const bool expanded = IsComponentExpanded(component, componentRootName);
    const float checkboxWidth = std::min(22.0f, rowHeight);
    const float foldoutWidth = std::min(24.0f, rowHeight);
    const float menuWidth = std::min(30.0f, std::max(24.0f, rowHeight));
    const float gap = shellMetrics.compactGap;

    EditorCheckbox::Ptr enabled = EditorCheckbox::Create();
    enabled->SetName(componentRootName + ".Enabled");
    enabled->ApplyTheme(theme);
    enabled->SetChecked(component.IsEnabled());
    enabled->SetEnabled(context.host != nullptr);
    enabled->SetPosition(padding, y);
    enabled->SetSize(checkboxWidth, rowHeight);
    if (context.host)
    {
        enabled->SetOnCheckedChanged([component = &component](bool checked) {
            EditorContext::Get().SetComponentEnabledUndoable(component, checked);
        });
    }
    context.contentContainer->AddChild(enabled);

    UI::Button::Ptr foldoutButton = CreateInspectorButton(componentRootName + ".Foldout",
                                                          expanded ? "v" : ">",
                                                          theme);
    foldoutButton->SetPosition(padding + checkboxWidth + gap, y);
    foldoutButton->SetSize(foldoutWidth, rowHeight);
    foldoutButton->SetEnabled(context.host != nullptr);
    if (context.host)
    {
        foldoutButton->SetOnClick([this,
                                   component = &component,
                                   componentRootName,
                                   expanded](const UI::UIEvent& event) {
            (void)event;
            SetComponentExpanded(*component, componentRootName, !expanded);
        });
    }
    context.contentContainer->AddChild(foldoutButton);

    UI::Label::Ptr header = CreateInspectorLabel(componentRootName + ".Header",
                                                 componentName,
                                                 theme,
                                                 theme.colors.text);
    const float headerX = padding + checkboxWidth + foldoutWidth + gap * 2.0f;
    header->SetPosition(headerX, y);
    header->SetSize(std::max(0.0f,
                             width - checkboxWidth - foldoutWidth - menuWidth -
                                 gap * 3.0f),
                    rowHeight);
    context.contentContainer->AddChild(header);

    UI::Button::Ptr menuButton = CreateInspectorButton(componentRootName + ".Menu",
                                                       "...",
                                                       theme);
    menuButton->SetPosition(padding + width - menuWidth, y);
    menuButton->SetSize(menuWidth, rowHeight);
    menuButton->SetEnabled(context.host != nullptr);
    if (context.host)
    {
        menuButton->SetOnClick([host = context.host,
                                component = &component,
                                componentRootName](const UI::UIEvent& event) {
            if (!host)
            {
                return;
            }

            EditorContextMenuDesc menu;
            menu.id = componentRootName + ".Menu";
            menu.anchor = event.position;
            menu.minWidth = 160.0f;
            menu.maxWidth = 260.0f;
            menu.focusOnOpen = true;

            const bool canRemove = EditorContext::Get().CanRemoveComponent(component);
            menu.items.push_back(EditorContextMenuItem::Action(
                "component.remove",
                "Remove Component",
                [component](EditorUIHost& itemHost) {
                    (void)itemHost;
                    EditorContext::Get().RemoveComponentUndoable(component);
                },
                canRemove));

            host->OpenContextMenu(std::move(menu));
        });
    }
    context.contentContainer->AddChild(menuButton);
    y += rowHeight;

    if (!expanded)
    {
        return;
    }

    if (!descriptor)
    {
        UI::Label::Ptr missing =
            CreateInspectorLabel(componentRootName + ".NoReflection",
                                 "No reflection info available.",
                                 theme,
                                 theme.colors.textMuted);
        missing->SetPosition(padding, y);
        missing->SetSize(width, labelHeight);
        context.contentContainer->AddChild(missing);
        y += labelHeight;
        return;
    }

    for (const Property& property : descriptor->GetProperties())
    {
        if (property.IsHidden())
        {
            continue;
        }

        ++m_lastBuildStats.reflectedPropertyCount;
        EditorPropertyDrawerContext drawerContext;
        drawerContext.host = context.host;
        drawerContext.ui = context.ui;
        drawerContext.canvas = context.canvas;
        drawerContext.parent = context.contentContainer;
        drawerContext.property = &property;
        drawerContext.instance = &component;
        drawerContext.widgetPrefix = widgetPrefix;
        drawerContext.x = padding;
        drawerContext.y = y;
        drawerContext.width = width;
        drawerContext.rowHeight = rowHeight;
        drawerContext.labelWidth = std::min(112.0f, width * 0.42f);

        const bool drawn =
            context.host && context.host->GetPropertyDrawerRegistry().DrawProperty(drawerContext);
        if (drawn)
        {
            ++m_lastBuildStats.drawnPropertyCount;
            y += std::max(rowHeight, drawerContext.consumedHeight) +
                 shellMetrics.compactGap;
            continue;
        }

        ++m_lastBuildStats.unsupportedPropertyCount;
        UI::Label::Ptr unsupported =
            CreateInspectorLabel(widgetPrefix + ".Unsupported." + property.GetName(),
                                 PropertyDisplayName(property) + ": Unsupported",
                                 theme,
                                 theme.colors.textMuted);
        unsupported->SetPosition(padding, y);
        unsupported->SetSize(width, labelHeight);
        context.contentContainer->AddChild(unsupported);
        y += labelHeight + shellMetrics.compactGap;
    }
}

} // namespace RVX::Editor
