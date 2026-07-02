/**
 * @file Inspector.cpp
 * @brief Inspector panel implementation
 */

#include "Editor/Panels/Inspector.h"
#include "Editor/EditorContext.h"
#include "Core/Log.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"
#include "Scene/SceneEntity.h"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <any>
#include <cstdio>

namespace RVX::Editor
{

InspectorPanel::InspectorPanel()
{
    m_entityNameBuffer.resize(256);
}

bool InspectorPanel::SetAssetReferenceProperty(const Property& prop,
                                               void* instance,
                                               const Tools::AssetGUID& guid)
{
    if (!instance || prop.GetType() != PropertyType::AssetRef || prop.IsReadOnly())
    {
        return false;
    }

    try
    {
        return prop.SetValueAny(instance, guid);
    }
    catch (const std::bad_any_cast&)
    {
        return false;
    }
}

void InspectorPanel::OnGUI()
{
    if (!ImGui::Begin(GetName()))
    {
        ImGui::End();
        return;
    }

    // Lock button
    ImGui::Checkbox("Lock", &m_lockSelection);
    ImGui::SameLine();
    ImGui::Checkbox("Debug", &m_showDebugInfo);
    ImGui::Separator();

    auto& context = EditorContext::Get();

    switch (context.GetSelectionType())
    {
        case SelectionType::Entity:
        {
            SceneEntity* entity = context.GetSelectedEntity();
            if (entity)
            {
                DrawEntityInspector(entity);
            }
            else
            {
                ImGui::TextDisabled("No entity selected");
            }
            break;
        }
        case SelectionType::Asset:
        {
            DrawAssetInspector();
            break;
        }
        default:
        {
            ImGui::TextDisabled("Select an object to inspect");
            break;
        }
    }

    ImGui::End();
}

namespace
{
    int GetFirstLayerIndex(uint32 layerMask)
    {
        for (int layer = 0; layer < 31; ++layer)
        {
            if ((layerMask & (1u << layer)) != 0)
            {
                return layer;
            }
        }
        return 0;
    }
}

void InspectorPanel::DrawEntityHeader(SceneEntity* entity)
{
    if (!entity)
        return;

    // Active checkbox
    auto& context = EditorContext::Get();
    bool active = entity->IsActive();
    if (ImGui::Checkbox("##Active", &active))
    {
        context.SetEntityActiveUndoable(entity, active);
    }

    ImGui::SameLine();

    // Entity name
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100);
    std::snprintf(m_entityNameBuffer.data(),
                  m_entityNameBuffer.size(),
                  "%s",
                  entity->GetName().c_str());
    if (ImGui::InputText("##Name", m_entityNameBuffer.data(), m_entityNameBuffer.size()))
    {
        context.SetEntityNameUndoable(entity, m_entityNameBuffer.data());
    }

    const char* layers[] = {
        "Default", "TransparentFX", "UI", "Ignore Raycast",
        "Layer 4", "Layer 5", "Layer 6", "Layer 7"
    };
    constexpr int layerCount = static_cast<int>(sizeof(layers) / sizeof(layers[0]));
    int layerIndex = GetFirstLayerIndex(entity->GetLayerMask());
    if (layerIndex >= layerCount)
    {
        layerIndex = 0;
    }
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("Layer", &layerIndex, layers, layerCount))
    {
        context.SetEntityLayerMaskUndoable(entity, 1u << static_cast<uint32>(layerIndex));
    }

    if (m_showDebugInfo)
    {
        ImGui::TextDisabled("Handle: %u", entity->GetHandle());
        ImGui::TextDisabled("Class: %s", entity->GetClassName());
        ImGui::TextDisabled("Components: %zu legacy, %zu actor",
                            entity->GetComponentCount(),
                            entity->GetActorComponentCount());
    }
}

void InspectorPanel::DrawEntityInspector(SceneEntity* entity)
{
    DrawEntityHeader(entity);

    ImGui::Separator();

    // Transform component (always present)
    DrawTransformComponent(entity);

    for (const auto& [type, component] : entity->GetComponents())
    {
        (void)type;
        DrawComponentInspector(component.get());
    }

    for (const auto& component : entity->GetActorComponents())
    {
        if (component && !dynamic_cast<Component*>(component.get()))
        {
            bool enabled = component->IsEnabled();
            if (ImGui::Checkbox((std::string("##") + component->GetName()).c_str(), &enabled))
            {
                EditorContext::Get().SetComponentEnabledUndoable(component.get(), enabled);
            }
            ImGui::SameLine();
            ImGui::Text("%s", component->GetClassName());
        }
    }

    ImGui::Separator();

    // Add Component button
    float buttonWidth = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Add Component", ImVec2(buttonWidth, 30)))
    {
        ImGui::OpenPopup("AddComponentPopup");
    }

    DrawAddComponentMenu(entity);
}

void InspectorPanel::DrawTransformComponent(SceneEntity* entity)
{
    if (!entity)
        return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed |
                               ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth;

    ImVec2 contentRegion = ImGui::GetContentRegionAvail();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
    bool open = ImGui::TreeNodeEx("Transform", flags);

    ImGui::SameLine(contentRegion.x - 25);
    if (ImGui::Button("...", ImVec2(25, 20)))
    {
        ImGui::OpenPopup("TransformContext");
    }

    if (ImGui::BeginPopup("TransformContext"))
    {
        if (ImGui::MenuItem("Reset"))
        {
            EditorContext::Get().SetEntityTransformUndoable(
                entity,
                Vec3(0.0f),
                Quat(1.0f, 0.0f, 0.0f, 0.0f),
                Vec3(1.0f),
                "Reset Transform");
        }
        if (ImGui::MenuItem("Copy"))
        {
            // Copy transform
        }
        if (ImGui::MenuItem("Paste"))
        {
            // Paste transform
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();

    if (open)
    {
        // Position
        Vec3 position = entity->GetPosition();
        ImGui::Text("Position");
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat3("##Position", glm::value_ptr(position), 0.1f))
        {
            EditorContext::Get().SetEntityTransformUndoable(
                entity,
                position,
                entity->GetRotation(),
                entity->GetScale(),
                "Move Entity");
        }

        // Rotation
        Vec3 rotation = degrees(QuatToEuler(entity->GetRotation()));
        ImGui::Text("Rotation");
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat3("##Rotation", glm::value_ptr(rotation), 0.5f))
        {
            EditorContext::Get().SetEntityTransformUndoable(
                entity,
                entity->GetPosition(),
                QuatFromEuler(radians(rotation)),
                entity->GetScale(),
                "Rotate Entity");
        }

        // Scale
        Vec3 scale = entity->GetScale();
        ImGui::Text("Scale");
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat3("##Scale", glm::value_ptr(scale), 0.01f))
        {
            EditorContext::Get().SetEntityTransformUndoable(
                entity,
                entity->GetPosition(),
                entity->GetRotation(),
                scale,
                "Scale Entity");
        }

        ImGui::TreePop();
    }
}

void InspectorPanel::DrawComponentInspector(Component* component)
{
    if (!component)
        return;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed |
                               ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth;

    bool removeRequested = false;
    DrawComponentHeader(component, removeRequested);

    if (removeRequested)
    {
        // TODO: Remove component from entity
        return;
    }

    // Get class descriptor for reflection
    auto* descriptor = ReflectionRegistry::Get().GetClass(typeid(*component));
    if (!descriptor)
    {
        ImGui::TextDisabled("No reflection info available");
        return;
    }

    bool open = ImGui::TreeNodeEx(descriptor->GetName().c_str(), flags);

    if (open)
    {
        // Draw all properties
        for (const auto& prop : descriptor->GetProperties())
        {
            if (!prop.IsHidden())
            {
                DrawProperty(prop, component);
            }
        }

        ImGui::TreePop();
    }
}

void InspectorPanel::DrawComponentHeader(Component* component, bool& removeRequested)
{
    if (!component)
        return;

    ImVec2 contentRegion = ImGui::GetContentRegionAvail();

    // Enabled checkbox
    bool enabled = component->IsEnabled();
    if (ImGui::Checkbox("##Enabled", &enabled))
    {
        EditorContext::Get().SetComponentEnabledUndoable(component, enabled);
    }
    ImGui::SameLine();

    // Context menu button
    ImGui::SameLine(contentRegion.x - 25);
    if (ImGui::Button("...", ImVec2(25, 20)))
    {
        ImGui::OpenPopup("ComponentContext");
    }

    if (ImGui::BeginPopup("ComponentContext"))
    {
        if (ImGui::MenuItem("Reset"))
        {
            // Reset component
        }
        if (ImGui::MenuItem("Remove"))
        {
            removeRequested = true;
        }
        if (ImGui::MenuItem("Copy"))
        {
            // Copy component
        }
        if (ImGui::MenuItem("Paste Values"))
        {
            // Paste values
        }
        ImGui::EndPopup();
    }
}

void InspectorPanel::DrawAddComponentMenu(SceneEntity* entity)
{
    (void)entity;

    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        ImGui::Text("Add Component");
        ImGui::Separator();

        // Search box
        static char searchBuffer[128] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##Search", searchBuffer, sizeof(searchBuffer));

        ImGui::Separator();

        // Component categories
        if (ImGui::BeginMenu("Rendering"))
        {
            if (ImGui::MenuItem("Mesh Renderer")) {}
            if (ImGui::MenuItem("Skinned Mesh Renderer")) {}
            if (ImGui::MenuItem("Particle System")) {}
            if (ImGui::MenuItem("Trail Renderer")) {}
            if (ImGui::MenuItem("Line Renderer")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Physics"))
        {
            if (ImGui::MenuItem("Rigidbody")) {}
            if (ImGui::MenuItem("Box Collider")) {}
            if (ImGui::MenuItem("Sphere Collider")) {}
            if (ImGui::MenuItem("Capsule Collider")) {}
            if (ImGui::MenuItem("Mesh Collider")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Audio"))
        {
            if (ImGui::MenuItem("Audio Source")) {}
            if (ImGui::MenuItem("Audio Listener")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Lighting"))
        {
            if (ImGui::MenuItem("Light")) {}
            if (ImGui::MenuItem("Reflection Probe")) {}
            if (ImGui::MenuItem("Light Probe Group")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Animation"))
        {
            if (ImGui::MenuItem("Animator")) {}
            if (ImGui::MenuItem("Animation")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("UI"))
        {
            if (ImGui::MenuItem("Canvas")) {}
            if (ImGui::MenuItem("Button")) {}
            if (ImGui::MenuItem("Text")) {}
            if (ImGui::MenuItem("Image")) {}
            ImGui::EndMenu();
        }

        ImGui::EndPopup();
    }
}

void InspectorPanel::DrawProperty(const Property& prop, void* instance)
{
    if (!BeginPropertyRow(prop))
        return;

    switch (prop.GetType())
    {
        case PropertyType::Bool:
            DrawBoolProperty(prop, instance);
            break;
        case PropertyType::Int32:
        case PropertyType::Int64:
            DrawIntProperty(prop, instance);
            break;
        case PropertyType::Float:
            DrawFloatProperty(prop, instance);
            break;
        case PropertyType::Double:
            DrawDoubleProperty(prop, instance);
            break;
        case PropertyType::Vec2:
            DrawVec2Property(prop, instance);
            break;
        case PropertyType::Vec3:
            DrawVec3Property(prop, instance);
            break;
        case PropertyType::Vec4:
            DrawVec4Property(prop, instance);
            break;
        case PropertyType::Color:
            DrawColorProperty(prop, instance);
            break;
        case PropertyType::String:
            DrawStringProperty(prop, instance);
            break;
        case PropertyType::Enum:
            DrawEnumProperty(prop, instance);
            break;
        case PropertyType::AssetRef:
            DrawAssetRefProperty(prop, instance);
            break;
        default:
            ImGui::TextDisabled("Unsupported type");
            break;
    }

    EndPropertyRow();
}

bool InspectorPanel::BeginPropertyRow(const Property& prop)
{
    DrawPropertyLabel(prop);
    ImGui::SameLine(120);
    ImGui::SetNextItemWidth(-1);
    return true;
}

void InspectorPanel::EndPropertyRow()
{
    // Nothing to do
}

void InspectorPanel::DrawPropertyLabel(const Property& prop)
{
    const auto& meta = prop.GetMeta();
    const char* label = meta.displayName.empty() ? prop.GetName().c_str() : meta.displayName.c_str();

    ImGui::Text("%s", label);

    if (!meta.tooltip.empty() && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", meta.tooltip.c_str());
    }
}

void InspectorPanel::DrawBoolProperty(const Property& prop, void* instance)
{
    bool value = prop.GetValue<bool>(instance);
    if (ImGui::Checkbox(("##" + prop.GetName()).c_str(), &value))
    {
        EditorContext::Get().SetReflectedPropertyUndoable(
            prop, instance, value, "Edit " + prop.GetName());
    }
}

void InspectorPanel::DrawIntProperty(const Property& prop, void* instance)
{
    int value = prop.GetValue<int>(instance);
    const auto& meta = prop.GetMeta();

    if (ImGui::DragInt(("##" + prop.GetName()).c_str(), &value, 1.0f,
                       static_cast<int>(meta.minValue), static_cast<int>(meta.maxValue)))
    {
        EditorContext::Get().SetReflectedPropertyUndoable(
            prop, instance, value, "Edit " + prop.GetName());
    }
}

void InspectorPanel::DrawFloatProperty(const Property& prop, void* instance)
{
    float value = prop.GetValue<float>(instance);
    const auto& meta = prop.GetMeta();

    if (ImGui::DragFloat(("##" + prop.GetName()).c_str(), &value, meta.step,
                         meta.minValue, meta.maxValue))
    {
        EditorContext::Get().SetReflectedPropertyUndoable(
            prop, instance, value, "Edit " + prop.GetName());
    }
}

void InspectorPanel::DrawDoubleProperty(const Property& prop, void* instance)
{
    double value = prop.GetValue<double>(instance);
    float fValue = static_cast<float>(value);
    const auto& meta = prop.GetMeta();

    if (ImGui::DragFloat(("##" + prop.GetName()).c_str(), &fValue, meta.step,
                         meta.minValue, meta.maxValue))
    {
        EditorContext::Get().SetReflectedPropertyUndoable(
            prop, instance, static_cast<double>(fValue), "Edit " + prop.GetName());
    }
}

void InspectorPanel::DrawVec2Property(const Property& prop, void* instance)
{
    Vec2 value = prop.GetValue<Vec2>(instance);

    if (ImGui::DragFloat2(("##" + prop.GetName()).c_str(), glm::value_ptr(value), 0.1f))
    {
        EditorContext::Get().SetReflectedPropertyUndoable(
            prop, instance, value, "Edit " + prop.GetName());
    }
}

void InspectorPanel::DrawVec3Property(const Property& prop, void* instance)
{
    Vec3 value = prop.GetValue<Vec3>(instance);

    if (ImGui::DragFloat3(("##" + prop.GetName()).c_str(), glm::value_ptr(value), 0.1f))
    {
        EditorContext::Get().SetReflectedPropertyUndoable(
            prop, instance, value, "Edit " + prop.GetName());
    }
}

void InspectorPanel::DrawVec4Property(const Property& prop, void* instance)
{
    Vec4 value = prop.GetValue<Vec4>(instance);

    if (ImGui::DragFloat4(("##" + prop.GetName()).c_str(), glm::value_ptr(value), 0.1f))
    {
        EditorContext::Get().SetReflectedPropertyUndoable(
            prop, instance, value, "Edit " + prop.GetName());
    }
}

void InspectorPanel::DrawColorProperty(const Property& prop, void* instance)
{
    Vec4 value = prop.GetValue<Vec4>(instance);

    if (ImGui::ColorEdit4(("##" + prop.GetName()).c_str(), glm::value_ptr(value)))
    {
        EditorContext::Get().SetReflectedPropertyUndoable(
            prop, instance, value, "Edit " + prop.GetName());
    }
}

void InspectorPanel::DrawStringProperty(const Property& prop, void* instance)
{
    std::string value = prop.GetValue<std::string>(instance);

    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());

    if (ImGui::InputText(("##" + prop.GetName()).c_str(), buffer, sizeof(buffer)))
    {
        EditorContext::Get().SetReflectedPropertyUndoable(
            prop, instance, std::string(buffer), "Edit " + prop.GetName());
    }
}

void InspectorPanel::DrawEnumProperty(const Property& prop, void* instance)
{
    int value = prop.GetValue<int>(instance);
    const auto& enumValues = prop.GetMeta().enumValues;

    if (!enumValues.empty())
    {
        std::vector<const char*> items;
        for (const auto& str : enumValues)
        {
            items.push_back(str.c_str());
        }

        if (ImGui::Combo(("##" + prop.GetName()).c_str(), &value, items.data(),
                         static_cast<int>(items.size())))
        {
            EditorContext::Get().SetReflectedPropertyUndoable(
                prop, instance, value, "Edit " + prop.GetName());
        }
    }
}

void InspectorPanel::DrawAssetRefProperty(const Property& prop, void* instance)
{
    Tools::AssetGUID currentGuid;
    bool hasGuidValue = false;
    try
    {
        currentGuid = prop.GetValue<Tools::AssetGUID>(instance);
        hasGuidValue = true;
    }
    catch (const std::bad_any_cast&)
    {
    }

    // Asset reference field with drag-drop support
    std::string label = "None (Asset)";
    if (hasGuidValue && currentGuid.IsValid())
    {
        const auto* entry = EditorContext::Get().GetAssetDatabase().GetAsset(currentGuid);
        label = entry ? entry->name : currentGuid.ToString();
    }
    ImGui::Button(label.c_str(), ImVec2(-1, 0));

    // Accept drag-drop
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RVX_ASSET_GUID"))
        {
            if (payload->DataSize == sizeof(Tools::AssetGUID))
            {
                const auto* guid = static_cast<const Tools::AssetGUID*>(payload->Data);
                if (guid)
                {
                    EditorContext::Get().SetReflectedPropertyUndoable(
                        prop, instance, *guid, "Set Asset Reference");
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void InspectorPanel::DrawAssetInspector()
{
    auto& context = EditorContext::Get();
    auto guid = context.GetSelectedAsset();

    // TODO: Get asset info from database
    // auto* entry = context.GetAssetDatabase().GetAsset(guid);

    ImGui::Text("Asset Inspector");
    ImGui::Separator();

    // Asset preview
    ImGui::Text("Preview:");
    ImVec2 previewSize(200, 200);
    ImGui::InvisibleButton("AssetPreview", previewSize);
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, IM_COL32(40, 40, 40, 255));
    drawList->AddRect(min, max, IM_COL32(60, 60, 60, 255));

    ImGui::Separator();

    // Asset properties
    ImGui::Text("Type: Unknown");
    ImGui::Text("Path: N/A");
    ImGui::Text("Size: N/A");
    ImGui::Text("Last Modified: N/A");
}

} // namespace RVX::Editor
