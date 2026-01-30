/**
 * @file AnimationEditor.cpp
 * @brief Animation editor panel implementation
 */

#include "Editor/Panels/AnimationEditor.h"
#include "Editor/EditorContext.h"
#include "Core/Log.h"

#include <imgui.h>
#include <cmath>
#include <algorithm>

namespace RVX::Editor
{

AnimationEditorPanel::AnimationEditorPanel()
{
}

void AnimationEditorPanel::OnInit()
{
}

void AnimationEditorPanel::OnUpdate(float deltaTime)
{
    if (m_isPlaying)
    {
        m_currentTime += deltaTime * m_playbackSpeed;

        if (m_currentTime >= m_duration)
        {
            if (m_isLooping)
            {
                m_currentTime = std::fmod(m_currentTime, m_duration);
            }
            else
            {
                m_currentTime = m_duration;
                m_isPlaying = false;
            }
        }
    }
}

void AnimationEditorPanel::OnGUI()
{
    if (!ImGui::Begin(GetName()))
    {
        ImGui::End();
        return;
    }

    DrawToolbar();
    ImGui::Separator();

    // Main content area
    float panelWidth = ImGui::GetContentRegionAvail().x;
    
    if (m_showBoneHierarchy)
    {
        ImGui::Columns(3, "AnimEditorColumns", true);
        ImGui::SetColumnWidth(0, 200);
        ImGui::SetColumnWidth(1, panelWidth - 400);

        // Left: Bone hierarchy
        ImGui::BeginChild("BoneHierarchy", ImVec2(0, 0), true);
        DrawBoneHierarchy();
        ImGui::EndChild();

        ImGui::NextColumn();

        // Center: Timeline
        ImGui::BeginChild("Timeline", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        DrawTimeline();
        ImGui::EndChild();

        ImGui::NextColumn();

        // Right: Property panel
        if (m_showPropertyPanel)
        {
            ImGui::BeginChild("PropertyPanel", ImVec2(0, 0), true);
            DrawPropertyPanel();
            ImGui::EndChild();
        }

        ImGui::Columns(1);
    }
    else
    {
        DrawTimeline();
    }

    // Curve editor (floating window)
    if (m_showCurveEditor)
    {
        DrawCurveEditor();
    }

    ImGui::End();
}

void AnimationEditorPanel::DrawToolbar()
{
    // Playback controls
    if (ImGui::Button(m_isPlaying ? "||" : ">"))
    {
        if (m_isPlaying)
            Pause();
        else
            Play();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(m_isPlaying ? "Pause" : "Play");

    ImGui::SameLine();
    if (ImGui::Button("[]"))
    {
        Stop();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stop");

    ImGui::SameLine();
    if (ImGui::Button("|<"))
    {
        m_currentTime = 0.0f;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Go to Start");

    ImGui::SameLine();
    if (ImGui::Button(">|"))
    {
        m_currentTime = m_duration;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Go to End");

    ImGui::SameLine();
    ImGui::Checkbox("Loop", &m_isLooping);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Time display
    ImGui::SetNextItemWidth(80);
    if (ImGui::DragFloat("##Time", &m_currentTime, 0.01f, 0.0f, m_duration, "%.2f s"))
    {
        m_currentTime = std::clamp(m_currentTime, 0.0f, m_duration);
    }

    ImGui::SameLine();
    ImGui::Text("/ %.2f s", m_duration);

    ImGui::SameLine();

    // Frame display
    int currentFrame = static_cast<int>(m_currentTime * m_frameRate);
    int totalFrames = static_cast<int>(m_duration * m_frameRate);
    ImGui::Text("Frame: %d / %d", currentFrame, totalFrames);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Playback speed
    ImGui::SetNextItemWidth(80);
    ImGui::DragFloat("Speed", &m_playbackSpeed, 0.1f, 0.1f, 4.0f, "%.1fx");

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // View options
    ImGui::Checkbox("Bones", &m_showBoneHierarchy);
    ImGui::SameLine();
    ImGui::Checkbox("Properties", &m_showPropertyPanel);
    ImGui::SameLine();
    ImGui::Checkbox("Curves", &m_showCurveEditor);
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &m_snapToFrames);

    ImGui::SameLine();

    // Zoom
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("Zoom", &m_timelineZoom, 20.0f, 500.0f, "%.0f");
}

void AnimationEditorPanel::DrawTimeline()
{
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background
    drawList->AddRectFilled(canvasPos, 
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(30, 30, 35, 255));

    // Draw time ruler
    DrawTimeRuler();

    // Draw tracks
    DrawTracks();

    // Draw playhead
    float playheadX = canvasPos.x + TimeToPixel(m_currentTime) - m_timelineScroll;
    if (playheadX >= canvasPos.x && playheadX <= canvasPos.x + canvasSize.x)
    {
        drawList->AddLine(ImVec2(playheadX, canvasPos.y),
                          ImVec2(playheadX, canvasPos.y + canvasSize.y),
                          IM_COL32(255, 100, 100, 255), 2.0f);

        // Playhead head
        drawList->AddTriangleFilled(
            ImVec2(playheadX - 6, canvasPos.y + m_rulerHeight - 10),
            ImVec2(playheadX + 6, canvasPos.y + m_rulerHeight - 10),
            ImVec2(playheadX, canvasPos.y + m_rulerHeight),
            IM_COL32(255, 100, 100, 255));
    }

    HandleTimelineInput();
}

void AnimationEditorPanel::DrawTimeRuler()
{
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Ruler background
    drawList->AddRectFilled(canvasPos,
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + m_rulerHeight),
                            IM_COL32(40, 40, 45, 255));

    // Calculate tick spacing
    float minTickSpacing = 50.0f;
    float timePerTick = minTickSpacing / m_timelineZoom;
    
    // Round to nice values
    float niceIntervals[] = { 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f };
    for (float interval : niceIntervals)
    {
        if (interval >= timePerTick)
        {
            timePerTick = interval;
            break;
        }
    }

    // Draw ticks
    float startTime = std::floor(m_timelineScroll / m_timelineZoom / timePerTick) * timePerTick;
    for (float time = startTime; time <= m_duration + timePerTick; time += timePerTick)
    {
        float x = canvasPos.x + TimeToPixel(time) - m_timelineScroll;
        if (x < canvasPos.x || x > canvasPos.x + canvasSize.x)
            continue;

        bool isMajor = std::fmod(time, timePerTick * 5) < 0.001f;

        // Tick line
        float tickHeight = isMajor ? m_rulerHeight * 0.6f : m_rulerHeight * 0.3f;
        drawList->AddLine(ImVec2(x, canvasPos.y + m_rulerHeight - tickHeight),
                          ImVec2(x, canvasPos.y + m_rulerHeight),
                          IM_COL32(100, 100, 100, 255));

        // Time label (major ticks only)
        if (isMajor)
        {
            char label[16];
            if (time >= 1.0f)
                snprintf(label, sizeof(label), "%.1fs", time);
            else
                snprintf(label, sizeof(label), "%.2fs", time);
            
            drawList->AddText(ImVec2(x + 2, canvasPos.y + 2), 
                             IM_COL32(180, 180, 180, 255), label);
        }
    }

    // Separator line
    drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + m_rulerHeight),
                      ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + m_rulerHeight),
                      IM_COL32(60, 60, 60, 255));
}

void AnimationEditorPanel::DrawTracks()
{
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    float trackY = canvasPos.y + m_rulerHeight;

    // Demo tracks
    const char* trackNames[] = { "Position.X", "Position.Y", "Position.Z", 
                                  "Rotation.X", "Rotation.Y", "Rotation.Z",
                                  "Scale.X", "Scale.Y", "Scale.Z" };

    for (int i = 0; i < 9; ++i)
    {
        // Track background
        ImU32 bgColor = (i == m_selectedTrack) ? IM_COL32(50, 70, 100, 255) :
                        (i % 2 == 0) ? IM_COL32(35, 35, 40, 255) : IM_COL32(40, 40, 45, 255);

        drawList->AddRectFilled(ImVec2(canvasPos.x, trackY),
                                ImVec2(canvasPos.x + canvasSize.x, trackY + m_trackHeight),
                                bgColor);

        // Track name
        drawList->AddText(ImVec2(canvasPos.x + 4, trackY + 4),
                         IM_COL32(180, 180, 180, 255), trackNames[i]);

        // Draw keyframes (demo)
        float keyframeTimes[] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f };
        for (float keyTime : keyframeTimes)
        {
            if (keyTime <= m_duration)
            {
                float kx = canvasPos.x + TimeToPixel(keyTime) - m_timelineScroll;
                if (kx >= canvasPos.x && kx <= canvasPos.x + canvasSize.x)
                {
                    float ky = trackY + m_trackHeight * 0.5f;
                    bool selected = (m_selectedTrack == i && m_selectedKeyframe >= 0);
                    ImU32 keyColor = selected ? IM_COL32(255, 200, 100, 255) : 
                                                IM_COL32(100, 180, 255, 255);

                    // Diamond shape
                    float size = 5.0f;
                    drawList->AddQuadFilled(ImVec2(kx, ky - size),
                                           ImVec2(kx + size, ky),
                                           ImVec2(kx, ky + size),
                                           ImVec2(kx - size, ky),
                                           keyColor);
                }
            }
        }

        trackY += m_trackHeight;
    }

    // Handle track selection
    if (ImGui::IsWindowHovered())
    {
        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.y > canvasPos.y + m_rulerHeight)
        {
            int hoveredTrack = static_cast<int>((mousePos.y - canvasPos.y - m_rulerHeight) / m_trackHeight);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredTrack >= 0 && hoveredTrack < 9)
            {
                m_selectedTrack = hoveredTrack;
            }
        }
    }
}

void AnimationEditorPanel::DrawTrack(int trackIndex)
{
    (void)trackIndex;
    // Individual track rendering handled in DrawTracks
}

void AnimationEditorPanel::DrawKeyframes(int trackIndex)
{
    (void)trackIndex;
    // Keyframe rendering handled in DrawTracks
}

void AnimationEditorPanel::DrawBoneHierarchy()
{
    ImGui::Text("Bone Hierarchy");
    ImGui::Separator();

    // Demo bone hierarchy
    const char* boneNames[] = { "Root", "Spine", "Spine1", "Spine2", "Neck", "Head",
                                 "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
                                 "RightShoulder", "RightArm", "RightForeArm", "RightHand",
                                 "LeftUpLeg", "LeftLeg", "LeftFoot",
                                 "RightUpLeg", "RightLeg", "RightFoot" };

    for (int i = 0; i < 20; ++i)
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (m_selectedBone == i)
            flags |= ImGuiTreeNodeFlags_Selected;

        // Simulate hierarchy depth
        int depth = (i == 0) ? 0 : (i < 6) ? 1 : (i < 14) ? 2 : 2;
        bool hasChildren = (i == 0 || i == 1 || i == 2 || i == 6 || i == 10 || i == 14 || i == 17);
        if (!hasChildren)
            flags |= ImGuiTreeNodeFlags_Leaf;

        for (int d = 0; d < depth; ++d)
            ImGui::Indent();

        bool open = ImGui::TreeNodeEx(boneNames[i], flags);

        if (ImGui::IsItemClicked())
        {
            m_selectedBone = i;
        }

        if (open)
        {
            ImGui::TreePop();
        }

        for (int d = 0; d < depth; ++d)
            ImGui::Unindent();
    }
}

void AnimationEditorPanel::DrawBoneNode(int boneIndex, int depth)
{
    (void)boneIndex;
    (void)depth;
    // Handled in DrawBoneHierarchy
}

void AnimationEditorPanel::DrawCurveEditor()
{
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Curve Editor", &m_showCurveEditor))
    {
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(canvasPos,
                                ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                IM_COL32(30, 30, 35, 255));

        // Grid
        ImU32 gridColor = IM_COL32(50, 50, 55, 255);
        for (float x = 0; x < canvasSize.x; x += 50)
        {
            drawList->AddLine(ImVec2(canvasPos.x + x, canvasPos.y),
                             ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y),
                             gridColor);
        }
        for (float y = 0; y < canvasSize.y; y += 50)
        {
            drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + y),
                             ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y),
                             gridColor);
        }

        // Demo curve
        ImU32 curveColor = IM_COL32(100, 180, 255, 255);
        for (int i = 0; i < static_cast<int>(canvasSize.x) - 1; ++i)
        {
            float t1 = static_cast<float>(i) / canvasSize.x;
            float t2 = static_cast<float>(i + 1) / canvasSize.x;
            
            // Simple sine wave for demo
            float v1 = 0.5f + 0.4f * std::sin(t1 * 6.28f);
            float v2 = 0.5f + 0.4f * std::sin(t2 * 6.28f);

            float y1 = canvasPos.y + canvasSize.y * (1.0f - v1);
            float y2 = canvasPos.y + canvasSize.y * (1.0f - v2);

            drawList->AddLine(ImVec2(canvasPos.x + i, y1),
                             ImVec2(canvasPos.x + i + 1, y2),
                             curveColor, 2.0f);
        }

        ImGui::InvisibleButton("CurveCanvas", canvasSize);
    }
    ImGui::End();
}

void AnimationEditorPanel::DrawPropertyPanel()
{
    ImGui::Text("Properties");
    ImGui::Separator();

    if (m_selectedBone >= 0)
    {
        ImGui::Text("Selected: Bone %d", m_selectedBone);
        ImGui::Separator();

        Vec3 position(0.0f);
        Vec3 rotation(0.0f);
        Vec3 scale(1.0f);

        ImGui::DragFloat3("Position", &position.x, 0.01f);
        ImGui::DragFloat3("Rotation", &rotation.x, 0.5f);
        ImGui::DragFloat3("Scale", &scale.x, 0.01f);

        ImGui::Separator();

        if (ImGui::Button("Add Keyframe", ImVec2(-1, 0)))
        {
            // Add keyframe at current time
        }
    }
    else if (m_selectedTrack >= 0)
    {
        ImGui::Text("Selected: Track %d", m_selectedTrack);
        ImGui::Separator();

        if (m_selectedKeyframe >= 0)
        {
            float keyTime = 0.0f;
            float keyValue = 0.0f;

            ImGui::DragFloat("Time", &keyTime, 0.01f, 0.0f, m_duration);
            ImGui::DragFloat("Value", &keyValue, 0.01f);

            ImGui::Separator();

            const char* tangentModes[] = { "Auto", "Linear", "Constant", "Free" };
            static int tangentMode = 0;
            ImGui::Combo("Tangent", &tangentMode, tangentModes, 4);

            ImGui::Separator();

            if (ImGui::Button("Delete Keyframe", ImVec2(-1, 0)))
            {
                // Delete selected keyframe
            }
        }
        else
        {
            ImGui::TextDisabled("Select a keyframe");
        }
    }
    else
    {
        ImGui::TextDisabled("Select a bone or track");
    }
}

void AnimationEditorPanel::HandleTimelineInput()
{
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    if (ImGui::IsWindowHovered())
    {
        // Scroll with mouse wheel
        if (ImGui::GetIO().MouseWheel != 0.0f)
        {
            if (ImGui::GetIO().KeyCtrl)
            {
                // Zoom
                float zoomDelta = ImGui::GetIO().MouseWheel * m_timelineZoom * 0.1f;
                m_timelineZoom = std::clamp(m_timelineZoom + zoomDelta, 20.0f, 500.0f);
            }
            else
            {
                // Scroll
                m_timelineScroll -= ImGui::GetIO().MouseWheel * 50.0f;
                m_timelineScroll = std::max(0.0f, m_timelineScroll);
            }
        }

        // Click on ruler to set time
        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.y >= canvasPos.y && mousePos.y <= canvasPos.y + m_rulerHeight)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                float clickX = mousePos.x - canvasPos.x + m_timelineScroll;
                m_currentTime = PixelToTime(clickX);
                m_currentTime = std::clamp(m_currentTime, 0.0f, m_duration);

                if (m_snapToFrames)
                {
                    m_currentTime = std::round(m_currentTime * m_frameRate) / m_frameRate;
                }
            }
        }
    }
}

void AnimationEditorPanel::HandleKeyframeEditing()
{
    // TODO: Implement keyframe drag, copy, paste, delete
}

float AnimationEditorPanel::TimeToPixel(float time) const
{
    return time * m_timelineZoom;
}

float AnimationEditorPanel::PixelToTime(float pixel) const
{
    return pixel / m_timelineZoom;
}

void AnimationEditorPanel::SetAnimationClip(std::shared_ptr<AnimationClip> clip)
{
    m_clip = clip;
    if (clip)
    {
        // m_duration = clip->GetDuration();
        m_currentTime = 0.0f;
    }
}

void AnimationEditorPanel::SetSkeleton(std::shared_ptr<Skeleton> skeleton)
{
    m_skeleton = skeleton;
    // Reset bone state
    m_boneExpanded.clear();
    m_boneVisible.clear();
    m_selectedBone = -1;
}

void AnimationEditorPanel::Play()
{
    m_isPlaying = true;
}

void AnimationEditorPanel::Pause()
{
    m_isPlaying = false;
}

void AnimationEditorPanel::Stop()
{
    m_isPlaying = false;
    m_currentTime = 0.0f;
}

void AnimationEditorPanel::SetTime(float time)
{
    m_currentTime = std::clamp(time, 0.0f, m_duration);
}

} // namespace RVX::Editor
