/**
 * @file SceneHierarchy.cpp
 * @brief Scene hierarchy panel implementation
 */

#include "Editor/Panels/SceneHierarchy.h"
#include "Editor/EditorContext.h"
#include "Core/Log.h"

#include <imgui.h>
#include <algorithm>

namespace RVX::Editor
{

SceneHierarchyPanel::SceneHierarchyPanel()
{
    m_searchFilter.resize(256);
    memset(m_renameBuffer, 0, sizeof(m_renameBuffer));
}

void SceneHierarchyPanel::OnGUI()
{
    if (!ImGui::Begin(GetName()))
    {
        ImGui::End();
        return;
    }

    DrawToolbar();
    ImGui::Separator();
    DrawSceneTree();

    ImGui::End();
}

void SceneHierarchyPanel::DrawToolbar()
{
    // Search box
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60);
    ImGui::InputTextWithHint("##Search", "Search...", m_searchFilter.data(), m_searchFilter.size());

    ImGui::SameLine();

    // Create entity button
    if (ImGui::Button("+", ImVec2(25, 0)))
    {
        ImGui::OpenPopup("CreateEntityPopup");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Create Entity");

    DrawCreateEntityMenu();

    ImGui::SameLine();

    // Options menu
    if (ImGui::Button("...", ImVec2(25, 0)))
    {
        ImGui::OpenPopup("HierarchyOptions");
    }

    if (ImGui::BeginPopup("HierarchyOptions"))
    {
        ImGui::Checkbox("Show Hidden", &m_showHidden);
        ImGui::Separator();
        if (ImGui::MenuItem("Expand All"))
        {
            m_expandAll = true;
        }
        if (ImGui::MenuItem("Collapse All"))
        {
            m_collapseAll = true;
        }
        ImGui::EndPopup();
    }
}

void SceneHierarchyPanel::DrawSceneTree()
{
    auto& context = EditorContext::Get();
    auto scene = context.GetActiveScene();

    if (!scene)
    {
        ImGui::TextDisabled("No scene loaded");
        
        if (ImGui::Button("New Scene", ImVec2(-1, 30)))
        {
            context.NewScene();
        }
        return;
    }

    // Scene root node
    ImGuiTreeNodeFlags sceneFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow |
                                     ImGuiTreeNodeFlags_SpanAvailWidth;

    bool sceneOpen = ImGui::TreeNodeEx("Scene", sceneFlags);

    // Scene context menu
    if (ImGui::BeginPopupContextItem())
    {
        DrawCreateEntityMenu();
        ImGui::Separator();
        if (ImGui::MenuItem("Save Scene"))
        {
            // Save scene
        }
        if (ImGui::MenuItem("Reload Scene"))
        {
            // Reload scene
        }
        ImGui::EndPopup();
    }

    if (sceneOpen)
    {
        // TODO: Iterate through scene root entities
        // for (auto* entity : scene->GetRootEntities())
        // {
        //     if (PassesFilter(entity))
        //     {
        //         DrawEntityNode(entity);
        //     }
        // }

        // Placeholder entities for demonstration
        DrawEntityNode(nullptr, 0);

        ImGui::TreePop();
    }

    // Handle expand/collapse all
    if (m_expandAll)
    {
        // TODO: Expand all tree nodes
        m_expandAll = false;
    }
    if (m_collapseAll)
    {
        // TODO: Collapse all tree nodes
        m_collapseAll = false;
    }

    // Right-click on empty space
    if (ImGui::BeginPopupContextWindow("HierarchyEmptyContext", ImGuiPopupFlags_NoOpenOverItems))
    {
        DrawCreateEntityMenu();
        ImGui::EndPopup();
    }
}

void SceneHierarchyPanel::DrawEntityNode(Entity* entity, int depth)
{
    (void)entity;
    (void)depth;

    auto& context = EditorContext::Get();

    // Demo entity for visualization
    const char* entityName = "Demo Entity";
    bool hasChildren = false;
    bool isSelected = false;  // TODO: context.IsSelected(entity);
    bool isActive = true;

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (isSelected)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (!hasChildren)
    {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    // Dim inactive entities
    if (!isActive)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    }

    // Draw node
    bool nodeOpen = false;
    if (m_renamingEntity == entity)
    {
        // Rename mode
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##Rename", m_renameBuffer, sizeof(m_renameBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            // TODO: entity->SetName(m_renameBuffer);
            m_renamingEntity = nullptr;
            context.MarkSceneDirty();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || 
            (!ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
        {
            m_renamingEntity = nullptr;
        }
    }
    else
    {
        nodeOpen = ImGui::TreeNodeEx(entityName, flags);

        // Handle selection
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            if (ImGui::GetIO().KeyCtrl)
            {
                // TODO: Toggle selection
            }
            else
            {
                context.SelectEntity(entity);
            }
        }

        // Double click to rename
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            m_renamingEntity = entity;
            strncpy(m_renameBuffer, entityName, sizeof(m_renameBuffer) - 1);
        }

        // Drag source
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("ENTITY_PTR", &entity, sizeof(Entity*));
            ImGui::Text("%s", entityName);
            ImGui::EndDragDropSource();
        }

        // Drop target
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_PTR"))
            {
                Entity* droppedEntity = *static_cast<Entity**>(payload->Data);
                (void)droppedEntity;
                // TODO: Set parent
                context.MarkSceneDirty();
            }
            ImGui::EndDragDropTarget();
        }
    }

    if (!isActive)
    {
        ImGui::PopStyleColor();
    }

    // Context menu
    DrawContextMenu(entity);

    // Draw children
    if (nodeOpen)
    {
        // TODO: for (auto* child : entity->GetChildren())
        // {
        //     if (PassesFilter(child))
        //     {
        //         DrawEntityNode(child, depth + 1);
        //     }
        // }
        ImGui::TreePop();
    }
}

void SceneHierarchyPanel::DrawContextMenu(Entity* entity)
{
    (void)entity;

    if (ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Rename", "F2"))
        {
            m_renamingEntity = entity;
            strncpy(m_renameBuffer, "Entity", sizeof(m_renameBuffer) - 1);
        }

        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
        {
            // TODO: Duplicate entity
            EditorContext::Get().MarkSceneDirty();
        }

        if (ImGui::MenuItem("Delete", "Delete"))
        {
            // TODO: Delete entity
            EditorContext::Get().MarkSceneDirty();
        }

        ImGui::Separator();

        if (ImGui::BeginMenu("Create Child"))
        {
            if (ImGui::MenuItem("Empty")) {}
            if (ImGui::MenuItem("Cube")) {}
            if (ImGui::MenuItem("Sphere")) {}
            if (ImGui::MenuItem("Light")) {}
            if (ImGui::MenuItem("Camera")) {}
            ImGui::EndMenu();
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Copy", "Ctrl+C"))
        {
            // TODO: Copy entity
        }

        if (ImGui::MenuItem("Paste", "Ctrl+V"))
        {
            // TODO: Paste entity
        }

        if (ImGui::MenuItem("Paste As Child"))
        {
            // TODO: Paste as child
        }

        ImGui::Separator();

        bool isActive = true;
        if (ImGui::MenuItem(isActive ? "Deactivate" : "Activate"))
        {
            // TODO: Toggle active state
            EditorContext::Get().MarkSceneDirty();
        }

        if (ImGui::MenuItem("Focus", "F"))
        {
            // TODO: Focus camera on entity
        }

        ImGui::EndPopup();
    }
}

void SceneHierarchyPanel::DrawCreateEntityMenu()
{
    if (ImGui::BeginPopup("CreateEntityPopup") || ImGui::BeginMenu("Create Entity"))
    {
        if (ImGui::MenuItem("Empty Entity"))
        {
            // TODO: Create empty entity
            EditorContext::Get().MarkSceneDirty();
        }

        ImGui::Separator();

        if (ImGui::BeginMenu("3D Objects"))
        {
            if (ImGui::MenuItem("Cube")) {}
            if (ImGui::MenuItem("Sphere")) {}
            if (ImGui::MenuItem("Plane")) {}
            if (ImGui::MenuItem("Cylinder")) {}
            if (ImGui::MenuItem("Capsule")) {}
            if (ImGui::MenuItem("Quad")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Lights"))
        {
            if (ImGui::MenuItem("Directional Light")) {}
            if (ImGui::MenuItem("Point Light")) {}
            if (ImGui::MenuItem("Spot Light")) {}
            if (ImGui::MenuItem("Area Light")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Audio"))
        {
            if (ImGui::MenuItem("Audio Source")) {}
            if (ImGui::MenuItem("Audio Listener")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Effects"))
        {
            if (ImGui::MenuItem("Particle System")) {}
            if (ImGui::MenuItem("Decal")) {}
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Camera")) {}

        if (ImGui::BeginMenu("UI"))
        {
            if (ImGui::MenuItem("Canvas")) {}
            if (ImGui::MenuItem("Text")) {}
            if (ImGui::MenuItem("Image")) {}
            if (ImGui::MenuItem("Button")) {}
            ImGui::EndMenu();
        }

        if (ImGui::IsPopupOpen("CreateEntityPopup"))
            ImGui::EndPopup();
        else
            ImGui::EndMenu();
    }
}

void SceneHierarchyPanel::HandleDragDrop(Entity* entity)
{
    (void)entity;
    // Already handled in DrawEntityNode
}

bool SceneHierarchyPanel::PassesFilter(Entity* entity) const
{
    (void)entity;

    if (m_searchFilter.empty() || m_searchFilter[0] == '\0')
        return true;

    // TODO: Check entity name against filter
    // std::string name = entity->GetName();
    // return name.find(m_searchFilter) != std::string::npos;

    return true;
}

} // namespace RVX::Editor
