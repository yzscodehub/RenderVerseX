/**
 * @file EditorViewportSelectionOverlayModel.h
 * @brief Native viewport selected-entity visual overlay model
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorViewportProjection.h"
#include "UI/UITypes.h"

#include <array>
#include <string>

namespace RVX
{
    class SceneEntity;
}

namespace RVX::Editor
{

class EditorContext;

inline constexpr uint32 RVX_EDITOR_SELECTION_OVERLAY_CORNER_COUNT = 8;
inline constexpr uint32 RVX_EDITOR_SELECTION_OVERLAY_EDGE_COUNT = 12;
inline constexpr uint32 RVX_EDITOR_SELECTION_OVERLAY_EDGE_SAMPLE_COUNT =
    RVX_EDITOR_SELECTION_OVERLAY_EDGE_COUNT * 3;

struct EditorViewportSelectionOverlayBuildDesc
{
    UI::Rect viewportBounds;
    EditorViewportCameraFrame cameraFrame;
    float screenPadding = 6.0f;
};

struct EditorViewportSelectionOverlayInfo
{
    bool enabled = false;
    bool usesWorldBounds = false;
    bool usesFallbackPoint = false;
    SceneEntity* entity = nullptr;
    std::string entityName;
    UI::Rect bounds;
    Vec2 anchor{0.0f};
    std::array<Vec2, RVX_EDITOR_SELECTION_OVERLAY_CORNER_COUNT> screenCorners{};
    uint32 screenCornerCount = 0;
    std::array<Vec2, RVX_EDITOR_SELECTION_OVERLAY_EDGE_SAMPLE_COUNT> edgeSamples{};
    uint32 edgeSampleCount = 0;
};

struct EditorViewportSelectionOverlayStats
{
    bool built = false;
    bool hasSelection = false;
    bool cameraValid = false;
    bool hasValidWorldBounds = false;
    bool projectedBounds = false;
    bool visible = false;
    bool usedFallbackPoint = false;
    SceneEntity* selectedEntity = nullptr;
    UI::Rect screenBounds;
    Vec2 anchor{0.0f};
    uint32 projectedCornerCount = 0;
    uint32 edgeSampleCount = 0;
};

class EditorViewportSelectionOverlayModel
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    void Build(EditorContext& context,
               const EditorViewportSelectionOverlayBuildDesc& desc);

    const EditorViewportSelectionOverlayInfo& GetOverlay() const { return m_overlay; }
    const EditorViewportSelectionOverlayStats& GetLastStats() const
    {
        return m_lastStats;
    }

private:
    EditorViewportSelectionOverlayInfo m_overlay;
    EditorViewportSelectionOverlayStats m_lastStats;
};

} // namespace RVX::Editor
