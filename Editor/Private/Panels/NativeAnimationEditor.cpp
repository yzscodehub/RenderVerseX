/**
 * @file NativeAnimationEditor.cpp
 * @brief Native UI animation editor panel implementation
 */

#include "Editor/Panels/NativeAnimationEditor.h"

#include "Editor/UI/EditorFormLayout.h"
#include "Editor/UI/EditorPanelButtonStyle.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTypography.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace RVX::Editor
{
namespace
{
    const char* AssetTypeLabel(Tools::AssetType type)
    {
        switch (type)
        {
            case Tools::AssetType::Texture:
                return "Texture";
            case Tools::AssetType::Mesh:
                return "Mesh";
            case Tools::AssetType::Material:
                return "Material";
            case Tools::AssetType::Shader:
                return "Shader";
            case Tools::AssetType::Animation:
                return "Animation";
            case Tools::AssetType::Audio:
                return "Audio";
            case Tools::AssetType::Font:
                return "Font";
            case Tools::AssetType::Prefab:
                return "Prefab";
            case Tools::AssetType::Scene:
                return "Scene";
            case Tools::AssetType::Script:
                return "Script";
            case Tools::AssetType::Unknown:
                break;
        }
        return "Unknown";
    }

    std::string FormatSeconds(float seconds)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2) << seconds << " s";
        return stream.str();
    }

    UI::Label::Ptr CreateAnimationLabel(const std::string& name,
                                        const std::string& text,
                                        const UI::UITheme& theme,
                                        const UI::UIColor& color,
                                        float fontSize = 0.0f)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(fontSize > 0.0f
                               ? fontSize
                               : EditorTypography::GetFontSize(
                                     theme,
                                     EditorTypographyRole::PropertyLabel));
        label->SetTextColor(color);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetInteractive(false);
        return label;
    }

    UI::Button::Ptr CreateAnimationButton(const std::string& name,
                                          const std::string& text,
                                          const UI::UITheme& theme,
                                          bool active = false)
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        EditorPanelButtonStyleDesc styleDesc;
        styleDesc.active = active;
        styleDesc.overflowMode = UI::TextOverflowMode::Clip;
        styleDesc.setTooltipFromText = false;
        EditorPanelButtonStyle::Apply(*button, theme, styleDesc);
        return button;
    }

    float AddFieldRow(EditorUIPanelFrameContext& context,
                      const std::string& widgetPrefix,
                      const std::string& labelText,
                      const std::string& valueText,
                      float y,
                      float width,
                      float rowHeight,
                      float padding,
                      const UI::UIColor& valueColor)
    {
        const UI::UITheme& theme = context.ui->GetTheme();
        const float labelWidth = std::min(104.0f, std::max(64.0f, width * 0.34f));
        const float valueWidth = std::max(0.0f, width - labelWidth - padding);

        UI::Label::Ptr label = CreateAnimationLabel(widgetPrefix + ".Label",
                                                    labelText,
                                                    theme,
                                                    theme.colors.textMuted);
        label->SetPosition(padding, y);
        label->SetSize(labelWidth, rowHeight);
        context.contentContainer->AddChild(label);

        UI::Label::Ptr value = CreateAnimationLabel(widgetPrefix + ".Value",
                                                    valueText,
                                                    theme,
                                                    valueColor);
        value->SetPosition(padding + labelWidth + padding, y);
        value->SetSize(valueWidth, rowHeight);
        value->SetWordWrap(true);
        context.contentContainer->AddChild(value);
        return y + rowHeight;
    }
}

NativeAnimationEditorPanel::NativeAnimationEditorPanel()
{
    m_desc.id = PanelId();
    m_desc.title = "Animation";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Bottom;
    m_desc.visibleByDefault = false;
    m_desc.closable = true;
}

void NativeAnimationEditorPanel::OnUpdate(float deltaTime)
{
    if (!m_isPlaying)
    {
        return;
    }

    m_currentTime += std::max(0.0f, deltaTime);
    if (m_currentTime >= m_duration)
    {
        if (m_isLooping && m_duration > 0.0f)
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

void NativeAnimationEditorPanel::BuildUI(EditorUIPanelFrameContext& context)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    m_lastBuildStats = {};
    m_lastBuildStats.built = true;
    PublishPlaybackStats();

    EditorContext& editorContext = EditorContext::Get();
    m_lastBuildStats.selectionType = editorContext.GetSelectionType();

    EditorFormLayoutDesc formDesc;
    formDesc.context = &context;
    formDesc.namePrefix = "NativeAnimationEditor";
    formDesc.scrollOffsetY = m_scrollOffsetY;
    formDesc.onScrollChanged = [this](const Vec2& offset) {
        m_scrollOffsetY = offset.y;
    };

    EditorFormLayoutFrame form =
        EditorFormLayout::BeginScrollableFrame(std::move(formDesc));
    if (!form)
    {
        return;
    }

    m_lastBuildStats.hasScrollViewport = true;
    m_lastBuildStats.scrollViewportHeight = form.metrics.viewportHeight;
    m_lastBuildStats.scrollOffsetY = m_scrollOffsetY;

    EditorUIPanelFrameContext contentContext = form.contentContext;

    float contentHeight = 0.0f;
    if (editorContext.GetSelectionType() == SelectionType::Asset)
    {
        const Tools::AssetGUID guid = editorContext.GetSelectedAsset();
        m_lastBuildStats.hasAssetSelection = guid.IsValid();
        m_lastBuildStats.selectedGuid = guid;

        const Tools::AssetEntry* asset = editorContext.GetAssetDatabase().GetAsset(guid);
        if (!asset)
        {
            contentHeight = AddMissingAssetState(contentContext);
        }
        else if (asset->type == Tools::AssetType::Animation)
        {
            contentHeight = AddAnimationAssetSummary(contentContext, *asset);
        }
        else
        {
            contentHeight = AddNonAnimationAssetState(contentContext, *asset);
        }
    }
    else
    {
        contentHeight = AddEmptyState(contentContext,
                                      "Select an animation asset.",
                                      "NativeAnimationEditor.Empty");
    }

    m_lastBuildStats.scrollContentHeight =
        std::max(form.metrics.viewportHeight, contentHeight);
    EditorFormLayout::EndScrollableFrame(form, contentHeight);
}

void NativeAnimationEditorPanel::Play()
{
    m_isPlaying = true;
}

void NativeAnimationEditorPanel::Pause()
{
    m_isPlaying = false;
}

void NativeAnimationEditorPanel::Stop()
{
    m_isPlaying = false;
    m_currentTime = 0.0f;
}

void NativeAnimationEditorPanel::SetTime(float time)
{
    m_currentTime = std::clamp(time, 0.0f, std::max(0.0f, m_duration));
}

void NativeAnimationEditorPanel::SetDuration(float duration)
{
    m_duration = std::max(0.01f, duration);
    m_currentTime = std::min(m_currentTime, m_duration);
}

float NativeAnimationEditorPanel::AddEmptyState(EditorUIPanelFrameContext& context,
                                                const std::string& text,
                                                const std::string& widgetName)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formLineHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);

    UI::Label::Ptr label = CreateAnimationLabel(widgetName,
                                                text,
                                                theme,
                                                theme.colors.textMuted);
    label->SetPosition(padding, padding);
    label->SetSize(width, rowHeight);
    context.contentContainer->AddChild(label);
    return padding + rowHeight + padding;
}

float NativeAnimationEditorPanel::AddMissingAssetState(EditorUIPanelFrameContext& context)
{
    m_lastBuildStats.selectedAssetMissing = true;
    return AddEmptyState(context,
                         "Selected animation asset is no longer indexed.",
                         "NativeAnimationEditor.MissingAsset");
}

float NativeAnimationEditorPanel::AddNonAnimationAssetState(EditorUIPanelFrameContext& context,
                                                            const Tools::AssetEntry& asset)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formRowHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    float y = padding;

    m_lastBuildStats.hasNonAnimationAsset = true;

    UI::Label::Ptr message =
        CreateAnimationLabel("NativeAnimationEditor.NonAnimation.Message",
                             "Selected asset is not an animation.",
                             theme,
                             theme.colors.warning);
    message->SetPosition(padding, y);
    message->SetSize(width, rowHeight);
    context.contentContainer->AddChild(message);
    y += rowHeight + padding;

    y = AddFieldRow(context,
                    "NativeAnimationEditor.NonAnimation.Type",
                    "Type",
                    AssetTypeLabel(asset.type),
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeAnimationEditor.NonAnimation.Path",
                    "Path",
                    asset.path,
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    AddActionButtons(context, y + padding, width, rowHeight, padding);
    return y + padding + rowHeight + padding;
}

float NativeAnimationEditorPanel::AddAnimationAssetSummary(EditorUIPanelFrameContext& context,
                                                           const Tools::AssetEntry& asset)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formRowHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    float y = padding;

    m_lastBuildStats.hasAnimationAsset = true;

    UI::Label::Ptr title =
        CreateAnimationLabel("NativeAnimationEditor.Header",
                             asset.name.empty() ? asset.path : asset.name,
                             theme,
                             theme.colors.text,
                             EditorTypography::GetFontSize(
                                 theme,
                                 EditorTypographyRole::Body));
    title->SetPosition(padding, y);
    title->SetSize(width, rowHeight);
    context.contentContainer->AddChild(title);
    y += rowHeight + padding;

    AddPlaybackToolbar(context, y, width, rowHeight, padding);
    y += rowHeight + padding;

    AddActionButtons(context, y, width, rowHeight, padding);
    y += rowHeight + padding;

    y = AddFieldRow(context,
                    "NativeAnimationEditor.Asset.Type",
                    "Type",
                    AssetTypeLabel(asset.type),
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeAnimationEditor.Asset.Path",
                    "Path",
                    asset.path,
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeAnimationEditor.Asset.Guid",
                    "GUID",
                    asset.guid.ToString(),
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.textMuted);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeAnimationEditor.Asset.Status",
                    "Status",
                    asset.isDirty ? "Dirty" : "Ready",
                    y,
                    width,
                    rowHeight,
                    padding,
                    asset.isDirty ? theme.colors.warning : theme.colors.textMuted);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeAnimationEditor.Playback.Time",
                    "Time",
                    FormatSeconds(m_currentTime) + " / " + FormatSeconds(m_duration),
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeAnimationEditor.Playback.Frames",
                    "Frames",
                    std::to_string(static_cast<uint32>(m_currentTime * m_frameRate)) +
                        " / " +
                        std::to_string(static_cast<uint32>(m_duration * m_frameRate)),
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddTimelinePreview(context, y + padding, width, rowHeight, padding);
    return y + padding;
}

void NativeAnimationEditorPanel::AddPlaybackToolbar(EditorUIPanelFrameContext& context,
                                                    float y,
                                                    float width,
                                                    float rowHeight,
                                                    float padding)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const float smallWidth = 62.0f;
    const float loopWidth = 86.0f;
    float x = padding;

    UI::Button::Ptr play = CreateAnimationButton("NativeAnimationEditor.Playback.PlayPause",
                                                 m_isPlaying ? "Pause" : "Play",
                                                 theme,
                                                 m_isPlaying);
    play->SetPosition(x, y);
    play->SetSize(smallWidth, rowHeight);
    play->SetOnClick([this](const UI::UIEvent& event) {
        (void)event;
        if (m_isPlaying)
        {
            Pause();
        }
        else
        {
            Play();
        }
    });
    context.contentContainer->AddChild(play);
    x += smallWidth + padding * 0.5f;

    UI::Button::Ptr stop = CreateAnimationButton("NativeAnimationEditor.Playback.Stop",
                                                 "Stop",
                                                 theme);
    stop->SetPosition(x, y);
    stop->SetSize(smallWidth, rowHeight);
    stop->SetOnClick([this](const UI::UIEvent& event) {
        (void)event;
        Stop();
    });
    context.contentContainer->AddChild(stop);
    x += smallWidth + padding * 0.5f;

    UI::Button::Ptr start = CreateAnimationButton("NativeAnimationEditor.Playback.Start",
                                                  "Start",
                                                  theme);
    start->SetPosition(x, y);
    start->SetSize(smallWidth, rowHeight);
    start->SetOnClick([this](const UI::UIEvent& event) {
        (void)event;
        SetTime(0.0f);
    });
    context.contentContainer->AddChild(start);
    x += smallWidth + padding * 0.5f;

    UI::Button::Ptr end = CreateAnimationButton("NativeAnimationEditor.Playback.End",
                                                "End",
                                                theme);
    end->SetPosition(x, y);
    end->SetSize(smallWidth, rowHeight);
    end->SetOnClick([this](const UI::UIEvent& event) {
        (void)event;
        SetTime(m_duration);
    });
    context.contentContainer->AddChild(end);
    x += smallWidth + padding * 0.5f;

    UI::Button::Ptr loop = CreateAnimationButton("NativeAnimationEditor.Playback.Loop",
                                                 m_isLooping ? "Loop On" : "Loop Off",
                                                 theme,
                                                 m_isLooping);
    loop->SetPosition(x, y);
    loop->SetSize(std::min(loopWidth, std::max(0.0f, width - (x - padding))), rowHeight);
    loop->SetOnClick([this](const UI::UIEvent& event) {
        (void)event;
        m_isLooping = !m_isLooping;
    });
    context.contentContainer->AddChild(loop);

    m_lastBuildStats.actionButtonCount += 5u;
}

float NativeAnimationEditorPanel::AddTimelinePreview(EditorUIPanelFrameContext& context,
                                                     float y,
                                                     float width,
                                                     float rowHeight,
                                                     float padding)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const float timelineHeight = std::max(48.0f, rowHeight * 2.0f);
    const float trackY = y + rowHeight;
    const float trackHeight = std::max(8.0f, rowHeight * 0.35f);

    UI::Label::Ptr header = CreateAnimationLabel("NativeAnimationEditor.Timeline.Header",
                                                 "Timeline Preview",
                                                 theme,
                                                 theme.colors.text);
    header->SetPosition(padding, y);
    header->SetSize(width, rowHeight);
    context.contentContainer->AddChild(header);

    UI::Panel::Ptr rail = UI::Panel::Create();
    rail->SetName("NativeAnimationEditor.Timeline.Rail");
    rail->SetPosition(padding, trackY + rowHeight * 0.35f);
    rail->SetSize(width, trackHeight);
    rail->SetBackgroundColor(theme.colors.surface);
    rail->SetBorderColor(theme.colors.border);
    rail->SetBorderWidth(1.0f);
    rail->SetBorderRadius(theme.metrics.cornerRadius);
    context.contentContainer->AddChild(rail);

    const uint32 tickCount = 6u;
    m_lastBuildStats.timelineTickCount = tickCount;
    for (uint32 tick = 0; tick < tickCount; ++tick)
    {
        const float t = tickCount > 1u
                            ? static_cast<float>(tick) / static_cast<float>(tickCount - 1u)
                            : 0.0f;
        UI::Panel::Ptr marker = UI::Panel::Create();
        marker->SetName("NativeAnimationEditor.Timeline.Tick." + std::to_string(tick));
        marker->SetPosition(padding + t * width, trackY + rowHeight * 0.15f);
        marker->SetSize(1.0f, rowHeight * 0.75f);
        marker->SetBackgroundColor(theme.colors.border);
        marker->SetBorderWidth(0.0f);
        context.contentContainer->AddChild(marker);
    }

    const float playheadT = m_duration > 0.0f
                                ? std::clamp(m_currentTime / m_duration, 0.0f, 1.0f)
                                : 0.0f;
    UI::Panel::Ptr playhead = UI::Panel::Create();
    playhead->SetName("NativeAnimationEditor.Timeline.Playhead");
    playhead->SetPosition(padding + playheadT * width, trackY);
    playhead->SetSize(2.0f, rowHeight);
    playhead->SetBackgroundColor(theme.colors.accent);
    playhead->SetBorderWidth(0.0f);
    context.contentContainer->AddChild(playhead);

    return y + rowHeight + timelineHeight;
}

void NativeAnimationEditorPanel::AddActionButtons(EditorUIPanelFrameContext& context,
                                                  float y,
                                                  float width,
                                                  float rowHeight,
                                                  float padding)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const float clearWidth = std::min(132.0f, std::max(70.0f, width * 0.45f));
    const float refreshWidth = std::min(132.0f,
                                        std::max(70.0f, width - clearWidth - padding));

    UI::Button::Ptr clear = CreateAnimationButton("NativeAnimationEditor.Action.ClearSelection",
                                                  "Clear Selection",
                                                  theme);
    clear->SetPosition(padding, y);
    clear->SetSize(clearWidth, rowHeight);
    clear->SetOnClick([](const UI::UIEvent& event) {
        (void)event;
        EditorContext::Get().ClearSelection();
    });
    context.contentContainer->AddChild(clear);

    UI::Button::Ptr refresh = CreateAnimationButton("NativeAnimationEditor.Action.RefreshDatabase",
                                                    "Refresh",
                                                    theme);
    refresh->SetPosition(padding + clearWidth + padding, y);
    refresh->SetSize(refreshWidth, rowHeight);
    refresh->SetOnClick([](const UI::UIEvent& event) {
        (void)event;
        EditorContext::Get().GetAssetDatabase().Refresh();
    });
    context.contentContainer->AddChild(refresh);

    m_lastBuildStats.actionButtonCount += 2u;
}

void NativeAnimationEditorPanel::PublishPlaybackStats()
{
    m_lastBuildStats.isPlaying = m_isPlaying;
    m_lastBuildStats.isLooping = m_isLooping;
    m_lastBuildStats.currentTime = m_currentTime;
    m_lastBuildStats.duration = m_duration;
}

} // namespace RVX::Editor
