/**
 * @file Inspector.cpp
 * @brief Inspector panel implementation
 */

#include "Editor/Panels/Inspector.h"
#include "Editor/EditorContext.h"
#include "Core/Log.h"
#include "Scene/Component.h"
#include "Scene/ComponentFactory.h"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace RVX::Editor
{

InspectorPanel::InspectorPanel()
{
    m_entityNameBuffer.resize(256);
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
            Entity* entity = context.GetSelectedEntity();
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

void InspectorPanel::DrawEntityHeader(Entity* entity)
{
    (void)entity;
    
    // Active checkbox
    bool active = true;  // TODO: entity->IsActive();
    if (ImGui::Checkbox("##Active", &active))
    {
        // TODO: entity->SetActive(active);
        EditorContext::Get().MarkSceneDirty();
    }

    ImGui::SameLine();

    // Entity name
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100);
    strcpy(m_entityNameBuffer.data(), "Entity");  // TODO: entity->GetName().c_str()
    if (ImGui::InputText("##Name", m_entityNameBuffer.data(), m_entityNameBuffer.size()))
    {
        // TODO: entity->SetName(m_entityNameBuffer.data());
        EditorContext::Get().MarkSceneDirty();
    }

    ImGui::SameLine();

    // Static checkbox
    bool isStatic = false;  // TODO: entity->IsStatic();
    if (ImGui::Checkbox("Static", &isStatic))
    {
        // TODO: entity->SetStatic(isStatic);
        EditorContext::Get().MarkSceneDirty();
    }

    // Tag and Layer
    ImGui::SetNextItemWidth(100);
    const char* tags[] = { "Untagged", "Player", "Enemy", "Pickup", "Environment" };
    int tagIndex = 0;
    ImGui::Combo("Tag", &tagIndex, tags, 5);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* layers[] = { "Default", "TransparentFX", "UI", "Ignore Raycast" };
    int layerIndex = 0;
    ImGui::Combo("Layer", &layerIndex, layers, 4);
}

void InspectorPanel::DrawEntityInspector(Entity* entity)
{
    DrawEntityHeader(entity);

    ImGui::Separator();

    // Transform component (always present)
    DrawTransformComponent(entity);

    // Other components
    // TODO: Iterate through entity components
    // for (auto& component : entity->GetComponents())
    // {
    //     DrawComponentInspector(component.get());
    // }

    ImGui::Separator();

    // Add Component button
    float buttonWidth = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Add Component", ImVec2(buttonWidth, 30)))
    {
        ImGui::OpenPopup("AddComponentPopup");
    }

    DrawAddComponentMenu(entity);
}

void InspectorPanel::DrawTransformComponent(Entity* entity)
{
    (void)entity;
    
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
            // Reset transform
            EditorContext::Get().MarkSceneDirty();
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
        Vec3 position(0.0f);  // TODO: entity->GetPosition();
        ImGui::Text("Position");
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat3("##Position", glm::value_ptr(position), 0.1f))
        {
            // TODO: entity->SetPosition(position);
            EditorContext::Get().MarkSceneDirty();
        }

        // Rotation
        Vec3 rotation(0.0f);  // TODO: entity->GetEulerAngles();
        ImGui::Text("Rotation");
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat3("##Rotation", glm::value_ptr(rotation), 0.5f))
        {
            // TODO: entity->SetEulerAngles(rotation);
            EditorContext::Get().MarkSceneDirty();
        }

        // Scale
        Vec3 scale(1.0f);  // TODO: entity->GetScale();
        ImGui::Text("Scale");
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat3("##Scale", glm::value_ptr(scale), 0.01f))
        {
            // TODO: entity->SetScale(scale);
            EditorContext::Get().MarkSceneDirty();
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
    (void)component;
    
    ImVec2 contentRegion = ImGui::GetContentRegionAvail();

    // Enabled checkbox
    bool enabled = true;  // TODO: component->IsEnabled();
    ImGui::Checkbox("##Enabled", &enabled);
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

void InspectorPanel::DrawAddComponentMenu(Entity* entity)
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
        prop.SetValue(instance, value);
        EditorContext::Get().MarkSceneDirty();
    }
}

void InspectorPanel::DrawIntProperty(const Property& prop, void* instance)
{
    int value = prop.GetValue<int>(instance);
    const auto& meta = prop.GetMeta();
    
    if (ImGui::DragInt(("##" + prop.GetName()).c_str(), &value, 1.0f,
                       static_cast<int>(meta.minValue), static_cast<int>(meta.maxValue)))
    {
        prop.SetValue(instance, value);
        EditorContext::Get().MarkSceneDirty();
    }
}

void InspectorPanel::DrawFloatProperty(const Property& prop, void* instance)
{
    float value = prop.GetValue<float>(instance);
    const auto& meta = prop.GetMeta();
    
    if (ImGui::DragFloat(("##" + prop.GetName()).c_str(), &value, meta.step,
                         meta.minValue, meta.maxValue))
    {
        prop.SetValue(instance, value);
        EditorContext::Get().MarkSceneDirty();
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
        prop.SetValue(instance, static_cast<double>(fValue));
        EditorContext::Get().MarkSceneDirty();
    }
}

void InspectorPanel::DrawVec2Property(const Property& prop, void* instance)
{
    Vec2 value = prop.GetValue<Vec2>(instance);
    
    if (ImGui::DragFloat2(("##" + prop.GetName()).c_str(), glm::value_ptr(value), 0.1f))
    {
        prop.SetValue(instance, value);
        EditorContext::Get().MarkSceneDirty();
    }
}

void InspectorPanel::DrawVec3Property(const Property& prop, void* instance)
{
    Vec3 value = prop.GetValue<Vec3>(instance);
    
    if (ImGui::DragFloat3(("##" + prop.GetName()).c_str(), glm::value_ptr(value), 0.1f))
    {
        prop.SetValue(instance, value);
        EditorContext::Get().MarkSceneDirty();
    }
}

void InspectorPanel::DrawVec4Property(const Property& prop, void* instance)
{
    Vec4 value = prop.GetValue<Vec4>(instance);
    
    if (ImGui::DragFloat4(("##" + prop.GetName()).c_str(), glm::value_ptr(value), 0.1f))
    {
        prop.SetValue(instance, value);
        EditorContext::Get().MarkSceneDirty();
    }
}

void InspectorPanel::DrawColorProperty(const Property& prop, void* instance)
{
    Vec4 value = prop.GetValue<Vec4>(instance);
    
    if (ImGui::ColorEdit4(("##" + prop.GetName()).c_str(), glm::value_ptr(value)))
    {
        prop.SetValue(instance, value);
        EditorContext::Get().MarkSceneDirty();
    }
}

void InspectorPanel::DrawStringProperty(const Property& prop, void* instance)
{
    std::string value = prop.GetValue<std::string>(instance);
    
    char buffer[256];
    strncpy(buffer, value.c_str(), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    if (ImGui::InputText(("##" + prop.GetName()).c_str(), buffer, sizeof(buffer)))
    {
        prop.SetValue(instance, std::string(buffer));
        EditorContext::Get().MarkSceneDirty();
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
            prop.SetValue(instance, value);
            EditorContext::Get().MarkSceneDirty();
        }
    }
}

void InspectorPanel::DrawAssetRefProperty(const Property& prop, void* instance)
{
    (void)prop;
    (void)instance;
    
    // Asset reference field with drag-drop support
    ImGui::Button("None (Asset)", ImVec2(-1, 0));

    // Accept drag-drop
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_GUID"))
        {
            // TODO: Set asset reference
            EditorContext::Get().MarkSceneDirty();
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
