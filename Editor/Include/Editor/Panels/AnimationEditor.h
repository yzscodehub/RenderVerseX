/**
 * @file AnimationEditor.h
 * @brief Animation timeline and editing panel
 */

#pragma once

#include "Editor/Panels/IEditorPanel.h"
#include "Core/MathTypes.h"
#include <string>
#include <vector>
#include <memory>

namespace RVX
{
    class AnimationClip;
    class Skeleton;
}

namespace RVX::Editor
{

/**
 * @brief Animation editor panel with timeline and keyframe editing
 */
class AnimationEditorPanel : public IEditorPanel
{
public:
    AnimationEditorPanel();
    ~AnimationEditorPanel() override = default;

    // =========================================================================
    // IEditorPanel Interface
    // =========================================================================

    const char* GetName() const override { return "Animation"; }
    const char* GetIcon() const override { return "animation"; }
    void OnInit() override;
    void OnUpdate(float deltaTime) override;
    void OnGUI() override;

    // =========================================================================
    // Animation Control
    // =========================================================================

    void SetAnimationClip(std::shared_ptr<AnimationClip> clip);
    std::shared_ptr<AnimationClip> GetAnimationClip() const { return m_clip; }

    void SetSkeleton(std::shared_ptr<Skeleton> skeleton);
    std::shared_ptr<Skeleton> GetSkeleton() const { return m_skeleton; }

    void Play();
    void Pause();
    void Stop();
    void SetTime(float time);
    float GetTime() const { return m_currentTime; }

    bool IsPlaying() const { return m_isPlaying; }
    bool IsLooping() const { return m_isLooping; }
    void SetLooping(bool loop) { m_isLooping = loop; }

private:
    void DrawToolbar();
    void DrawTimeline();
    void DrawTimeRuler();
    void DrawTracks();
    void DrawTrack(int trackIndex);
    void DrawKeyframes(int trackIndex);
    void DrawBoneHierarchy();
    void DrawBoneNode(int boneIndex, int depth = 0);
    void DrawCurveEditor();
    void DrawPropertyPanel();

    void HandleTimelineInput();
    void HandleKeyframeEditing();

    float TimeToPixel(float time) const;
    float PixelToTime(float pixel) const;

    // Animation data
    std::shared_ptr<AnimationClip> m_clip;
    std::shared_ptr<Skeleton> m_skeleton;

    // Playback state
    float m_currentTime = 0.0f;
    float m_duration = 5.0f;
    float m_playbackSpeed = 1.0f;
    bool m_isPlaying = false;
    bool m_isLooping = true;

    // Timeline view
    float m_timelineZoom = 100.0f;      // Pixels per second
    float m_timelineScroll = 0.0f;      // Scroll offset in pixels
    float m_timelineHeight = 300.0f;
    float m_trackHeight = 24.0f;
    float m_rulerHeight = 30.0f;

    // Selection
    int m_selectedTrack = -1;
    int m_selectedKeyframe = -1;
    int m_selectedBone = -1;
    bool m_isDraggingKeyframe = false;
    bool m_isDraggingTime = false;

    // UI state
    bool m_showCurveEditor = false;
    bool m_showBoneHierarchy = true;
    bool m_showPropertyPanel = true;
    bool m_snapToFrames = true;
    float m_frameRate = 30.0f;

    // Bone hierarchy
    std::vector<bool> m_boneExpanded;
    std::vector<bool> m_boneVisible;
};

} // namespace RVX::Editor
