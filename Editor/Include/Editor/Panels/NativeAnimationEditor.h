/**
 * @file NativeAnimationEditor.h
 * @brief Native UI animation editor panel
 */

#pragma once

#include "Editor/EditorContext.h"
#include "Editor/UI/EditorUIPanel.h"
#include "Tools/AssetDatabase.h"

#include <string>

namespace RVX::Editor
{

struct NativeAnimationEditorStats
{
    SelectionType selectionType = SelectionType::None;
    Tools::AssetGUID selectedGuid;
    bool built = false;
    bool hasAssetSelection = false;
    bool hasAnimationAsset = false;
    bool hasNonAnimationAsset = false;
    bool selectedAssetMissing = false;
    bool hasScrollViewport = false;
    float scrollViewportHeight = 0.0f;
    float scrollContentHeight = 0.0f;
    float scrollOffsetY = 0.0f;
    bool isPlaying = false;
    bool isLooping = true;
    float currentTime = 0.0f;
    float duration = 0.0f;
    uint32 detailRowCount = 0;
    uint32 actionButtonCount = 0;
    uint32 timelineTickCount = 0;
};

class NativeAnimationEditorPanel final : public IEditorUIPanel
{
public:
    NativeAnimationEditorPanel();

    static constexpr const char* PanelId() { return "native.animationEditor"; }

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void OnUpdate(float deltaTime) override;
    void BuildUI(EditorUIPanelFrameContext& context) override;

    void Play();
    void Pause();
    void Stop();
    void SetTime(float time);
    void SetDuration(float duration);

    const NativeAnimationEditorStats& GetLastBuildStats() const { return m_lastBuildStats; }

private:
    float AddEmptyState(EditorUIPanelFrameContext& context,
                        const std::string& text,
                        const std::string& widgetName);
    float AddMissingAssetState(EditorUIPanelFrameContext& context);
    float AddNonAnimationAssetState(EditorUIPanelFrameContext& context,
                                    const Tools::AssetEntry& asset);
    float AddAnimationAssetSummary(EditorUIPanelFrameContext& context,
                                   const Tools::AssetEntry& asset);
    void AddPlaybackToolbar(EditorUIPanelFrameContext& context,
                            float y,
                            float width,
                            float rowHeight,
                            float padding);
    float AddTimelinePreview(EditorUIPanelFrameContext& context,
                             float y,
                             float width,
                             float rowHeight,
                             float padding);
    void AddActionButtons(EditorUIPanelFrameContext& context,
                          float y,
                          float width,
                          float rowHeight,
                          float padding);
    void PublishPlaybackStats();

    EditorUIPanelDesc m_desc;
    NativeAnimationEditorStats m_lastBuildStats;
    float m_scrollOffsetY = 0.0f;
    float m_currentTime = 0.0f;
    float m_duration = 5.0f;
    float m_frameRate = 30.0f;
    bool m_isPlaying = false;
    bool m_isLooping = true;
};

} // namespace RVX::Editor
