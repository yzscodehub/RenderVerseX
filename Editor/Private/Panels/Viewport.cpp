/**
 * @file Viewport.cpp
 * @brief Viewport panel implementation
 */

#include "Editor/Panels/Viewport.h"
#include "Editor/EditorContext.h"
#include "Core/Log.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>

namespace RVX::Editor
{

ViewportPanel::ViewportPanel()
{
    ResetCamera();
}

void ViewportPanel::OnInit()
{
    // TODO: Create render target
}

void ViewportPanel::OnUpdate(float deltaTime)
{
    if (m_isHovered || m_isNavigating)
    {
        UpdateCamera(deltaTime);
    }
}

void ViewportPanel::OnGUI()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    
    if (ImGui::Begin(GetName(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        m_isFocused = ImGui::IsWindowFocused();
        m_isHovered = ImGui::IsWindowHovered();

        DrawToolbar();
        
        // Get viewport content region
        ImVec2 viewportPanelSize = ImGui::GetContentRegionAvail();
        m_viewportWidth = static_cast<uint32>(viewportPanelSize.x);
        m_viewportHeight = static_cast<uint32>(viewportPanelSize.y);

        ImVec2 viewportPos = ImGui::GetCursorScreenPos();
        m_viewportPos.x = viewportPos.x;
        m_viewportPos.y = viewportPos.y;

        DrawViewportContent();
        
        if (m_showGizmo)
        {
            DrawGizmo();
        }

        DrawOverlays();
        HandleInput();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void ViewportPanel::UpdateCamera(float deltaTime)
{
    switch (m_cameraMode)
    {
        case ViewportCameraMode::Orbit:
            UpdateOrbitCamera(deltaTime);
            break;
        case ViewportCameraMode::Fly:
            UpdateFlyCamera(deltaTime);
            break;
        case ViewportCameraMode::TopDown:
            // Top-down mode uses orbit logic but locks pitch
            UpdateOrbitCamera(deltaTime);
            break;
    }
}

void ViewportPanel::UpdateOrbitCamera(float deltaTime)
{
    (void)deltaTime;

    ImGuiIO& io = ImGui::GetIO();

    // Right mouse: rotate camera around target
    if (m_rightMouseDown && m_isHovered)
    {
        float dx = io.MouseDelta.x * m_cameraRotateSpeed;
        float dy = io.MouseDelta.y * m_cameraRotateSpeed;

        m_cameraYaw += dx;
        m_cameraPitch -= dy;

        // Clamp pitch
        m_cameraPitch = glm::clamp(m_cameraPitch, -89.0f, 89.0f);
    }

    // Middle mouse: pan camera
    if (m_middleMouseDown && m_isHovered)
    {
        float panSpeed = m_cameraDistance * 0.002f;
        
        // Calculate right and up vectors
        float yawRad = glm::radians(m_cameraYaw);
        float pitchRad = glm::radians(m_cameraPitch);

        Vec3 forward;
        forward.x = std::cos(pitchRad) * std::cos(yawRad);
        forward.y = std::sin(pitchRad);
        forward.z = std::cos(pitchRad) * std::sin(yawRad);
        
        Vec3 right = glm::normalize(glm::cross(forward, m_cameraUp));
        Vec3 up = glm::normalize(glm::cross(right, forward));

        Vec3 pan = -io.MouseDelta.x * panSpeed * right + io.MouseDelta.y * panSpeed * up;
        m_cameraTarget += pan;
    }

    // Mouse wheel: zoom
    if (m_isHovered && std::abs(io.MouseWheel) > 0.0f)
    {
        float zoomSpeed = m_cameraDistance * 0.1f;
        m_cameraDistance -= io.MouseWheel * zoomSpeed;
        m_cameraDistance = glm::clamp(m_cameraDistance, 0.1f, 1000.0f);
    }

    // Calculate camera position from spherical coordinates
    float yawRad = glm::radians(m_cameraYaw);
    float pitchRad = glm::radians(m_cameraPitch);

    m_cameraPosition.x = m_cameraTarget.x + m_cameraDistance * std::cos(pitchRad) * std::cos(yawRad);
    m_cameraPosition.y = m_cameraTarget.y + m_cameraDistance * std::sin(pitchRad);
    m_cameraPosition.z = m_cameraTarget.z + m_cameraDistance * std::cos(pitchRad) * std::sin(yawRad);
}

void ViewportPanel::UpdateFlyCamera(float deltaTime)
{
    ImGuiIO& io = ImGui::GetIO();

    // Right mouse: look around
    if (m_rightMouseDown && m_isHovered)
    {
        float dx = io.MouseDelta.x * m_cameraRotateSpeed;
        float dy = io.MouseDelta.y * m_cameraRotateSpeed;

        m_cameraYaw += dx;
        m_cameraPitch -= dy;
        m_cameraPitch = glm::clamp(m_cameraPitch, -89.0f, 89.0f);
    }

    // Calculate forward direction
    float yawRad = glm::radians(m_cameraYaw);
    float pitchRad = glm::radians(m_cameraPitch);

    Vec3 forward;
    forward.x = std::cos(pitchRad) * std::cos(yawRad);
    forward.y = std::sin(pitchRad);
    forward.z = std::cos(pitchRad) * std::sin(yawRad);
    forward = glm::normalize(forward);

    Vec3 right = glm::normalize(glm::cross(forward, m_cameraUp));

    // WASD movement (only when right mouse is held)
    if (m_rightMouseDown && m_isHovered)
    {
        float speed = m_cameraMoveSpeed * deltaTime;
        
        // Shift for faster movement
        if (io.KeyShift)
            speed *= 3.0f;

        if (ImGui::IsKeyDown(ImGuiKey_W))
            m_cameraPosition += forward * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S))
            m_cameraPosition -= forward * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A))
            m_cameraPosition -= right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D))
            m_cameraPosition += right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_Q))
            m_cameraPosition -= m_cameraUp * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E))
            m_cameraPosition += m_cameraUp * speed;
    }

    // Update target to be in front of camera
    m_cameraTarget = m_cameraPosition + forward * m_cameraDistance;
}

void ViewportPanel::DrawToolbar()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::BeginChild("ViewportToolbar", ImVec2(0, 32), false, 
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    auto& context = EditorContext::Get();

    // Gizmo mode buttons
    bool isTranslate = context.GetGizmoMode() == EditorContext::GizmoMode::Translate;
    bool isRotate = context.GetGizmoMode() == EditorContext::GizmoMode::Rotate;
    bool isScale = context.GetGizmoMode() == EditorContext::GizmoMode::Scale;

    if (ImGui::RadioButton("Translate (W)", isTranslate))
        context.SetGizmoMode(EditorContext::GizmoMode::Translate);
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", isRotate))
        context.SetGizmoMode(EditorContext::GizmoMode::Rotate);
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale (R)", isScale))
        context.SetGizmoMode(EditorContext::GizmoMode::Scale);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Gizmo space
    bool isWorld = context.GetGizmoSpace() == EditorContext::GizmoSpace::World;
    if (ImGui::RadioButton("World", isWorld))
        context.SetGizmoSpace(EditorContext::GizmoSpace::World);
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", !isWorld))
        context.SetGizmoSpace(EditorContext::GizmoSpace::Local);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Snap toggle
    bool snapEnabled = context.IsSnapEnabled();
    if (ImGui::Checkbox("Snap", &snapEnabled))
        context.SetSnapEnabled(snapEnabled);

    if (snapEnabled)
    {
        ImGui::SameLine();
        float snapValue = context.GetSnapValue();
        ImGui::SetNextItemWidth(60);
        if (ImGui::DragFloat("##SnapValue", &snapValue, 0.1f, 0.1f, 100.0f))
            context.SetSnapValue(snapValue);
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Camera mode
    const char* cameraModes[] = { "Orbit", "Fly", "Top" };
    int cameraMode = static_cast<int>(m_cameraMode);
    ImGui::SetNextItemWidth(80);
    if (ImGui::Combo("##CameraMode", &cameraMode, cameraModes, 3))
        m_cameraMode = static_cast<ViewportCameraMode>(cameraMode);

    ImGui::SameLine();

    // Shading mode
    const char* shadingModes[] = { "Lit", "Unlit", "Wireframe", "Normals", "Depth", "Albedo" };
    int shadingMode = static_cast<int>(m_shadingMode);
    ImGui::SetNextItemWidth(90);
    if (ImGui::Combo("##ShadingMode", &shadingMode, shadingModes, 6))
        m_shadingMode = static_cast<ViewportShadingMode>(shadingMode);

    ImGui::SameLine();

    // Display options dropdown
    if (ImGui::BeginCombo("##Options", "Options", ImGuiComboFlags_NoPreview))
    {
        ImGui::Checkbox("Grid", &m_showGrid);
        ImGui::Checkbox("Gizmo", &m_showGizmo);
        ImGui::Checkbox("Stats", &m_showStats);
        ImGui::Checkbox("Bounds", &m_showBounds);
        ImGui::Checkbox("Debug Draw", &m_showDebugDraw);
        ImGui::EndCombo();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    
    ImGui::Separator();
}

void ViewportPanel::DrawGizmo()
{
    auto& context = EditorContext::Get();
    Entity* selectedEntity = context.GetSelectedEntity();
    
    if (!selectedEntity)
        return;

    // Set up ImGuizmo
    ImGuizmo::SetOrthographic(m_cameraMode == ViewportCameraMode::TopDown);
    ImGuizmo::SetDrawlist();
    
    ImGuizmo::SetRect(m_viewportPos.x, m_viewportPos.y, 
                      static_cast<float>(m_viewportWidth), 
                      static_cast<float>(m_viewportHeight));

    // Get matrices
    Mat4 view = GetViewMatrix();
    Mat4 projection = GetProjectionMatrix();

    // TODO: Get entity transform matrix
    Mat4 transform = glm::mat4(1.0f);  // Identity for now

    // Determine gizmo operation
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    switch (context.GetGizmoMode())
    {
        case EditorContext::GizmoMode::Translate: operation = ImGuizmo::TRANSLATE; break;
        case EditorContext::GizmoMode::Rotate: operation = ImGuizmo::ROTATE; break;
        case EditorContext::GizmoMode::Scale: operation = ImGuizmo::SCALE; break;
    }

    // Determine gizmo mode
    ImGuizmo::MODE mode = context.GetGizmoSpace() == EditorContext::GizmoSpace::World 
                          ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

    // Snap values
    float snapValues[3] = { context.GetSnapValue(), context.GetSnapValue(), context.GetSnapValue() };
    float* snap = context.IsSnapEnabled() ? snapValues : nullptr;

    // Draw and manipulate gizmo
    if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
                              operation, mode, glm::value_ptr(transform), nullptr, snap))
    {
        // TODO: Apply transform to entity
        if (ImGuizmo::IsUsing())
        {
            context.MarkSceneDirty();
        }
    }
}

void ViewportPanel::DrawViewportContent()
{
    // Draw a placeholder viewport (dark background)
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), 
                            IM_COL32(30, 30, 35, 255));

    // Draw grid (placeholder)
    if (m_showGrid)
    {
        // Simple grid visualization
        ImU32 gridColor = IM_COL32(50, 50, 55, 255);
        float gridSpacing = 50.0f;
        
        for (float x = pos.x; x < pos.x + size.x; x += gridSpacing)
        {
            drawList->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y), gridColor);
        }
        for (float y = pos.y; y < pos.y + size.y; y += gridSpacing)
        {
            drawList->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + size.x, y), gridColor);
        }
    }

    // Draw center cross
    ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    drawList->AddLine(ImVec2(center.x - 20, center.y), ImVec2(center.x + 20, center.y), 
                      IM_COL32(100, 100, 100, 255));
    drawList->AddLine(ImVec2(center.x, center.y - 20), ImVec2(center.x, center.y + 20), 
                      IM_COL32(100, 100, 100, 255));

    // Make viewport interactive
    ImGui::InvisibleButton("ViewportInteractive", size, 
                           ImGuiButtonFlags_MouseButtonLeft | 
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
}

void ViewportPanel::DrawOverlays()
{
    if (!m_showStats)
        return;

    ImVec2 pos = ImVec2(m_viewportPos.x + 10, m_viewportPos.y + 40);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background for stats
    ImVec2 statsSize(200, 100);
    drawList->AddRectFilled(pos, ImVec2(pos.x + statsSize.x, pos.y + statsSize.y),
                            IM_COL32(0, 0, 0, 150), 4.0f);

    // Camera info
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "Camera: %.1f, %.1f, %.1f", 
             m_cameraPosition.x, m_cameraPosition.y, m_cameraPosition.z);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 8), IM_COL32(200, 200, 200, 255), buffer);

    snprintf(buffer, sizeof(buffer), "Target: %.1f, %.1f, %.1f",
             m_cameraTarget.x, m_cameraTarget.y, m_cameraTarget.z);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 26), IM_COL32(200, 200, 200, 255), buffer);

    snprintf(buffer, sizeof(buffer), "Distance: %.2f", m_cameraDistance);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 44), IM_COL32(200, 200, 200, 255), buffer);

    snprintf(buffer, sizeof(buffer), "Viewport: %ux%u", m_viewportWidth, m_viewportHeight);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 62), IM_COL32(200, 200, 200, 255), buffer);

    const char* modeStr = m_cameraMode == ViewportCameraMode::Orbit ? "Orbit" :
                          m_cameraMode == ViewportCameraMode::Fly ? "Fly" : "TopDown";
    snprintf(buffer, sizeof(buffer), "Mode: %s", modeStr);
    drawList->AddText(ImVec2(pos.x + 8, pos.y + 80), IM_COL32(200, 200, 200, 255), buffer);
}

void ViewportPanel::HandleInput()
{
    ImGuiIO& io = ImGui::GetIO();

    // Track mouse button state
    m_rightMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    m_middleMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    // Start navigation
    if (m_isHovered && (m_rightMouseDown || m_middleMouseDown))
    {
        m_isNavigating = true;
    }
    else if (!m_rightMouseDown && !m_middleMouseDown)
    {
        m_isNavigating = false;
    }

    // Focus shortcut
    if (m_isHovered && ImGui::IsKeyPressed(ImGuiKey_F))
    {
        FocusOnSelection();
    }

    // Mouse picking
    if (m_isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing())
    {
        HandleMousePick();
    }
}

void ViewportPanel::HandleMousePick()
{
    // TODO: Implement ray casting for mouse picking
    // 1. Convert mouse position to NDC
    // 2. Create ray from camera through mouse position
    // 3. Perform ray cast against scene entities
    // 4. Select nearest hit entity
}

void ViewportPanel::FocusOnSelection()
{
    auto& context = EditorContext::Get();
    Entity* selected = context.GetSelectedEntity();
    
    if (selected)
    {
        // TODO: Get entity position and bounds
        // m_cameraTarget = entity->GetPosition();
        // m_cameraDistance = CalculateFitDistance(entity->GetBounds());
    }
}

void ViewportPanel::ResetCamera()
{
    m_cameraPosition = Vec3(0.0f, 5.0f, 10.0f);
    m_cameraTarget = Vec3(0.0f, 0.0f, 0.0f);
    m_cameraYaw = -90.0f;
    m_cameraPitch = -15.0f;
    m_cameraDistance = 10.0f;
}

Mat4 ViewportPanel::GetViewMatrix() const
{
    return glm::lookAt(m_cameraPosition, m_cameraTarget, m_cameraUp);
}

Mat4 ViewportPanel::GetProjectionMatrix() const
{
    float aspect = static_cast<float>(m_viewportWidth) / static_cast<float>(m_viewportHeight);
    
    if (m_cameraMode == ViewportCameraMode::TopDown)
    {
        float orthoSize = m_cameraDistance;
        return glm::ortho(-orthoSize * aspect, orthoSize * aspect, 
                          -orthoSize, orthoSize, m_cameraNear, m_cameraFar);
    }
    
    return glm::perspective(glm::radians(m_cameraFOV), aspect, m_cameraNear, m_cameraFar);
}

} // namespace RVX::Editor
