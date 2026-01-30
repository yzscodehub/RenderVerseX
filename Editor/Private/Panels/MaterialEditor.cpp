/**
 * @file MaterialEditor.cpp
 * @brief Material editor panel implementation
 */

#include "Editor/Panels/MaterialEditor.h"
#include "Editor/EditorContext.h"
#include "Core/Log.h"

#include <imgui.h>
#include <cmath>
#include <algorithm>

namespace RVX::Editor
{

MaterialEditorPanel::MaterialEditorPanel()
{
    // Create default output node
    NewMaterial();
}

void MaterialEditorPanel::OnInit()
{
}

void MaterialEditorPanel::OnGUI()
{
    if (!ImGui::Begin(GetName()))
    {
        ImGui::End();
        return;
    }

    DrawToolbar();
    ImGui::Separator();

    // Layout: Node graph on left, properties/preview on right
    float panelWidth = ImGui::GetContentRegionAvail().x;
    float propertiesWidth = 250.0f;

    if (m_showProperties || m_showPreview)
    {
        ImGui::Columns(2, "MaterialEditorColumns", true);
        ImGui::SetColumnWidth(0, panelWidth - propertiesWidth);
    }

    // Node graph
    ImGui::BeginChild("NodeGraph", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
    DrawNodeGraph();
    ImGui::EndChild();

    if (m_showProperties || m_showPreview)
    {
        ImGui::NextColumn();

        // Properties panel
        if (m_showProperties)
        {
            ImGui::BeginChild("Properties", ImVec2(0, m_showPreview ? 300 : 0), true);
            DrawPropertiesPanel();
            ImGui::EndChild();
        }

        // Preview panel
        if (m_showPreview)
        {
            ImGui::BeginChild("Preview", ImVec2(0, 0), true);
            DrawPreviewPanel();
            ImGui::EndChild();
        }

        ImGui::Columns(1);
    }

    ImGui::End();
}

void MaterialEditorPanel::DrawToolbar()
{
    if (ImGui::Button("New"))
    {
        NewMaterial();
    }

    ImGui::SameLine();
    if (ImGui::Button("Load"))
    {
        // Load material
    }

    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        SaveMaterial();
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // View options
    ImGui::Checkbox("Grid", &m_showGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Minimap", &m_showMinimap);
    ImGui::SameLine();
    ImGui::Checkbox("Properties", &m_showProperties);
    ImGui::SameLine();
    ImGui::Checkbox("Preview", &m_showPreview);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Zoom
    ImGui::SetNextItemWidth(80);
    ImGui::SliderFloat("Zoom", &m_canvasZoom, 0.25f, 2.0f, "%.2f");

    ImGui::SameLine();
    if (ImGui::Button("Reset View"))
    {
        m_canvasOffset = Vec2(0.0f, 0.0f);
        m_canvasZoom = 1.0f;
    }
}

void MaterialEditorPanel::DrawNodeGraph()
{
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background
    drawList->AddRectFilled(canvasPos,
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(30, 30, 35, 255));

    // Grid
    if (m_showGrid)
    {
        float gridSize = 32.0f * m_canvasZoom;
        ImU32 gridColor = IM_COL32(50, 50, 55, 255);
        ImU32 gridColorMajor = IM_COL32(60, 60, 65, 255);

        float offsetX = std::fmod(m_canvasOffset.x * m_canvasZoom, gridSize);
        float offsetY = std::fmod(m_canvasOffset.y * m_canvasZoom, gridSize);

        int lineIndex = 0;
        for (float x = offsetX; x < canvasSize.x; x += gridSize)
        {
            ImU32 color = (lineIndex++ % 5 == 0) ? gridColorMajor : gridColor;
            drawList->AddLine(ImVec2(canvasPos.x + x, canvasPos.y),
                             ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y),
                             color);
        }

        lineIndex = 0;
        for (float y = offsetY; y < canvasSize.y; y += gridSize)
        {
            ImU32 color = (lineIndex++ % 5 == 0) ? gridColorMajor : gridColor;
            drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + y),
                             ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y),
                             color);
        }
    }

    // Draw links first (behind nodes)
    DrawLinks();

    // Draw nodes
    for (auto& node : m_nodes)
    {
        DrawNode(node);
    }

    // Draw link being created
    if (m_isCreatingLink)
    {
        Vec2 startPos = GetPinPosition(m_linkSourceNode, m_linkSourcePin, m_linkSourceIsOutput);
        ImVec2 start(canvasPos.x + (startPos.x + m_canvasOffset.x) * m_canvasZoom,
                     canvasPos.y + (startPos.y + m_canvasOffset.y) * m_canvasZoom);
        ImVec2 end = ImGui::GetMousePos();

        ImU32 linkColor = IM_COL32(200, 200, 200, 200);
        drawList->AddBezierCubic(start, 
                                  ImVec2(start.x + 50, start.y),
                                  ImVec2(end.x - 50, end.y),
                                  end, linkColor, 2.0f);
    }

    // Handle input
    HandleNodeInteraction();

    // Context menu
    DrawContextMenu();

    // Minimap
    if (m_showMinimap)
    {
        float minimapSize = 150.0f;
        ImVec2 minimapPos(canvasPos.x + canvasSize.x - minimapSize - 10,
                          canvasPos.y + canvasSize.y - minimapSize - 10);

        drawList->AddRectFilled(minimapPos,
                                ImVec2(minimapPos.x + minimapSize, minimapPos.y + minimapSize),
                                IM_COL32(20, 20, 25, 200), 4.0f);
        drawList->AddRect(minimapPos,
                          ImVec2(minimapPos.x + minimapSize, minimapPos.y + minimapSize),
                          IM_COL32(60, 60, 65, 255), 4.0f);

        // Draw node positions in minimap
        for (const auto& node : m_nodes)
        {
            float mx = minimapPos.x + (node.position.x + m_canvasOffset.x) * 0.05f + minimapSize * 0.5f;
            float my = minimapPos.y + (node.position.y + m_canvasOffset.y) * 0.05f + minimapSize * 0.5f;
            mx = std::clamp(mx, minimapPos.x + 5, minimapPos.x + minimapSize - 5);
            my = std::clamp(my, minimapPos.y + 5, minimapPos.y + minimapSize - 5);
            drawList->AddCircleFilled(ImVec2(mx, my), 3.0f, IM_COL32(100, 150, 200, 255));
        }
    }
}

void MaterialEditorPanel::DrawNode(MaterialNode& node)
{
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Calculate node screen position
    float nodeX = canvasPos.x + (node.position.x + m_canvasOffset.x) * m_canvasZoom;
    float nodeY = canvasPos.y + (node.position.y + m_canvasOffset.y) * m_canvasZoom;
    float nodeWidth = m_nodeWidth * m_canvasZoom;
    
    // Calculate node height based on pins
    int pinCount = std::max(static_cast<int>(node.inputs.size()), 
                           static_cast<int>(node.outputs.size()));
    float nodeHeight = (30.0f + pinCount * 22.0f) * m_canvasZoom;

    ImVec2 nodePos(nodeX, nodeY);
    ImVec2 nodeSize(nodeWidth, nodeHeight);

    // Node colors
    bool isSelected = (m_selectedNode == node.id);
    bool isHovered = (m_hoveredNode == node.id);

    ImU32 headerColor = isSelected ? IM_COL32(80, 120, 180, 255) :
                        isHovered ? IM_COL32(70, 100, 150, 255) :
                                    IM_COL32(60, 80, 120, 255);
    ImU32 bodyColor = IM_COL32(40, 40, 45, 230);
    ImU32 borderColor = isSelected ? IM_COL32(100, 150, 200, 255) :
                                      IM_COL32(60, 60, 65, 255);

    // Special color for output node
    if (node.type == MaterialNodeType::Output)
    {
        headerColor = IM_COL32(150, 80, 80, 255);
    }

    // Draw node background
    float headerHeight = 24.0f * m_canvasZoom;
    float cornerRadius = 4.0f * m_canvasZoom;

    // Header
    drawList->AddRectFilled(nodePos, 
                            ImVec2(nodePos.x + nodeSize.x, nodePos.y + headerHeight),
                            headerColor, cornerRadius, ImDrawFlags_RoundCornersTop);

    // Body
    drawList->AddRectFilled(ImVec2(nodePos.x, nodePos.y + headerHeight),
                            ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
                            bodyColor, cornerRadius, ImDrawFlags_RoundCornersBottom);

    // Border
    drawList->AddRect(nodePos, 
                      ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
                      borderColor, cornerRadius, 0, isSelected ? 2.0f : 1.0f);

    // Node title
    ImVec2 textPos(nodePos.x + 8 * m_canvasZoom, nodePos.y + 4 * m_canvasZoom);
    drawList->AddText(textPos, IM_COL32(220, 220, 220, 255), node.name.c_str());

    // Draw pins
    float pinY = nodePos.y + headerHeight + 12 * m_canvasZoom;
    float pinSpacing = 22.0f * m_canvasZoom;

    // Input pins (left side)
    for (size_t i = 0; i < node.inputs.size(); ++i)
    {
        const auto& pin = node.inputs[i];
        ImVec2 pinPos(nodePos.x, pinY);

        // Pin circle
        ImU32 pinColor = GetPinTypeColor(pin.type);
        drawList->AddCircleFilled(pinPos, m_pinRadius * m_canvasZoom, pinColor);
        drawList->AddCircle(pinPos, m_pinRadius * m_canvasZoom, IM_COL32(180, 180, 180, 255));

        // Pin label
        ImVec2 labelPos(pinPos.x + 12 * m_canvasZoom, pinPos.y - 6 * m_canvasZoom);
        drawList->AddText(labelPos, IM_COL32(180, 180, 180, 255), pin.name.c_str());

        pinY += pinSpacing;
    }

    // Output pins (right side)
    pinY = nodePos.y + headerHeight + 12 * m_canvasZoom;
    for (size_t i = 0; i < node.outputs.size(); ++i)
    {
        const auto& pin = node.outputs[i];
        ImVec2 pinPos(nodePos.x + nodeSize.x, pinY);

        // Pin circle
        ImU32 pinColor = GetPinTypeColor(pin.type);
        drawList->AddCircleFilled(pinPos, m_pinRadius * m_canvasZoom, pinColor);
        drawList->AddCircle(pinPos, m_pinRadius * m_canvasZoom, IM_COL32(180, 180, 180, 255));

        // Pin label
        ImVec2 textSize = ImGui::CalcTextSize(pin.name.c_str());
        ImVec2 labelPos(pinPos.x - textSize.x - 12 * m_canvasZoom, pinPos.y - 6 * m_canvasZoom);
        drawList->AddText(labelPos, IM_COL32(180, 180, 180, 255), pin.name.c_str());

        pinY += pinSpacing;
    }

    // Handle node interaction
    ImVec2 mousePos = ImGui::GetMousePos();
    bool mouseInNode = mousePos.x >= nodePos.x && mousePos.x <= nodePos.x + nodeSize.x &&
                       mousePos.y >= nodePos.y && mousePos.y <= nodePos.y + nodeSize.y;

    if (mouseInNode)
    {
        m_hoveredNode = node.id;
    }
}

void MaterialEditorPanel::DrawLinks()
{
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    for (const auto& link : m_links)
    {
        Vec2 startPos = GetPinPosition(link.sourceNodeId, link.sourcePinIndex, true);
        Vec2 endPos = GetPinPosition(link.destNodeId, link.destPinIndex, false);

        ImVec2 start(canvasPos.x + (startPos.x + m_canvasOffset.x) * m_canvasZoom,
                     canvasPos.y + (startPos.y + m_canvasOffset.y) * m_canvasZoom);
        ImVec2 end(canvasPos.x + (endPos.x + m_canvasOffset.x) * m_canvasZoom,
                   canvasPos.y + (endPos.y + m_canvasOffset.y) * m_canvasZoom);

        // Get color based on pin type
        MaterialPinType type = GetPinType(link.sourceNodeId, link.sourcePinIndex, true);
        ImU32 linkColor = GetPinTypeColor(type);

        // Draw bezier curve
        float tangentLength = std::abs(end.x - start.x) * 0.5f;
        tangentLength = std::max(tangentLength, 50.0f);

        drawList->AddBezierCubic(start,
                                  ImVec2(start.x + tangentLength, start.y),
                                  ImVec2(end.x - tangentLength, end.y),
                                  end, linkColor, 3.0f);
    }
}

void MaterialEditorPanel::DrawContextMenu()
{
    if (ImGui::BeginPopupContextWindow("NodeContextMenu", ImGuiPopupFlags_NoOpenOverItems))
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        Vec2 nodePos((mousePos.x - canvasPos.x) / m_canvasZoom - m_canvasOffset.x,
                     (mousePos.y - canvasPos.y) / m_canvasZoom - m_canvasOffset.y);

        ImGui::Text("Add Node");
        ImGui::Separator();

        if (ImGui::BeginMenu("Constants"))
        {
            if (ImGui::MenuItem("Float")) CreateNode(MaterialNodeType::Constant, nodePos);
            if (ImGui::MenuItem("Vector2")) CreateNode(MaterialNodeType::Constant, nodePos);
            if (ImGui::MenuItem("Vector3")) CreateNode(MaterialNodeType::Constant, nodePos);
            if (ImGui::MenuItem("Vector4")) CreateNode(MaterialNodeType::Constant, nodePos);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Parameters"))
        {
            if (ImGui::MenuItem("Float Parameter")) CreateNode(MaterialNodeType::Parameter, nodePos);
            if (ImGui::MenuItem("Vector Parameter")) CreateNode(MaterialNodeType::Parameter, nodePos);
            if (ImGui::MenuItem("Color Parameter")) CreateNode(MaterialNodeType::Parameter, nodePos);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Textures"))
        {
            if (ImGui::MenuItem("Texture 2D")) CreateNode(MaterialNodeType::Texture, nodePos);
            if (ImGui::MenuItem("Texture Cube")) CreateNode(MaterialNodeType::Texture, nodePos);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Math"))
        {
            if (ImGui::MenuItem("Add")) CreateNode(MaterialNodeType::Add, nodePos);
            if (ImGui::MenuItem("Subtract")) CreateNode(MaterialNodeType::Subtract, nodePos);
            if (ImGui::MenuItem("Multiply")) CreateNode(MaterialNodeType::Multiply, nodePos);
            if (ImGui::MenuItem("Divide")) CreateNode(MaterialNodeType::Divide, nodePos);
            if (ImGui::MenuItem("Lerp")) CreateNode(MaterialNodeType::Lerp, nodePos);
            if (ImGui::MenuItem("Dot")) CreateNode(MaterialNodeType::Dot, nodePos);
            if (ImGui::MenuItem("Cross")) CreateNode(MaterialNodeType::Cross, nodePos);
            if (ImGui::MenuItem("Normalize")) CreateNode(MaterialNodeType::Normalize, nodePos);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Utility"))
        {
            if (ImGui::MenuItem("Fresnel")) CreateNode(MaterialNodeType::Fresnel, nodePos);
            if (ImGui::MenuItem("Time")) CreateNode(MaterialNodeType::Time, nodePos);
            if (ImGui::MenuItem("Tex Coord")) CreateNode(MaterialNodeType::TexCoord, nodePos);
            if (ImGui::MenuItem("World Position")) CreateNode(MaterialNodeType::WorldPosition, nodePos);
            if (ImGui::MenuItem("World Normal")) CreateNode(MaterialNodeType::WorldNormal, nodePos);
            if (ImGui::MenuItem("View Direction")) CreateNode(MaterialNodeType::ViewDirection, nodePos);
            ImGui::EndMenu();
        }

        ImGui::EndPopup();
    }
}

void MaterialEditorPanel::DrawPropertiesPanel()
{
    ImGui::Text("Properties");
    ImGui::Separator();

    if (m_selectedNode >= 0)
    {
        MaterialNode* node = nullptr;
        for (auto& n : m_nodes)
        {
            if (n.id == m_selectedNode)
            {
                node = &n;
                break;
            }
        }

        if (node)
        {
            ImGui::Text("Type: %s", GetNodeTypeName(node->type));
            ImGui::Separator();

            char nameBuffer[128];
            strncpy(nameBuffer, node->name.c_str(), sizeof(nameBuffer) - 1);
            if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
            {
                node->name = nameBuffer;
            }

            ImGui::Separator();

            // Type-specific properties
            switch (node->type)
            {
                case MaterialNodeType::Constant:
                    ImGui::ColorEdit4("Value", &node->constantValue.x);
                    break;

                case MaterialNodeType::Parameter:
                    {
                        char paramBuffer[128];
                        strncpy(paramBuffer, node->parameterName.c_str(), sizeof(paramBuffer) - 1);
                        if (ImGui::InputText("Parameter", paramBuffer, sizeof(paramBuffer)))
                        {
                            node->parameterName = paramBuffer;
                        }
                        ImGui::ColorEdit4("Default", &node->constantValue.x);
                    }
                    break;

                case MaterialNodeType::Texture:
                    ImGui::Text("Texture: %s", 
                               node->texturePath.empty() ? "(none)" : node->texturePath.c_str());
                    if (ImGui::Button("Browse...", ImVec2(-1, 0)))
                    {
                        // Open file browser
                    }
                    break;

                default:
                    ImGui::TextDisabled("No additional properties");
                    break;
            }

            ImGui::Separator();

            // Delete node button (except output)
            if (node->type != MaterialNodeType::Output)
            {
                if (ImGui::Button("Delete Node", ImVec2(-1, 0)))
                {
                    DeleteNode(node->id);
                    m_selectedNode = -1;
                }
            }
        }
    }
    else
    {
        ImGui::TextDisabled("Select a node");
    }
}

void MaterialEditorPanel::DrawPreviewPanel()
{
    ImGui::Text("Preview");
    ImGui::Separator();

    ImVec2 previewSize = ImGui::GetContentRegionAvail();
    previewSize.y -= 30;  // Reserve space for controls

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Draw preview background
    drawList->AddRectFilled(pos, ImVec2(pos.x + previewSize.x, pos.y + previewSize.y),
                            IM_COL32(40, 40, 45, 255));

    // Draw sphere preview (placeholder)
    ImVec2 center(pos.x + previewSize.x * 0.5f, pos.y + previewSize.y * 0.5f);
    float radius = std::min(previewSize.x, previewSize.y) * 0.35f;

    // Gradient sphere
    for (int i = 0; i < 32; ++i)
    {
        float t = static_cast<float>(i) / 32.0f;
        float r = radius * (1.0f - t * 0.5f);
        int brightness = static_cast<int>(200 - t * 150);
        drawList->AddCircleFilled(center, r, IM_COL32(brightness, brightness, brightness, 255));
    }

    ImGui::InvisibleButton("PreviewArea", previewSize);

    // Preview controls
    const char* meshTypes[] = { "Sphere", "Cube", "Plane", "Cylinder" };
    static int meshType = 0;
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("Mesh", &meshType, meshTypes, 4);

    ImGui::SameLine();

    if (ImGui::Button("Compile"))
    {
        // Compile material shader
    }
}

void MaterialEditorPanel::HandleNodeInteraction()
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 mousePos = io.MousePos;

    // Reset hover state
    m_hoveredNode = -1;

    // Canvas panning
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        m_canvasOffset.x += io.MouseDelta.x / m_canvasZoom;
        m_canvasOffset.y += io.MouseDelta.y / m_canvasZoom;
    }

    // Zoom with mouse wheel
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f)
    {
        float zoomDelta = io.MouseWheel * 0.1f;
        m_canvasZoom = std::clamp(m_canvasZoom + zoomDelta, 0.25f, 2.0f);
    }

    // Node selection and dragging
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered())
    {
        m_selectedNode = m_hoveredNode;
        if (m_hoveredNode >= 0)
        {
            m_isDraggingNode = true;
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_isDraggingNode = false;
        m_isCreatingLink = false;
    }

    if (m_isDraggingNode && m_selectedNode >= 0)
    {
        for (auto& node : m_nodes)
        {
            if (node.id == m_selectedNode)
            {
                node.position.x += io.MouseDelta.x / m_canvasZoom;
                node.position.y += io.MouseDelta.y / m_canvasZoom;
                break;
            }
        }
    }

    // Delete node with Delete key
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_selectedNode >= 0)
    {
        HandleNodeDeletion();
    }
}

void MaterialEditorPanel::HandleLinkCreation()
{
    // TODO: Implement link creation from pin dragging
}

void MaterialEditorPanel::HandleNodeDeletion()
{
    DeleteNode(m_selectedNode);
    m_selectedNode = -1;
}

MaterialNode* MaterialEditorPanel::CreateNode(MaterialNodeType type, Vec2 position)
{
    MaterialNode node;
    node.id = m_nextNodeId++;
    node.type = type;
    node.name = GetNodeTypeName(type);
    node.position = position;

    // Set up pins based on type
    switch (type)
    {
        case MaterialNodeType::Output:
            node.inputs.push_back({"BaseColor", MaterialPinType::Vec3, false, node.id, 0});
            node.inputs.push_back({"Metallic", MaterialPinType::Float, false, node.id, 1});
            node.inputs.push_back({"Roughness", MaterialPinType::Float, false, node.id, 2});
            node.inputs.push_back({"Normal", MaterialPinType::Vec3, false, node.id, 3});
            node.inputs.push_back({"Emissive", MaterialPinType::Vec3, false, node.id, 4});
            node.inputs.push_back({"AO", MaterialPinType::Float, false, node.id, 5});
            break;

        case MaterialNodeType::Constant:
            node.outputs.push_back({"Value", MaterialPinType::Vec4, true, node.id, 0});
            break;

        case MaterialNodeType::Parameter:
            node.outputs.push_back({"Value", MaterialPinType::Vec4, true, node.id, 0});
            break;

        case MaterialNodeType::Texture:
            node.inputs.push_back({"UV", MaterialPinType::Vec2, false, node.id, 0});
            node.outputs.push_back({"RGBA", MaterialPinType::Vec4, true, node.id, 0});
            node.outputs.push_back({"RGB", MaterialPinType::Vec3, true, node.id, 1});
            node.outputs.push_back({"R", MaterialPinType::Float, true, node.id, 2});
            break;

        case MaterialNodeType::Add:
        case MaterialNodeType::Subtract:
        case MaterialNodeType::Multiply:
        case MaterialNodeType::Divide:
            node.inputs.push_back({"A", MaterialPinType::Vec4, false, node.id, 0});
            node.inputs.push_back({"B", MaterialPinType::Vec4, false, node.id, 1});
            node.outputs.push_back({"Result", MaterialPinType::Vec4, true, node.id, 0});
            break;

        case MaterialNodeType::Lerp:
            node.inputs.push_back({"A", MaterialPinType::Vec4, false, node.id, 0});
            node.inputs.push_back({"B", MaterialPinType::Vec4, false, node.id, 1});
            node.inputs.push_back({"Alpha", MaterialPinType::Float, false, node.id, 2});
            node.outputs.push_back({"Result", MaterialPinType::Vec4, true, node.id, 0});
            break;

        case MaterialNodeType::Fresnel:
            node.inputs.push_back({"Exponent", MaterialPinType::Float, false, node.id, 0});
            node.outputs.push_back({"Result", MaterialPinType::Float, true, node.id, 0});
            break;

        case MaterialNodeType::Time:
            node.outputs.push_back({"Time", MaterialPinType::Float, true, node.id, 0});
            break;

        case MaterialNodeType::TexCoord:
            node.outputs.push_back({"UV", MaterialPinType::Vec2, true, node.id, 0});
            break;

        case MaterialNodeType::WorldPosition:
        case MaterialNodeType::WorldNormal:
        case MaterialNodeType::ViewDirection:
            node.outputs.push_back({"XYZ", MaterialPinType::Vec3, true, node.id, 0});
            break;

        default:
            break;
    }

    m_nodes.push_back(node);
    return &m_nodes.back();
}

void MaterialEditorPanel::DeleteNode(int nodeId)
{
    // Don't delete output node
    for (const auto& node : m_nodes)
    {
        if (node.id == nodeId && node.type == MaterialNodeType::Output)
            return;
    }

    // Remove links connected to this node
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
        [nodeId](const MaterialLink& link)
        {
            return link.sourceNodeId == nodeId || link.destNodeId == nodeId;
        }), m_links.end());

    // Remove node
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(),
        [nodeId](const MaterialNode& node) { return node.id == nodeId; }),
        m_nodes.end());
}

void MaterialEditorPanel::CreateLink(int sourceNode, int sourcePin, int destNode, int destPin)
{
    MaterialLink link;
    link.id = m_nextLinkId++;
    link.sourceNodeId = sourceNode;
    link.sourcePinIndex = sourcePin;
    link.destNodeId = destNode;
    link.destPinIndex = destPin;
    m_links.push_back(link);
}

void MaterialEditorPanel::DeleteLink(int linkId)
{
    m_links.erase(std::remove_if(m_links.begin(), m_links.end(),
        [linkId](const MaterialLink& link) { return link.id == linkId; }),
        m_links.end());
}

Vec2 MaterialEditorPanel::GetPinPosition(int nodeId, int pinIndex, bool isOutput) const
{
    for (const auto& node : m_nodes)
    {
        if (node.id == nodeId)
        {
            float x = isOutput ? node.position.x + m_nodeWidth : node.position.x;
            float y = node.position.y + 30.0f + pinIndex * 22.0f + 12.0f;
            return Vec2(x, y);
        }
    }
    return Vec2(0.0f, 0.0f);
}

MaterialPinType MaterialEditorPanel::GetPinType(int nodeId, int pinIndex, bool isOutput) const
{
    for (const auto& node : m_nodes)
    {
        if (node.id == nodeId)
        {
            const auto& pins = isOutput ? node.outputs : node.inputs;
            if (pinIndex >= 0 && pinIndex < static_cast<int>(pins.size()))
            {
                return pins[pinIndex].type;
            }
        }
    }
    return MaterialPinType::Float;
}

const char* MaterialEditorPanel::GetNodeTypeName(MaterialNodeType type) const
{
    switch (type)
    {
        case MaterialNodeType::Output: return "Material Output";
        case MaterialNodeType::Constant: return "Constant";
        case MaterialNodeType::Parameter: return "Parameter";
        case MaterialNodeType::Texture: return "Texture Sample";
        case MaterialNodeType::Add: return "Add";
        case MaterialNodeType::Subtract: return "Subtract";
        case MaterialNodeType::Multiply: return "Multiply";
        case MaterialNodeType::Divide: return "Divide";
        case MaterialNodeType::Lerp: return "Lerp";
        case MaterialNodeType::Dot: return "Dot";
        case MaterialNodeType::Cross: return "Cross";
        case MaterialNodeType::Normalize: return "Normalize";
        case MaterialNodeType::Fresnel: return "Fresnel";
        case MaterialNodeType::Time: return "Time";
        case MaterialNodeType::TexCoord: return "Texture Coordinate";
        case MaterialNodeType::WorldPosition: return "World Position";
        case MaterialNodeType::WorldNormal: return "World Normal";
        case MaterialNodeType::ViewDirection: return "View Direction";
        default: return "Unknown";
    }
}

uint32 MaterialEditorPanel::GetPinTypeColor(MaterialPinType type) const
{
    switch (type)
    {
        case MaterialPinType::Float: return IM_COL32(150, 200, 150, 255);
        case MaterialPinType::Vec2: return IM_COL32(200, 200, 100, 255);
        case MaterialPinType::Vec3: return IM_COL32(100, 200, 200, 255);
        case MaterialPinType::Vec4: return IM_COL32(200, 150, 200, 255);
        case MaterialPinType::Texture2D: return IM_COL32(200, 100, 100, 255);
        case MaterialPinType::TextureCube: return IM_COL32(200, 150, 100, 255);
        default: return IM_COL32(180, 180, 180, 255);
    }
}

void MaterialEditorPanel::SetMaterial(std::shared_ptr<Material> material)
{
    m_material = material;
    // TODO: Load material graph from material
}

void MaterialEditorPanel::NewMaterial()
{
    m_nodes.clear();
    m_links.clear();
    m_nextNodeId = 1;
    m_nextLinkId = 1;
    m_selectedNode = -1;

    // Create output node
    CreateNode(MaterialNodeType::Output, Vec2(300.0f, 100.0f));
}

void MaterialEditorPanel::SaveMaterial()
{
    // TODO: Save material graph
}

void MaterialEditorPanel::LoadMaterial(const std::string& path)
{
    (void)path;
    // TODO: Load material graph from file
}

} // namespace RVX::Editor
