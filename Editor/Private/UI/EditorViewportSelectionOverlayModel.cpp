/**
 * @file EditorViewportSelectionOverlayModel.cpp
 * @brief Native viewport selected-entity visual overlay model implementation
 */

#include "Editor/UI/EditorViewportSelectionOverlayModel.h"
#include "Editor/EditorContext.h"
#include "Scene/SceneEntity.h"

#include <algorithm>
#include <array>

namespace RVX::Editor
{
namespace
{
    UI::Rect BoundsFromPoints(
        const std::array<Vec2, RVX_EDITOR_SELECTION_OVERLAY_CORNER_COUNT>& points,
        uint32 count)
    {
        if (count == 0)
        {
            return {};
        }

        Vec2 minPoint = points[0];
        Vec2 maxPoint = points[0];
        for (uint32 index = 1; index < count; ++index)
        {
            minPoint = min(minPoint, points[index]);
            maxPoint = max(maxPoint, points[index]);
        }

        return UI::Rect(minPoint.x,
                        minPoint.y,
                        maxPoint.x - minPoint.x,
                        maxPoint.y - minPoint.y);
    }

    std::array<Vec3, RVX_EDITOR_SELECTION_OVERLAY_CORNER_COUNT> BuildWorldCorners(
        const AABB& bounds)
    {
        const Vec3 minPoint = bounds.GetMin();
        const Vec3 maxPoint = bounds.GetMax();
        return {
            Vec3(minPoint.x, minPoint.y, minPoint.z),
            Vec3(maxPoint.x, minPoint.y, minPoint.z),
            Vec3(minPoint.x, maxPoint.y, minPoint.z),
            Vec3(maxPoint.x, maxPoint.y, minPoint.z),
            Vec3(minPoint.x, minPoint.y, maxPoint.z),
            Vec3(maxPoint.x, minPoint.y, maxPoint.z),
            Vec3(minPoint.x, maxPoint.y, maxPoint.z),
            Vec3(maxPoint.x, maxPoint.y, maxPoint.z)
        };
    }

    bool BoundsTouchesViewport(const UI::Rect& bounds, const UI::Rect& viewport)
    {
        return bounds.Expand(1.0f).Overlaps(viewport);
    }

    Vec2 ClampedAnchor(const Vec2& anchor, const UI::Rect& viewport)
    {
        return Vec2(clamp(anchor.x, viewport.Left(), viewport.Right()),
                    clamp(anchor.y, viewport.Top(), viewport.Bottom()));
    }
}

void EditorViewportSelectionOverlayModel::Build(
    EditorContext& context,
    const EditorViewportSelectionOverlayBuildDesc& desc)
{
    m_overlay = {};
    m_lastStats = {};
    m_lastStats.built = true;
    m_lastStats.cameraValid = desc.cameraFrame.valid;

    SceneEntity* selectedEntity = context.GetSelectedEntity();
    m_lastStats.selectedEntity = selectedEntity;
    m_lastStats.hasSelection = selectedEntity != nullptr;
    if (!selectedEntity ||
        !desc.cameraFrame.valid ||
        desc.viewportBounds.width <= 0.0f ||
        desc.viewportBounds.height <= 0.0f)
    {
        return;
    }

    const AABB worldBounds = selectedEntity->GetWorldBounds();
    m_lastStats.hasValidWorldBounds = worldBounds.IsValid();
    if (!worldBounds.IsValid())
    {
        const EditorViewportProjectedPoint projectedCenter =
            EditorViewportProjection::ProjectWorldPoint(desc.cameraFrame,
                                                        selectedEntity->GetWorldPosition());
        if (!projectedCenter.valid || !projectedCenter.insideViewport)
        {
            return;
        }

        m_overlay.enabled = true;
        m_overlay.usesFallbackPoint = true;
        m_overlay.entity = selectedEntity;
        m_overlay.entityName = selectedEntity->GetName();
        m_overlay.anchor = projectedCenter.screenPosition;
        m_overlay.bounds = UI::Rect(projectedCenter.screenPosition.x - 8.0f,
                                    projectedCenter.screenPosition.y - 8.0f,
                                    16.0f,
                                    16.0f);
        m_lastStats.visible = true;
        m_lastStats.usedFallbackPoint = true;
        m_lastStats.screenBounds = m_overlay.bounds;
        m_lastStats.anchor = m_overlay.anchor;
        return;
    }

    const std::array<Vec3, RVX_EDITOR_SELECTION_OVERLAY_CORNER_COUNT> worldCorners =
        BuildWorldCorners(worldBounds);
    std::array<Vec2, RVX_EDITOR_SELECTION_OVERLAY_CORNER_COUNT> screenCorners{};
    uint32 projectedCornerCount = 0;
    uint32 insideCornerCount = 0;
    for (uint32 index = 0; index < worldCorners.size(); ++index)
    {
        const EditorViewportProjectedPoint projected =
            EditorViewportProjection::ProjectWorldPoint(desc.cameraFrame,
                                                        worldCorners[index]);
        if (!projected.valid)
        {
            return;
        }

        screenCorners[index] = projected.screenPosition;
        ++projectedCornerCount;
        if (projected.insideViewport)
        {
            ++insideCornerCount;
        }
    }

    UI::Rect screenBounds =
        BoundsFromPoints(screenCorners, projectedCornerCount).Expand(desc.screenPadding);
    const bool visible =
        insideCornerCount > 0 || BoundsTouchesViewport(screenBounds, desc.viewportBounds);
    m_lastStats.projectedBounds = true;
    m_lastStats.projectedCornerCount = projectedCornerCount;
    m_lastStats.screenBounds = screenBounds;
    if (!visible)
    {
        return;
    }

    static constexpr std::array<std::array<uint32, 2>, RVX_EDITOR_SELECTION_OVERLAY_EDGE_COUNT>
        edges = {{
            {{0u, 1u}},
            {{1u, 3u}},
            {{3u, 2u}},
            {{2u, 0u}},
            {{4u, 5u}},
            {{5u, 7u}},
            {{7u, 6u}},
            {{6u, 4u}},
            {{0u, 4u}},
            {{1u, 5u}},
            {{2u, 6u}},
            {{3u, 7u}}
        }};

    m_overlay.enabled = true;
    m_overlay.usesWorldBounds = true;
    m_overlay.entity = selectedEntity;
    m_overlay.entityName = selectedEntity->GetName();
    m_overlay.bounds = screenBounds;
    m_overlay.screenCorners = screenCorners;
    m_overlay.screenCornerCount = projectedCornerCount;

    uint32 sampleIndex = 0;
    for (const auto& edge : edges)
    {
        const Vec2 start = screenCorners[edge[0]];
        const Vec2 end = screenCorners[edge[1]];
        m_overlay.edgeSamples[sampleIndex++] = start;
        m_overlay.edgeSamples[sampleIndex++] = (start + end) * 0.5f;
        m_overlay.edgeSamples[sampleIndex++] = end;
    }
    m_overlay.edgeSampleCount = sampleIndex;

    const Vec3 worldCenter = worldBounds.GetCenter();
    const EditorViewportProjectedPoint projectedCenter =
        EditorViewportProjection::ProjectWorldPoint(desc.cameraFrame, worldCenter);
    m_overlay.anchor = projectedCenter.valid
                           ? ClampedAnchor(projectedCenter.screenPosition, desc.viewportBounds)
                           : screenBounds.Center();

    m_lastStats.visible = true;
    m_lastStats.anchor = m_overlay.anchor;
    m_lastStats.edgeSampleCount = m_overlay.edgeSampleCount;
}

} // namespace RVX::Editor
