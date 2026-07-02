/**
 * @file EditorPropertyDrawerRegistry.cpp
 * @brief Native editor property drawer registry implementation
 */

#include "Editor/UI/EditorPropertyDrawerRegistry.h"

#include "Core/MathTypes.h"
#include "Editor/EditorContext.h"
#include "Editor/UI/EditorCheckbox.h"
#include "Editor/UI/EditorComboBox.h"
#include "Editor/UI/EditorContextMenu.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTextInput.h"
#include "Editor/UI/EditorTypography.h"
#include "Editor/UI/EditorUIHost.h"
#include "Tools/AssetDatabase.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <array>
#include <any>
#include <iomanip>
#include <sstream>
#include <vector>

namespace RVX::Editor
{
namespace
{
    std::string PropertyDisplayName(const Property& property)
    {
        return property.GetMeta().displayName.empty()
                   ? property.GetName()
                   : property.GetMeta().displayName;
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

    std::string MakeWidgetName(const EditorPropertyDrawerContext& context,
                               const char* role)
    {
        const std::string prefix = context.widgetPrefix.empty()
                                       ? "Editor.Property"
                                       : context.widgetPrefix;
        return prefix + "." + role + "." + context.property->GetName();
    }

    UI::Label::Ptr CreatePropertyLabel(const std::string& name,
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

    void AddPropertyNameLabel(EditorPropertyDrawerContext& context,
                              const UI::UITheme& theme)
    {
        const float labelWidth = std::max(54.0f, context.labelWidth);
        UI::Label::Ptr label =
            CreatePropertyLabel(MakeWidgetName(context, "Label"),
                                PropertyDisplayName(*context.property),
                                theme,
                                context.property->IsReadOnly() ? theme.colors.textMuted
                                                               : theme.colors.text);
        label->SetPosition(context.x, context.y);
        label->SetSize(labelWidth, context.rowHeight);
        context.parent->AddChild(label);
    }

    UI::Rect ControlRect(const EditorPropertyDrawerContext& context,
                         const UI::UITheme& theme)
    {
        const EditorShellMetrics shellMetrics =
            EditorShellMetricPolicy::Resolve(theme);
        const float labelWidth = std::max(54.0f, context.labelWidth);
        const float gap = shellMetrics.compactGap;
        const float controlX = context.x + labelWidth + gap;
        return UI::Rect(controlX,
                        context.y,
                        std::max(0.0f, context.width - labelWidth - gap),
                        context.rowHeight);
    }

    bool PreparePropertyRow(EditorPropertyDrawerContext& context)
    {
        if (!context.parent || !context.ui || !context.property || !context.instance)
        {
            return false;
        }

        context.consumedHeight = std::max(20.0f, context.rowHeight);
        return true;
    }

    template<typename ValueType>
    bool TryGetPropertyValue(const Property& property, void* instance, ValueType& value)
    {
        try
        {
            value = property.GetValue<ValueType>(instance);
            return true;
        }
        catch (const std::bad_any_cast&)
        {
            return false;
        }
    }

    template<typename ValueType>
    bool CommitPropertyValue(const Property& property,
                             void* instance,
                             const ValueType& value)
    {
        return EditorContext::Get().SetReflectedPropertyUndoable(
            property,
            instance,
            value,
            "Edit " + property.GetName());
    }

    std::string FormatFloat(float value)
    {
        std::ostringstream stream;
        stream << std::setprecision(6) << value;
        return stream.str();
    }

    std::string FormatDouble(double value)
    {
        std::ostringstream stream;
        stream << std::setprecision(9) << value;
        return stream.str();
    }

    bool ParseInt32Strict(const std::string& text, int32& value)
    {
        try
        {
            size_t parsed = 0;
            const int parsedValue = std::stoi(text, &parsed);
            if (parsed != text.size())
            {
                return false;
            }
            value = static_cast<int32>(parsedValue);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseInt64Strict(const std::string& text, int64& value)
    {
        try
        {
            size_t parsed = 0;
            const long long parsedValue = std::stoll(text, &parsed);
            if (parsed != text.size())
            {
                return false;
            }
            value = static_cast<int64>(parsedValue);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseFloatStrict(const std::string& text, float& value)
    {
        try
        {
            size_t parsed = 0;
            const float parsedValue = std::stof(text, &parsed);
            if (parsed != text.size())
            {
                return false;
            }
            value = parsedValue;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseDoubleStrict(const std::string& text, double& value)
    {
        try
        {
            size_t parsed = 0;
            const double parsedValue = std::stod(text, &parsed);
            if (parsed != text.size())
            {
                return false;
            }
            value = parsedValue;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool DrawBoolProperty(EditorPropertyDrawerContext& context)
    {
        if (!PreparePropertyRow(context))
        {
            return false;
        }

        bool value = false;
        if (!TryGetPropertyValue(*context.property, context.instance, value))
        {
            return false;
        }

        const UI::UITheme& theme = context.ui->GetTheme();
        AddPropertyNameLabel(context, theme);
        const UI::Rect control = ControlRect(context, theme);

        EditorCheckbox::Ptr checkbox = EditorCheckbox::Create();
        checkbox->SetName(MakeWidgetName(context, "Field"));
        checkbox->ApplyTheme(theme);
        checkbox->SetChecked(value);
        checkbox->SetEnabled(!context.property->IsReadOnly());
        checkbox->SetPosition(control.x, control.y);
        checkbox->SetSize(std::min(24.0f, control.width), control.height);
        checkbox->SetOnCheckedChanged([property = *context.property,
                                       instance = context.instance](bool checked) {
            CommitPropertyValue(property, instance, checked);
        });
        context.parent->AddChild(checkbox);
        return true;
    }

    template<typename ValueType, typename FormatFn, typename ParseFn>
    bool DrawTextBackedProperty(EditorPropertyDrawerContext& context,
                                FormatFn formatValue,
                                ParseFn parseValue)
    {
        if (!PreparePropertyRow(context))
        {
            return false;
        }

        ValueType value{};
        if (!TryGetPropertyValue(*context.property, context.instance, value))
        {
            return false;
        }

        const UI::UITheme& theme = context.ui->GetTheme();
        AddPropertyNameLabel(context, theme);
        const UI::Rect control = ControlRect(context, theme);

        EditorTextInput::Ptr input = EditorTextInput::Create();
        input->SetName(MakeWidgetName(context, "Field"));
        input->ApplyTheme(theme);
        input->SetPlaceholder(PropertyDisplayName(*context.property));
        input->SetText(formatValue(value));
        input->SetPosition(control.x, control.y);
        input->SetSize(control.width, control.height);
        input->SetInteractive(!context.property->IsReadOnly());
        if (!context.property->IsReadOnly())
        {
            input->SetOnTextChanged([property = *context.property,
                                     instance = context.instance,
                                     parseValue](const std::string& text) {
                ValueType parsedValue{};
                if (parseValue(text, parsedValue))
                {
                    CommitPropertyValue(property, instance, parsedValue);
                }
            });
        }
        context.parent->AddChild(input);
        return true;
    }

    bool DrawStringProperty(EditorPropertyDrawerContext& context)
    {
        return DrawTextBackedProperty<std::string>(
            context,
            [](const std::string& value) {
                return value;
            },
            [](const std::string& text, std::string& value) {
                value = text;
                return true;
            });
    }

    bool DrawInt32Property(EditorPropertyDrawerContext& context)
    {
        return DrawTextBackedProperty<int32>(
            context,
            [](int32 value) {
                return std::to_string(value);
            },
            ParseInt32Strict);
    }

    bool DrawInt64Property(EditorPropertyDrawerContext& context)
    {
        return DrawTextBackedProperty<int64>(
            context,
            [](int64 value) {
                return std::to_string(value);
            },
            ParseInt64Strict);
    }

    bool DrawFloatProperty(EditorPropertyDrawerContext& context)
    {
        return DrawTextBackedProperty<float>(context, FormatFloat, ParseFloatStrict);
    }

    bool DrawDoubleProperty(EditorPropertyDrawerContext& context)
    {
        return DrawTextBackedProperty<double>(context, FormatDouble, ParseDoubleStrict);
    }

    template<typename ValueType>
    float GetVectorComponent(const ValueType& value, uint32 index)
    {
        return value[static_cast<int>(index)];
    }

    float GetVectorComponent(const Quat& value, uint32 index)
    {
        switch (index)
        {
            case 0:
                return value.w;
            case 1:
                return value.x;
            case 2:
                return value.y;
            case 3:
                return value.z;
            default:
                break;
        }
        return 0.0f;
    }

    template<typename ValueType>
    void SetVectorComponent(ValueType& value, uint32 index, float component)
    {
        value[static_cast<int>(index)] = component;
    }

    void SetVectorComponent(Quat& value, uint32 index, float component)
    {
        switch (index)
        {
            case 0:
                value.w = component;
                break;
            case 1:
                value.x = component;
                break;
            case 2:
                value.y = component;
                break;
            case 3:
                value.z = component;
                break;
            default:
                break;
        }
    }

    template<typename ValueType, size_t ComponentCount>
    bool DrawVectorProperty(EditorPropertyDrawerContext& context,
                            const std::array<const char*, ComponentCount>& labels)
    {
        if (!PreparePropertyRow(context))
        {
            return false;
        }

        ValueType value{};
        if (!TryGetPropertyValue(*context.property, context.instance, value))
        {
            return false;
        }

        const UI::UITheme& theme = context.ui->GetTheme();
        AddPropertyNameLabel(context, theme);
        const UI::Rect control = ControlRect(context, theme);
        const EditorShellMetrics shellMetrics =
            EditorShellMetricPolicy::Resolve(theme);
        const float gap = shellMetrics.compactGap;
        const float fieldWidth =
            std::max(0.0f,
                     (control.width - gap * static_cast<float>(ComponentCount - 1u)) /
                         static_cast<float>(ComponentCount));

        const std::string fieldPrefix = MakeWidgetName(context, "Field");
        for (size_t componentIndex = 0; componentIndex < ComponentCount; ++componentIndex)
        {
            EditorTextInput::Ptr input = EditorTextInput::Create();
            input->SetName(fieldPrefix + "." + labels[componentIndex]);
            input->ApplyTheme(theme);
            input->SetPlaceholder(labels[componentIndex]);
            input->SetText(FormatFloat(GetVectorComponent(value,
                                                          static_cast<uint32>(componentIndex))));
            input->SetPosition(control.x + static_cast<float>(componentIndex) *
                                             (fieldWidth + gap),
                               control.y);
            input->SetSize(fieldWidth, control.height);
            input->SetInteractive(!context.property->IsReadOnly());
            if (!context.property->IsReadOnly())
            {
                input->SetOnTextChanged([property = *context.property,
                                         instance = context.instance,
                                         componentIndex](const std::string& text) {
                    float parsedValue = 0.0f;
                    if (!ParseFloatStrict(text, parsedValue))
                    {
                        return;
                    }

                    ValueType currentValue{};
                    if (!TryGetPropertyValue(property, instance, currentValue))
                    {
                        return;
                    }

                    SetVectorComponent(currentValue,
                                       static_cast<uint32>(componentIndex),
                                       parsedValue);
                    CommitPropertyValue(property, instance, currentValue);
                });
            }
            context.parent->AddChild(input);
        }
        return true;
    }

    bool DrawVec2Property(EditorPropertyDrawerContext& context)
    {
        return DrawVectorProperty<Vec2, 2>(context, {"X", "Y"});
    }

    bool DrawVec3Property(EditorPropertyDrawerContext& context)
    {
        return DrawVectorProperty<Vec3, 3>(context, {"X", "Y", "Z"});
    }

    bool DrawVec4Property(EditorPropertyDrawerContext& context)
    {
        return DrawVectorProperty<Vec4, 4>(context, {"X", "Y", "Z", "W"});
    }

    bool DrawColorProperty(EditorPropertyDrawerContext& context)
    {
        return DrawVectorProperty<Vec4, 4>(context, {"R", "G", "B", "A"});
    }

    bool DrawQuatProperty(EditorPropertyDrawerContext& context)
    {
        return DrawVectorProperty<Quat, 4>(context, {"W", "X", "Y", "Z"});
    }

    bool DrawEnumProperty(EditorPropertyDrawerContext& context)
    {
        if (!PreparePropertyRow(context))
        {
            return false;
        }

        int32 selectedIndex = -1;
        if (!TryGetPropertyValue(*context.property, context.instance, selectedIndex))
        {
            return false;
        }

        const std::vector<std::string> enumValues = context.property->GetMeta().enumValues;
        if (enumValues.empty())
        {
            return false;
        }

        const UI::UITheme& theme = context.ui->GetTheme();
        AddPropertyNameLabel(context, theme);
        const UI::Rect control = ControlRect(context, theme);

        EditorComboBox::Ptr combo = EditorComboBox::Create();
        combo->SetName(MakeWidgetName(context, "Field"));
        combo->ApplyTheme(theme);
        combo->SetOptions(enumValues);
        combo->SetSelectedIndex(selectedIndex);
        combo->SetPlaceholder("Invalid");
        combo->SetPosition(control.x, control.y);
        combo->SetSize(control.width, control.height);
        combo->SetEnabled(!context.property->IsReadOnly() && context.host != nullptr);

        if (!context.property->IsReadOnly() && context.host)
        {
            combo->SetOnOpenRequested([host = context.host,
                                       property = *context.property,
                                       instance = context.instance,
                                       comboName = combo->GetName(),
                                       enumValues](const UI::Rect& bounds) {
                if (!host)
                {
                    return;
                }

                EditorContextMenuDesc menu;
                menu.id = comboName;
                menu.anchor = Vec2(bounds.x, bounds.Bottom());
                menu.minWidth = std::max(96.0f, bounds.width);
                menu.maxWidth = std::max(menu.minWidth, 320.0f);
                menu.focusOnOpen = true;

                for (size_t index = 0; index < enumValues.size(); ++index)
                {
                    menu.items.push_back(EditorContextMenuItem::Action(
                        property.GetName() + "." + std::to_string(index),
                        enumValues[index],
                        [property, instance, index](EditorUIHost& itemHost) {
                            (void)itemHost;
                            CommitPropertyValue(property,
                                                instance,
                                                static_cast<int32>(index));
                        }));
                }

                host->OpenContextMenu(std::move(menu));
            });
        }

        context.parent->AddChild(combo);
        return true;
    }

    std::string AssetReferenceLabel(const Tools::AssetGUID& guid)
    {
        if (!guid.IsValid())
        {
            return "None";
        }

        const Tools::AssetEntry* entry =
            EditorContext::Get().GetAssetDatabase().GetAsset(guid);
        if (!entry)
        {
            return "Missing: " + guid.ToString().substr(0, 8);
        }

        const std::string& assetName = entry->name.empty() ? entry->path : entry->name;
        return assetName + " (" + AssetTypeLabel(entry->type) + ")";
    }

    std::vector<const Tools::AssetEntry*> SortedAssetEntries()
    {
        std::vector<const Tools::AssetEntry*> entries;
        const auto& databaseAssets = EditorContext::Get().GetAssetDatabase().GetAllAssets();
        entries.reserve(databaseAssets.size());
        for (const auto& [hash, entry] : databaseAssets)
        {
            (void)hash;
            entries.push_back(&entry);
        }

        std::sort(entries.begin(),
                  entries.end(),
                  [](const Tools::AssetEntry* lhs, const Tools::AssetEntry* rhs) {
                      const std::string lhsPath = lhs ? lhs->path : std::string{};
                      const std::string rhsPath = rhs ? rhs->path : std::string{};
                      if (lhsPath != rhsPath)
                      {
                          return lhsPath < rhsPath;
                      }
                      return lhs < rhs;
                  });
        return entries;
    }

    bool DrawAssetRefProperty(EditorPropertyDrawerContext& context)
    {
        if (!PreparePropertyRow(context))
        {
            return false;
        }

        Tools::AssetGUID currentGuid;
        if (!TryGetPropertyValue(*context.property, context.instance, currentGuid))
        {
            return false;
        }

        const UI::UITheme& theme = context.ui->GetTheme();
        AddPropertyNameLabel(context, theme);
        const UI::Rect control = ControlRect(context, theme);

        UI::Button::Ptr button = UI::Button::Create(AssetReferenceLabel(currentGuid));
        button->SetName(MakeWidgetName(context, "Field"));
        button->GetStyle().fontSize = EditorTypography::GetFontSize(
            theme,
            EditorTypographyRole::PropertyValue);
        button->GetStyle().textColor = context.property->IsReadOnly()
                                           ? theme.colors.textMuted
                                           : theme.colors.text;
        button->SetNormalColor(theme.colors.windowBackground);
        button->SetHoverColor(theme.colors.surfaceHover);
        button->SetPressedColor(theme.colors.surfaceActive);
        button->SetDisabledColor(theme.colors.surface.WithAlpha(0.45f));
        button->SetEnabled(!context.property->IsReadOnly() && context.host != nullptr);
        button->SetOverflowMode(UI::TextOverflowMode::EndEllipsis);
        button->SetTooltipText(button->GetText());
        button->SetPosition(control.x, control.y);
        button->SetSize(control.width, control.height);

        if (!context.property->IsReadOnly() && context.host)
        {
            button->SetOnClick([host = context.host,
                                property = *context.property,
                                instance = context.instance,
                                fieldName = button->GetName(),
                                currentGuid](const UI::UIEvent& event) {
                if (!host)
                {
                    return;
                }

                EditorContextMenuDesc menu;
                menu.id = fieldName;
                menu.anchor = event.position;
                menu.minWidth = 220.0f;
                menu.maxWidth = 420.0f;
                menu.focusOnOpen = true;

                menu.items.push_back(EditorContextMenuItem::Action(
                    "asset.clear",
                    "Clear Reference",
                    [property, instance](EditorUIHost& itemHost) {
                        (void)itemHost;
                        CommitPropertyValue(property, instance, Tools::AssetGUID{});
                    },
                    currentGuid.IsValid()));

                const std::vector<const Tools::AssetEntry*> entries = SortedAssetEntries();
                if (!entries.empty())
                {
                    menu.items.push_back(EditorContextMenuItem::Separator("asset.separator"));
                    for (size_t index = 0; index < entries.size(); ++index)
                    {
                        const Tools::AssetEntry* entry = entries[index];
                        if (!entry)
                        {
                            continue;
                        }

                        const Tools::AssetGUID guid = entry->guid;
                        const std::string text =
                            std::string(AssetTypeLabel(entry->type)) + " - " + entry->path;
                        menu.items.push_back(EditorContextMenuItem::Action(
                            "asset.select." + std::to_string(index),
                            text,
                            [property, instance, guid](EditorUIHost& itemHost) {
                                (void)itemHost;
                                CommitPropertyValue(property, instance, guid);
                            }));
                    }
                }
                else
                {
                    menu.items.push_back(EditorContextMenuItem::Separator("asset.separator"));
                    menu.items.push_back(EditorContextMenuItem::Action(
                        "asset.none",
                        "No assets available",
                        {},
                        false));
                }

                host->OpenContextMenu(std::move(menu));
            });
        }

        context.parent->AddChild(button);
        return true;
    }

    bool RegisterDefaultDrawer(EditorPropertyDrawerRegistry& registry,
                               PropertyType type,
                               std::string debugName,
                               EditorPropertyDrawerCallback callback)
    {
        EditorPropertyDrawerDesc desc;
        desc.propertyType = type;
        desc.debugName = std::move(debugName);
        desc.callback = std::move(callback);
        return registry.RegisterDrawer(std::move(desc));
    }
}

bool EditorPropertyDrawerRegistry::RegisterDrawer(EditorPropertyDrawerDesc desc)
{
    if (desc.propertyType == PropertyType::Unknown || !desc.callback)
    {
        return false;
    }

    if (EditorPropertyDrawerDesc* existing = FindDrawer(desc.propertyType))
    {
        *existing = std::move(desc);
        return true;
    }

    m_drawers.push_back(std::move(desc));
    return true;
}

bool EditorPropertyDrawerRegistry::UnregisterDrawer(PropertyType type)
{
    const auto it = std::find_if(m_drawers.begin(),
                                 m_drawers.end(),
                                 [type](const EditorPropertyDrawerDesc& drawer) {
                                     return drawer.propertyType == type;
                                 });
    if (it == m_drawers.end())
    {
        return false;
    }

    m_drawers.erase(it);
    return true;
}

bool EditorPropertyDrawerRegistry::DrawProperty(EditorPropertyDrawerContext& context) const
{
    if (!context.property)
    {
        return false;
    }

    const EditorPropertyDrawerDesc* drawer = FindDrawer(context.property->GetType());
    if (!drawer || !drawer->callback)
    {
        return false;
    }

    return drawer->callback(context);
}

bool EditorPropertyDrawerRegistry::HasDrawer(PropertyType type) const
{
    return FindDrawer(type) != nullptr;
}

const EditorPropertyDrawerDesc* EditorPropertyDrawerRegistry::FindDrawer(PropertyType type) const
{
    const auto it = std::find_if(m_drawers.begin(),
                                 m_drawers.end(),
                                 [type](const EditorPropertyDrawerDesc& drawer) {
                                     return drawer.propertyType == type;
                                 });
    return it == m_drawers.end() ? nullptr : &(*it);
}

EditorPropertyDrawerDesc* EditorPropertyDrawerRegistry::FindDrawer(PropertyType type)
{
    const auto it = std::find_if(m_drawers.begin(),
                                 m_drawers.end(),
                                 [type](const EditorPropertyDrawerDesc& drawer) {
                                     return drawer.propertyType == type;
                                 });
    return it == m_drawers.end() ? nullptr : &(*it);
}

bool RegisterDefaultEditorPropertyDrawers(EditorPropertyDrawerRegistry& registry)
{
    bool ok = true;
    ok &= RegisterDefaultDrawer(registry, PropertyType::Bool, "Default Bool", DrawBoolProperty);
    ok &= RegisterDefaultDrawer(registry,
                                PropertyType::Int32,
                                "Default Int32",
                                DrawInt32Property);
    ok &= RegisterDefaultDrawer(registry,
                                PropertyType::Int64,
                                "Default Int64",
                                DrawInt64Property);
    ok &= RegisterDefaultDrawer(registry, PropertyType::Float, "Default Float", DrawFloatProperty);
    ok &= RegisterDefaultDrawer(registry,
                                PropertyType::Double,
                                "Default Double",
                                DrawDoubleProperty);
    ok &= RegisterDefaultDrawer(registry,
                                PropertyType::String,
                                "Default String",
                                DrawStringProperty);
    ok &= RegisterDefaultDrawer(registry, PropertyType::Vec2, "Default Vec2", DrawVec2Property);
    ok &= RegisterDefaultDrawer(registry, PropertyType::Vec3, "Default Vec3", DrawVec3Property);
    ok &= RegisterDefaultDrawer(registry, PropertyType::Vec4, "Default Vec4", DrawVec4Property);
    ok &= RegisterDefaultDrawer(registry, PropertyType::Quat, "Default Quat", DrawQuatProperty);
    ok &= RegisterDefaultDrawer(registry,
                                PropertyType::Color,
                                "Default Color",
                                DrawColorProperty);
    ok &= RegisterDefaultDrawer(registry, PropertyType::Enum, "Default Enum", DrawEnumProperty);
    ok &= RegisterDefaultDrawer(registry,
                                PropertyType::AssetRef,
                                "Default AssetRef",
                                DrawAssetRefProperty);
    return ok;
}

} // namespace RVX::Editor
