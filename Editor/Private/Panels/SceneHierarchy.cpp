/**
 * @file SceneHierarchy.cpp
 * @brief Scene hierarchy panel implementation
 */

#include "Editor/Panels/SceneHierarchy.h"
#include "Editor/EditorContext.h"
#include "Core/Log.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace RVX::Editor
{

SceneHierarchyPanel::SceneHierarchyPanel()
{
    m_searchFilter.resize(256);
    memset(m_renameBuffer, 0, sizeof(m_renameBuffer));
}

void SceneHierarchyPanel::OnGUI()
{
    m_entityRows.clear();
    m_renameEditBounds = {};

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

void SceneHierarchyPanel::OnNativeInput(const UI::UIInputState& input)
{
    if (m_renamingEntity && input.WasKeyPressed(UI::RVX_UI_KEY_ESCAPE))
    {
        m_renamingEntity = nullptr;
        return;
    }

    const Vec2 mousePosition = input.current.mousePosition;
    const bool leftPressed = input.WasMouseButtonPressed(UI::UIMouseButton::Left);
    if (m_renamingEntity)
    {
        if (leftPressed && !m_renameEditBounds.Contains(mousePosition))
        {
            m_renamingEntity = nullptr;
        }
        return;
    }

    if (!leftPressed)
    {
        return;
    }

    SceneEntity* hitEntity = nullptr;
    for (auto it = m_entityRows.rbegin(); it != m_entityRows.rend(); ++it)
    {
        if (it->entity && it->bounds.Contains(mousePosition))
        {
            hitEntity = it->entity;
            break;
        }
    }
    if (!hitEntity)
    {
        return;
    }

    auto& context = EditorContext::Get();
    if (!(input.current.modifiers & UI::ToMask(UI::UIInputModifier::Ctrl)))
    {
        context.SelectEntity(hitEntity);
    }

    if (input.WasMouseButtonDoubleClicked(UI::UIMouseButton::Left))
    {
        m_renamingEntity = hitEntity;
        const std::string& entityName = hitEntity->GetName();
        std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", entityName.c_str());
    }
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
    auto* sceneManager = context.GetActiveSceneManager();

    if (!sceneManager)
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
    if (ImGui::BeginPopupContextItem("SceneRootContext"))
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
        sceneManager->ForEachEntity([this](SceneEntity* entity) {
            if (entity && entity->IsRoot() && PassesFilter(entity))
            {
                DrawEntityNode(entity);
            }
        });

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

void SceneHierarchyPanel::DrawEntityNode(SceneEntity* entity, int depth)
{
    (void)depth;
    if (!entity)
        return;

    auto& context = EditorContext::Get();

    const std::string& entityName = entity->GetName();
    bool hasChildren = entity->GetChildCount() > 0;
    bool isSelected = context.IsSelected(entity);
    bool isActive = entity->IsActive();

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
    ImGui::PushID(static_cast<int>(entity->GetHandle()));
    if (m_renamingEntity == entity)
    {
        // Rename mode
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##Rename", m_renameBuffer, sizeof(m_renameBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            context.SetEntityNameUndoable(entity, m_renameBuffer);
            m_renamingEntity = nullptr;
        }
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        m_renameEditBounds = UI::Rect(itemMin.x,
                                      itemMin.y,
                                      itemMax.x - itemMin.x,
                                      itemMax.y - itemMin.y);
    }
    else
    {
        nodeOpen = ImGui::TreeNodeEx(entityName.empty() ? "Entity" : entityName.c_str(), flags);
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        m_entityRows.push_back(EntityRowHit{
            entity,
            UI::Rect(itemMin.x, itemMin.y, itemMax.x - itemMin.x, itemMax.y - itemMin.y)});

        // Drag source
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("ENTITY_PTR", &entity, sizeof(SceneEntity*));
            ImGui::Text("%s", entityName.empty() ? "Entity" : entityName.c_str());
            ImGui::EndDragDropSource();
        }

        // Drop target
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_PTR"))
            {
                SceneEntity* droppedEntity = *static_cast<SceneEntity**>(payload->Data);
                if (droppedEntity && droppedEntity != entity && !entity->IsDescendantOf(droppedEntity))
                {
                    context.SetEntityParentUndoable(droppedEntity, entity);
                }
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
        for (auto* child : entity->GetChildren())
        {
            if (PassesFilter(child))
            {
                DrawEntityNode(child, depth + 1);
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void SceneHierarchyPanel::DrawContextMenu(SceneEntity* entity)
{
    if (!entity)
        return;

    if (ImGui::BeginPopupContextItem("EntityContext"))
    {
        if (ImGui::MenuItem("Rename", "F2"))
        {
            m_renamingEntity = entity;
            std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", entity->GetName().c_str());
        }

        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
        {
            // TODO: Duplicate entity
            EditorContext::Get().MarkSceneDirty();
        }

        if (ImGui::MenuItem("Delete", "Delete"))
        {
            EditorContext::Get().DestroyEntityUndoable(entity);
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

        bool isActive = entity->IsActive();
        if (ImGui::MenuItem(isActive ? "Deactivate" : "Activate"))
        {
            EditorContext::Get().SetEntityActiveUndoable(entity, !isActive);
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
            auto& context = EditorContext::Get();
            if (context.GetActiveSceneManager())
            {
                if (SceneEntity* entity = context.CreateEntityUndoable("Entity"))
                {
                    context.SelectEntity(entity);
                }
            }
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

void SceneHierarchyPanel::HandleDragDrop(SceneEntity* entity)
{
    (void)entity;
    // Already handled in DrawEntityNode
}

bool SceneHierarchyPanel::PassesFilter(SceneEntity* entity) const
{
    if (!entity)
        return false;

    if (m_searchFilter.empty() || m_searchFilter[0] == '\0')
        return true;

    return entity->GetName().find(m_searchFilter.c_str()) != std::string::npos;
}

} // namespace RVX::Editor
