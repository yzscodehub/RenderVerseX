/**
 * @file ParticleEditor.cpp
 * @brief Particle editor panel implementation
 */

#include "Editor/Panels/ParticleEditor.h"
#include "Editor/EditorContext.h"
#include "Core/Log.h"

#include <imgui.h>
#include <cmath>

namespace RVX::Editor
{

ParticleEditorPanel::ParticleEditorPanel() = default;

ParticleEditorPanel::~ParticleEditorPanel()
{
    if (m_previewInstance)
    {
        delete m_previewInstance;
        m_previewInstance = nullptr;
    }
}

void ParticleEditorPanel::OnInit()
{
}

void ParticleEditorPanel::SetParticleSystem(std::shared_ptr<Particle::ParticleSystem> system)
{
    if (m_previewInstance)
    {
        delete m_previewInstance;
        m_previewInstance = nullptr;
    }

    m_system = system;
    m_selectedEmitter = -1;
    m_selectedModule = -1;

    // TODO: Create preview instance
}

void ParticleEditorPanel::OnUpdate(float deltaTime)
{
    if (m_previewInstance && !m_previewPaused)
    {
        // TODO: Simulate preview instance
        m_previewTime += deltaTime;
    }
}

void ParticleEditorPanel::OnGUI()
{
    if (!ImGui::Begin(GetName()))
    {
        ImGui::End();
        return;
    }

    DrawToolbar();

    if (!m_system)
    {
        ImGui::TextDisabled("No particle system loaded.");
        ImGui::Text("Create a new system or load an existing one.");
        
        if (ImGui::Button("New Particle System", ImVec2(-1, 30)))
        {
            // TODO: Create new particle system
        }
        
        ImGui::End();
        return;
    }

    // Main content area with columns
    ImGui::Columns(2, "ParticleEditorColumns", true);

    // Left panel: Properties
    ImGui::BeginChild("Properties", ImVec2(0, 0), true);
    {
        if (ImGui::CollapsingHeader("System", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawSystemProperties();
        }

        if (ImGui::CollapsingHeader("Emitters", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawEmitterList();
        }

        if (ImGui::CollapsingHeader("Modules", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawModuleList();
        }

        if (ImGui::CollapsingHeader("Rendering"))
        {
            DrawRenderingProperties();
        }

        if (ImGui::CollapsingHeader("LOD"))
        {
            DrawLODProperties();
        }
    }
    ImGui::EndChild();

    ImGui::NextColumn();

    // Right panel: Preview
    DrawPreviewWindow();

    ImGui::Columns(1);
    ImGui::End();
}

void ParticleEditorPanel::DrawToolbar()
{
    if (ImGui::Button("New"))
    {
        // Create new particle system
    }
    ImGui::SameLine();

    if (ImGui::Button("Open"))
    {
        // Open file dialog
    }
    ImGui::SameLine();

    if (ImGui::Button("Save") && m_system)
    {
        // Save file dialog
    }
    ImGui::SameLine();

    ImGui::Separator();
    ImGui::SameLine();

    // Preview controls
    if (ImGui::Button(m_previewPaused ? "Play" : "Pause"))
    {
        if (m_previewPaused)
            PlayPreview();
        else
            PausePreview();
    }
    ImGui::SameLine();

    if (ImGui::Button("Stop"))
    {
        StopPreview();
    }
    ImGui::SameLine();

    if (ImGui::Button("Restart"))
    {
        RestartPreview();
    }
    ImGui::SameLine();

    // Particle count
    ImGui::Text("Particles: %u", m_previewInstance ? 0u : 0u);  // TODO: Get actual count

    ImGui::Separator();
}

void ParticleEditorPanel::DrawSystemProperties()
{
    // Demo properties
    static char name[256] = "New Particle System";
    ImGui::InputText("Name", name, sizeof(name));

    static int maxParticles = 1000;
    ImGui::DragInt("Max Particles", &maxParticles, 100, 100, 1000000);

    static float duration = 5.0f;
    ImGui::DragFloat("Duration", &duration, 0.1f, 0.0f, 60.0f);

    static bool looping = true;
    ImGui::Checkbox("Looping", &looping);

    static bool prewarm = false;
    ImGui::Checkbox("Prewarm", &prewarm);
    
    if (prewarm)
    {
        static float prewarmTime = 1.0f;
        ImGui::DragFloat("Prewarm Time", &prewarmTime, 0.1f, 0.0f, 10.0f);
    }

    static float startDelay = 0.0f;
    ImGui::DragFloat("Start Delay", &startDelay, 0.1f, 0.0f, 10.0f);

    static float simulationSpeed = 1.0f;
    ImGui::DragFloat("Simulation Speed", &simulationSpeed, 0.01f, 0.0f, 10.0f);

    const char* spaceItems[] = { "World", "Local" };
    static int space = 0;
    ImGui::Combo("Simulation Space", &space, spaceItems, 2);
}

void ParticleEditorPanel::DrawEmitterList()
{
    if (ImGui::Button("Add Emitter"))
    {
        ImGui::OpenPopup("AddEmitterPopup");
    }

    if (ImGui::BeginPopup("AddEmitterPopup"))
    {
        if (ImGui::MenuItem("Point")) {}
        if (ImGui::MenuItem("Box")) {}
        if (ImGui::MenuItem("Sphere")) {}
        if (ImGui::MenuItem("Cone")) {}
        if (ImGui::MenuItem("Circle")) {}
        if (ImGui::MenuItem("Edge")) {}
        if (ImGui::MenuItem("Mesh")) {}
        ImGui::EndPopup();
    }

    // Demo emitters
    const char* emitterNames[] = { "Point Emitter", "Sphere Emitter" };
    for (int i = 0; i < 2; ++i)
    {
        bool selected = (m_selectedEmitter == i);
        
        ImGui::PushID(i);
        
        if (ImGui::Selectable(emitterNames[i], selected))
        {
            m_selectedEmitter = i;
        }

        if (selected)
        {
            DrawEmitterProperties(i);
        }

        ImGui::PopID();
    }
}

void ParticleEditorPanel::DrawEmitterProperties(int index)
{
    (void)index;
    
    ImGui::Indent();

    static bool enabled = true;
    ImGui::Checkbox("Enabled", &enabled);

    static float emissionRate = 100.0f;
    ImGui::DragFloat("Emission Rate", &emissionRate, 1.0f, 0.0f, 10000.0f);
    
    static int burstCount = 0;
    ImGui::DragInt("Burst Count", &burstCount, 1, 0, 1000);
    
    static float burstInterval = 1.0f;
    ImGui::DragFloat("Burst Interval", &burstInterval, 0.1f, 0.0f, 10.0f);

    ImGui::Separator();
    ImGui::Text("Initial Properties");

    static float lifetimeMin = 1.0f, lifetimeMax = 2.0f;
    DrawFloatRangeEditor("Lifetime", lifetimeMin, lifetimeMax);

    static float speedMin = 1.0f, speedMax = 5.0f;
    DrawFloatRangeEditor("Speed", speedMin, speedMax);

    static float sizeMin = 0.1f, sizeMax = 0.5f;
    DrawFloatRangeEditor("Size", sizeMin, sizeMax);

    ImGui::Unindent();
}

void ParticleEditorPanel::DrawModuleList()
{
    if (ImGui::Button("Add Module"))
    {
        ImGui::OpenPopup("AddModulePopup");
    }

    if (ImGui::BeginPopup("AddModulePopup"))
    {
        if (ImGui::MenuItem("Force")) {}
        if (ImGui::MenuItem("Color Over Lifetime")) {}
        if (ImGui::MenuItem("Size Over Lifetime")) {}
        if (ImGui::MenuItem("Velocity Over Lifetime")) {}
        if (ImGui::MenuItem("Rotation Over Lifetime")) {}
        if (ImGui::MenuItem("Noise")) {}
        if (ImGui::MenuItem("Collision")) {}
        if (ImGui::MenuItem("Texture Sheet")) {}
        if (ImGui::MenuItem("Trail")) {}
        if (ImGui::MenuItem("Lights")) {}
        if (ImGui::MenuItem("Sub Emitter")) {}
        ImGui::EndPopup();
    }

    // Demo modules
    const char* moduleNames[] = { "Force", "Color Over Lifetime" };
    for (int i = 0; i < 2; ++i)
    {
        bool selected = (m_selectedModule == i);
        
        ImGui::PushID(i + 100);
        
        if (ImGui::Selectable(moduleNames[i], selected))
        {
            m_selectedModule = i;
        }

        if (selected)
        {
            DrawModuleProperties(i);
        }

        ImGui::PopID();
    }
}

void ParticleEditorPanel::DrawModuleProperties(int index)
{
    ImGui::Indent();

    static bool enabled = true;
    ImGui::Checkbox("Enabled", &enabled);

    if (index == 0)  // Force module
    {
        static Vec3 gravity(0.0f, -9.8f, 0.0f);
        ImGui::DragFloat3("Gravity", &gravity.x, 0.1f);

        static Vec3 constantForce(0.0f);
        ImGui::DragFloat3("Constant Force", &constantForce.x, 0.1f);

        static float drag = 0.0f;
        ImGui::DragFloat("Drag", &drag, 0.01f, 0.0f, 1.0f);
    }
    else if (index == 1)  // Color Over Lifetime
    {
        ImGui::Text("Color Gradient:");
        
        // Simple gradient preview
        ImVec2 barSize(180, 20);
        ImVec2 barPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        for (int i = 0; i < static_cast<int>(barSize.x); ++i)
        {
            float t = static_cast<float>(i) / barSize.x;
            int r = static_cast<int>(255 * (1.0f - t));
            int g = static_cast<int>(255 * t);
            int b = 128;
            int a = static_cast<int>(255 * (1.0f - t * 0.5f));
            
            drawList->AddLine(
                ImVec2(barPos.x + i, barPos.y),
                ImVec2(barPos.x + i, barPos.y + barSize.y),
                IM_COL32(r, g, b, a)
            );
        }

        drawList->AddRect(barPos, 
                          ImVec2(barPos.x + barSize.x, barPos.y + barSize.y),
                          IM_COL32(100, 100, 100, 255));

        ImGui::InvisibleButton("GradientBar", barSize);

        if (ImGui::Button("Edit Gradient..."))
        {
            m_gradientEditorOpen = true;
        }
    }

    ImGui::Unindent();
}

void ParticleEditorPanel::DrawRenderingProperties()
{
    const char* renderModes[] = { "Billboard", "Stretched Billboard", "Horizontal", 
                                   "Vertical", "Mesh", "Trail" };
    static int renderMode = 0;
    ImGui::Combo("Render Mode", &renderMode, renderModes, 6);

    const char* blendModes[] = { "Additive", "Alpha Blend", "Multiply", "Premultiplied" };
    static int blendMode = 0;
    ImGui::Combo("Blend Mode", &blendMode, blendModes, 4);

    ImGui::Separator();
    ImGui::Text("Soft Particles");
    
    static bool softEnabled = true;
    ImGui::Checkbox("Enable Soft Particles", &softEnabled);
    
    if (softEnabled)
    {
        static float fadeDistance = 1.0f;
        ImGui::DragFloat("Fade Distance", &fadeDistance, 0.1f, 0.0f, 10.0f);

        static float contrast = 1.0f;
        ImGui::DragFloat("Contrast", &contrast, 0.1f, 0.1f, 5.0f);
    }
}

void ParticleEditorPanel::DrawLODProperties()
{
    static bool lodEnabled = true;
    ImGui::Checkbox("Enable LOD", &lodEnabled);
    
    if (lodEnabled)
    {
        static float cullDistance = 100.0f;
        ImGui::DragFloat("Cull Distance", &cullDistance, 1.0f, 0.0f, 1000.0f);

        static bool fadeTransition = true;
        ImGui::Checkbox("Fade Transition", &fadeTransition);
        
        if (fadeTransition)
        {
            static float fadeRange = 10.0f;
            ImGui::DragFloat("Fade Range", &fadeRange, 0.5f, 0.0f, 50.0f);
        }

        ImGui::Text("LOD Levels: 3");
    }
}

void ParticleEditorPanel::DrawPreviewWindow()
{
    ImGui::BeginChild("Preview", ImVec2(0, 0), true);
    
    ImGui::Text("Preview");
    ImGui::Separator();

    ImGui::Text("Time: %.2f s", m_previewTime);
    ImGui::Text("Particles: 0 / 1000");
    ImGui::Text("State: %s", m_previewPaused ? "Paused" : "Playing");

    // Preview viewport
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("PreviewViewport", size);
    
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, IM_COL32(30, 30, 30, 255));
    drawList->AddRect(min, max, IM_COL32(60, 60, 60, 255));
    
    // Draw some placeholder particles
    ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    
    for (int i = 0; i < 50; ++i)
    {
        float angle = (i * 0.1f + m_previewTime * 2.0f);
        float radius = 50.0f + i * 1.5f;
        float x = center.x + std::cos(angle) * radius;
        float y = center.y + std::sin(angle) * radius;
        
        float alpha = 1.0f - (i / 50.0f);
        float size = 3.0f + i * 0.1f;
        
        drawList->AddCircleFilled(ImVec2(x, y), size, 
                                   IM_COL32(255, 200, 100, static_cast<int>(alpha * 255)));
    }

    ImGui::EndChild();
}

void ParticleEditorPanel::DrawCurveEditor(const char* label, float* curve, int numKeys)
{
    (void)curve;
    (void)numKeys;
    
    ImGui::Text("%s", label);
    
    ImVec2 canvasSize(200, 80);
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background
    drawList->AddRectFilled(canvasPos, 
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(40, 40, 40, 255));

    // Grid line
    drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y * 0.5f),
                      ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y * 0.5f),
                      IM_COL32(60, 60, 60, 255));

    // Demo curve
    for (int i = 0; i < static_cast<int>(canvasSize.x) - 1; ++i)
    {
        float t1 = static_cast<float>(i) / canvasSize.x;
        float t2 = static_cast<float>(i + 1) / canvasSize.x;
        float v1 = 1.0f - t1 * t1;  // Simple ease-out
        float v2 = 1.0f - t2 * t2;

        ImVec2 p1(canvasPos.x + i, canvasPos.y + canvasSize.y * (1.0f - v1));
        ImVec2 p2(canvasPos.x + i + 1, canvasPos.y + canvasSize.y * (1.0f - v2));
        drawList->AddLine(p1, p2, IM_COL32(100, 200, 100, 255), 2.0f);
    }

    ImGui::InvisibleButton("CurveCanvas", canvasSize);
}

void ParticleEditorPanel::DrawGradientEditor(const char* label, Vec4* colors, float* times, int numKeys)
{
    (void)colors;
    (void)times;
    (void)numKeys;
    
    ImGui::Text("%s", label);
    
    ImVec2 barSize(200, 20);
    ImVec2 barPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Demo gradient
    for (int i = 0; i < static_cast<int>(barSize.x); ++i)
    {
        float t = static_cast<float>(i) / barSize.x;
        int r = static_cast<int>(255);
        int g = static_cast<int>(255 * (1.0f - t));
        int b = static_cast<int>(100);
        int a = static_cast<int>(255 * (1.0f - t * 0.5f));
        
        drawList->AddLine(
            ImVec2(barPos.x + i, barPos.y),
            ImVec2(barPos.x + i, barPos.y + barSize.y),
            IM_COL32(r, g, b, a)
        );
    }

    drawList->AddRect(barPos, 
                      ImVec2(barPos.x + barSize.x, barPos.y + barSize.y),
                      IM_COL32(100, 100, 100, 255));

    ImGui::InvisibleButton("GradientBar", barSize);
}

void ParticleEditorPanel::DrawFloatRangeEditor(const char* label, float& min, float& max)
{
    float values[2] = { min, max };
    if (ImGui::DragFloat2(label, values, 0.1f))
    {
        min = values[0];
        max = values[1];
    }
}

void ParticleEditorPanel::DrawVec3RangeEditor(const char* label, Vec3& min, Vec3& max)
{
    ImGui::Text("%s", label);
    ImGui::DragFloat3("Min", &min.x, 0.1f);
    ImGui::DragFloat3("Max", &max.x, 0.1f);
}

void ParticleEditorPanel::DrawColorRangeEditor(const char* label, Vec4& min, Vec4& max)
{
    ImGui::Text("%s", label);
    ImGui::ColorEdit4("Min", &min.x);
    ImGui::ColorEdit4("Max", &max.x);
}

void ParticleEditorPanel::PlayPreview()
{
    m_previewPaused = false;
}

void ParticleEditorPanel::PausePreview()
{
    m_previewPaused = true;
}

void ParticleEditorPanel::StopPreview()
{
    m_previewPaused = false;
    m_previewTime = 0.0f;
}

void ParticleEditorPanel::RestartPreview()
{
    m_previewPaused = false;
    m_previewTime = 0.0f;
}

} // namespace RVX::Editor
