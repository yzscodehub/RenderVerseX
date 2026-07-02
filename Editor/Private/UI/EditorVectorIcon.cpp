/**
 * @file EditorVectorIcon.cpp
 * @brief Semantic vector icon drawing for native editor controls
 */

#include "Editor/UI/EditorVectorIcon.h"

#include "UI/UIRenderer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace RVX::Editor
{
namespace
{
    bool IsScaleIconName(const std::string& name)
    {
        return name == "scale" || name == "maximize";
    }

    void DrawHLine(UI::UIRenderer& renderer,
                   EditorVectorIconDrawStats& stats,
                   float x,
                   float y,
                   float width,
                   float thickness,
                   const UI::UIColor& color)
    {
        if (width > 0.0f && thickness > 0.0f)
        {
            renderer.DrawRect(UI::Rect(x, y, width, thickness), color);
            ++stats.rectCount;
        }
    }

    void DrawVLine(UI::UIRenderer& renderer,
                   EditorVectorIconDrawStats& stats,
                   float x,
                   float y,
                   float height,
                   float thickness,
                   const UI::UIColor& color)
    {
        if (height > 0.0f && thickness > 0.0f)
        {
            renderer.DrawRect(UI::Rect(x, y, thickness, height), color);
            ++stats.rectCount;
        }
    }

    void DrawLine(UI::UIRenderer& renderer,
                  EditorVectorIconDrawStats& stats,
                  const Vec2& start,
                  const Vec2& end,
                  float thickness,
                  const UI::UIColor& color)
    {
        renderer.DrawLine(start, end, thickness, color);
        ++stats.lineCount;
    }

    void DrawBorder(UI::UIRenderer& renderer,
                    EditorVectorIconDrawStats& stats,
                    const UI::Rect& rect,
                    const UI::UIColor& color,
                    float thickness)
    {
        renderer.DrawBorder(rect, color, thickness);
        ++stats.borderCount;
    }

    void DrawPlus(UI::UIRenderer& renderer,
                  EditorVectorIconDrawStats& stats,
                  const UI::Rect& bounds,
                  const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float cx = bounds.x + bounds.width * 0.5f;
        const float cy = bounds.y + bounds.height * 0.5f;
        DrawHLine(renderer,
                  stats,
                  bounds.x + bounds.width * 0.18f,
                  cy - thickness * 0.5f,
                  bounds.width * 0.64f,
                  thickness,
                  color);
        DrawVLine(renderer,
                  stats,
                  cx - thickness * 0.5f,
                  bounds.y + bounds.height * 0.18f,
                  bounds.height * 0.64f,
                  thickness,
                  color);
    }

    void DrawFilePlusIcon(UI::UIRenderer& renderer,
                          EditorVectorIconDrawStats& stats,
                          const UI::Rect& bounds,
                          const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const UI::Rect file(bounds.x + bounds.width * 0.18f,
                            bounds.y + bounds.height * 0.08f,
                            bounds.width * 0.58f,
                            bounds.height * 0.76f);
        DrawBorder(renderer, stats, file, color, thickness);
        DrawHLine(renderer,
                  stats,
                  file.x + file.width * 0.56f,
                  file.y + file.height * 0.20f,
                  file.width * 0.26f,
                  thickness,
                  color);
        DrawVLine(renderer,
                  stats,
                  file.x + file.width * 0.56f,
                  file.y,
                  file.height * 0.22f,
                  thickness,
                  color);

        const UI::Rect plus(bounds.x + bounds.width * 0.50f,
                            bounds.y + bounds.height * 0.50f,
                            bounds.width * 0.38f,
                            bounds.height * 0.38f);
        DrawPlus(renderer, stats, plus, color);
    }

    void DrawFolderOpenIcon(UI::UIRenderer& renderer,
                            EditorVectorIconDrawStats& stats,
                            const UI::Rect& bounds,
                            const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float left = bounds.x + bounds.width * 0.12f;
        const float top = bounds.y + bounds.height * 0.24f;
        const float right = bounds.x + bounds.width * 0.88f;
        const float bottom = bounds.y + bounds.height * 0.78f;
        const float tabRight = bounds.x + bounds.width * 0.42f;
        const float lidY = bounds.y + bounds.height * 0.34f;

        DrawLine(renderer, stats, Vec2(left, lidY), Vec2(tabRight, lidY), thickness, color);
        DrawLine(renderer,
                 stats,
                 Vec2(left, lidY),
                 Vec2(left + bounds.width * 0.08f, top),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(left + bounds.width * 0.08f, top),
                 Vec2(tabRight - bounds.width * 0.05f, top),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(tabRight - bounds.width * 0.05f, top),
                 Vec2(tabRight, lidY),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(tabRight, lidY),
                 Vec2(right - bounds.width * 0.08f, lidY),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(right - bounds.width * 0.08f, lidY),
                 Vec2(right, bottom),
                 thickness,
                 color);
        DrawLine(renderer, stats, Vec2(right, bottom), Vec2(left, bottom), thickness, color);
        DrawLine(renderer, stats, Vec2(left, bottom), Vec2(left, lidY), thickness, color);

        DrawLine(renderer,
                 stats,
                 Vec2(left + bounds.width * 0.10f, bottom - bounds.height * 0.08f),
                 Vec2(right - bounds.width * 0.08f, bottom - bounds.height * 0.08f),
                 thickness,
                 color.WithAlpha(color.a * 0.82f));
    }

    void DrawSaveIcon(UI::UIRenderer& renderer,
                      EditorVectorIconDrawStats& stats,
                      const UI::Rect& bounds,
                      const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const UI::Rect body(bounds.x + bounds.width * 0.12f,
                            bounds.y + bounds.height * 0.12f,
                            bounds.width * 0.76f,
                            bounds.height * 0.76f);
        DrawBorder(renderer, stats, body, color, thickness);
        DrawHLine(renderer,
                  stats,
                  body.x + body.width * 0.18f,
                  body.y + body.height * 0.22f,
                  body.width * 0.50f,
                  thickness,
                  color);
        DrawVLine(renderer,
                  stats,
                  body.x + body.width * 0.74f,
                  body.y + thickness,
                  body.height * 0.26f,
                  thickness,
                  color);
        DrawBorder(renderer,
                   stats,
                   UI::Rect(body.x + body.width * 0.22f,
                            body.y + body.height * 0.58f,
                            body.width * 0.56f,
                            body.height * 0.22f),
                   color,
                   thickness);
    }

    void DrawTrashIcon(UI::UIRenderer& renderer,
                       EditorVectorIconDrawStats& stats,
                       const UI::Rect& bounds,
                       const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float lidY = bounds.y + bounds.height * 0.22f;
        const float left = bounds.x + bounds.width * 0.24f;
        const float right = bounds.x + bounds.width * 0.76f;
        DrawHLine(renderer, stats, left, lidY, right - left, thickness, color);
        DrawHLine(renderer,
                  stats,
                  bounds.x + bounds.width * 0.40f,
                  bounds.y + bounds.height * 0.13f,
                  bounds.width * 0.20f,
                  thickness,
                  color);
        const UI::Rect bin(bounds.x + bounds.width * 0.30f,
                           bounds.y + bounds.height * 0.30f,
                           bounds.width * 0.40f,
                           bounds.height * 0.52f);
        DrawBorder(renderer, stats, bin, color, thickness);
        DrawVLine(renderer,
                  stats,
                  bin.x + bin.width * 0.36f,
                  bin.y + bin.height * 0.16f,
                  bin.height * 0.62f,
                  thickness,
                  color.WithAlpha(color.a * 0.78f));
        DrawVLine(renderer,
                  stats,
                  bin.x + bin.width * 0.64f,
                  bin.y + bin.height * 0.16f,
                  bin.height * 0.62f,
                  thickness,
                  color.WithAlpha(color.a * 0.78f));
    }

    void DrawSettingsIcon(UI::UIRenderer& renderer,
                          EditorVectorIconDrawStats& stats,
                          const UI::Rect& bounds,
                          const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 center = bounds.Center();
        const float outer = std::min(bounds.width, bounds.height) * 0.34f;
        const float inner = outer * 0.46f;
        const std::array<Vec2, 8> directions{
            Vec2(1.0f, 0.0f),
            Vec2(0.707f, 0.707f),
            Vec2(0.0f, 1.0f),
            Vec2(-0.707f, 0.707f),
            Vec2(-1.0f, 0.0f),
            Vec2(-0.707f, -0.707f),
            Vec2(0.0f, -1.0f),
            Vec2(0.707f, -0.707f),
        };
        for (const Vec2& direction : directions)
        {
            DrawLine(renderer,
                     stats,
                     center + direction * (outer * 0.72f),
                     center + direction * outer,
                     thickness,
                     color);
        }
        const UI::Rect innerBox(center.x - inner,
                                center.y - inner,
                                inner * 2.0f,
                                inner * 2.0f);
        DrawBorder(renderer, stats, innerBox, color, thickness);
    }

    void DrawCommandIcon(UI::UIRenderer& renderer,
                         EditorVectorIconDrawStats& stats,
                         const UI::Rect& bounds,
                         const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float left = bounds.x + bounds.width * 0.22f;
        const float top = bounds.y + bounds.height * 0.30f;
        const float midY = bounds.y + bounds.height * 0.50f;
        const float bottom = bounds.y + bounds.height * 0.70f;
        DrawLine(renderer,
                 stats,
                 Vec2(left, top),
                 Vec2(bounds.x + bounds.width * 0.40f, midY),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(left, bottom),
                 Vec2(bounds.x + bounds.width * 0.40f, midY),
                 thickness,
                 color);
        DrawHLine(renderer,
                  stats,
                  bounds.x + bounds.width * 0.52f,
                  bottom - thickness * 0.5f,
                  bounds.width * 0.26f,
                  thickness,
                  color);
    }

    void DrawRefreshIcon(UI::UIRenderer& renderer,
                         EditorVectorIconDrawStats& stats,
                         const UI::Rect& bounds,
                         const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 center = bounds.Center();
        const float radiusX = bounds.width * 0.28f;
        const float radiusY = bounds.height * 0.26f;
        DrawLine(renderer,
                 stats,
                 Vec2(center.x - radiusX, center.y - radiusY * 0.10f),
                 Vec2(center.x - radiusX * 0.55f, center.y - radiusY),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x - radiusX * 0.55f, center.y - radiusY),
                 Vec2(center.x + radiusX * 0.55f, center.y - radiusY),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x + radiusX * 0.55f, center.y - radiusY),
                 Vec2(center.x + radiusX, center.y - radiusY * 0.10f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x + radiusX, center.y + radiusY * 0.10f),
                 Vec2(center.x + radiusX * 0.55f, center.y + radiusY),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x + radiusX * 0.55f, center.y + radiusY),
                 Vec2(center.x - radiusX * 0.55f, center.y + radiusY),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x - radiusX * 0.55f, center.y + radiusY),
                 Vec2(center.x - radiusX, center.y + radiusY * 0.10f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x + radiusX, center.y - radiusY * 0.10f),
                 Vec2(center.x + radiusX * 0.78f, center.y - radiusY * 0.38f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x + radiusX, center.y - radiusY * 0.10f),
                 Vec2(center.x + radiusX * 0.72f, center.y + radiusY * 0.02f),
                 thickness,
                 color);
    }

    void DrawPanelBottomIcon(UI::UIRenderer& renderer,
                             EditorVectorIconDrawStats& stats,
                             const UI::Rect& bounds,
                             const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const UI::Rect panel(bounds.x + bounds.width * 0.14f,
                             bounds.y + bounds.height * 0.18f,
                             bounds.width * 0.72f,
                             bounds.height * 0.64f);
        DrawBorder(renderer, stats, panel, color, thickness);
        DrawHLine(renderer,
                  stats,
                  panel.x + thickness,
                  panel.y + panel.height * 0.66f,
                  std::max(0.0f, panel.width - thickness * 2.0f),
                  thickness,
                  color);
        DrawHLine(renderer,
                  stats,
                  panel.x + panel.width * 0.25f,
                  panel.y + panel.height * 0.82f,
                  panel.width * 0.50f,
                  thickness,
                  color.WithAlpha(color.a * 0.78f));
    }

    void DrawLayoutSaveIcon(UI::UIRenderer& renderer,
                            EditorVectorIconDrawStats& stats,
                            const UI::Rect& bounds,
                            const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const UI::Rect layout(bounds.x + bounds.width * 0.12f,
                              bounds.y + bounds.height * 0.12f,
                              bounds.width * 0.76f,
                              bounds.height * 0.76f);
        DrawBorder(renderer, stats, layout, color, thickness);
        DrawVLine(renderer,
                  stats,
                  layout.x + layout.width * 0.34f,
                  layout.y + thickness,
                  layout.height - thickness * 2.0f,
                  thickness,
                  color.WithAlpha(color.a * 0.72f));
        DrawHLine(renderer,
                  stats,
                  layout.x + layout.width * 0.34f,
                  layout.y + layout.height * 0.52f,
                  layout.width * 0.64f - thickness,
                  thickness,
                  color.WithAlpha(color.a * 0.72f));
        const UI::Rect disk(layout.x + layout.width * 0.54f,
                            layout.y + layout.height * 0.58f,
                            layout.width * 0.28f,
                            layout.height * 0.24f);
        DrawBorder(renderer, stats, disk, color, thickness);
        DrawHLine(renderer,
                  stats,
                  disk.x + disk.width * 0.20f,
                  disk.y + disk.height * 0.36f,
                  disk.width * 0.46f,
                  thickness,
                  color);
    }

    void DrawCubePlusIcon(UI::UIRenderer& renderer,
                          EditorVectorIconDrawStats& stats,
                          const UI::Rect& bounds,
                          const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 top(bounds.x + bounds.width * 0.48f,
                       bounds.y + bounds.height * 0.14f);
        const Vec2 right(bounds.x + bounds.width * 0.76f,
                         bounds.y + bounds.height * 0.30f);
        const Vec2 bottom(bounds.x + bounds.width * 0.76f,
                          bounds.y + bounds.height * 0.62f);
        const Vec2 front(bounds.x + bounds.width * 0.48f,
                         bounds.y + bounds.height * 0.78f);
        const Vec2 left(bounds.x + bounds.width * 0.20f,
                        bounds.y + bounds.height * 0.62f);
        const Vec2 back(bounds.x + bounds.width * 0.20f,
                        bounds.y + bounds.height * 0.30f);
        DrawLine(renderer, stats, top, right, thickness, color);
        DrawLine(renderer, stats, right, bottom, thickness, color);
        DrawLine(renderer, stats, bottom, front, thickness, color);
        DrawLine(renderer, stats, front, left, thickness, color);
        DrawLine(renderer, stats, left, back, thickness, color);
        DrawLine(renderer, stats, back, top, thickness, color);
        DrawLine(renderer, stats, top, front, thickness, color.WithAlpha(color.a * 0.72f));
        DrawLine(renderer, stats, back, left, thickness, color.WithAlpha(color.a * 0.72f));
        DrawPlus(renderer,
                 stats,
                 UI::Rect(bounds.x + bounds.width * 0.54f,
                          bounds.y + bounds.height * 0.50f,
                          bounds.width * 0.32f,
                          bounds.height * 0.32f),
                 color);
    }

    void DrawArrowIcon(UI::UIRenderer& renderer,
                       EditorVectorIconDrawStats& stats,
                       const UI::Rect& bounds,
                       const UI::UIColor& color,
                       bool right)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float y = bounds.y + bounds.height * 0.50f;
        const float left = bounds.x + bounds.width * 0.20f;
        const float rightEdge = bounds.x + bounds.width * 0.80f;
        DrawLine(renderer, stats, Vec2(left, y), Vec2(rightEdge, y), thickness, color);

        const float headWidth = bounds.width * 0.23f;
        const float headHeight = bounds.height * 0.22f;
        if (right)
        {
            DrawLine(renderer,
                     stats,
                     Vec2(rightEdge, y),
                     Vec2(rightEdge - headWidth, y - headHeight),
                     thickness,
                     color);
            DrawLine(renderer,
                     stats,
                     Vec2(rightEdge, y),
                     Vec2(rightEdge - headWidth, y + headHeight),
                     thickness,
                     color);
            return;
        }

        DrawLine(renderer,
                 stats,
                 Vec2(left, y),
                 Vec2(left + headWidth, y - headHeight),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(left, y),
                 Vec2(left + headWidth, y + headHeight),
                 thickness,
                 color);
    }

    void DrawCheckIcon(UI::UIRenderer& renderer,
                       EditorVectorIconDrawStats& stats,
                       const UI::Rect& bounds,
                       const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 start(bounds.x + bounds.width * 0.20f,
                         bounds.y + bounds.height * 0.55f);
        const Vec2 middle(bounds.x + bounds.width * 0.42f,
                          bounds.y + bounds.height * 0.74f);
        const Vec2 end(bounds.x + bounds.width * 0.80f,
                       bounds.y + bounds.height * 0.28f);
        DrawLine(renderer, stats, start, middle, thickness, color);
        DrawLine(renderer, stats, middle, end, thickness, color);
    }

    void DrawCloseIcon(UI::UIRenderer& renderer,
                       EditorVectorIconDrawStats& stats,
                       const UI::Rect& bounds,
                       const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float inset = std::max(3.0f, std::min(bounds.width, bounds.height) * 0.24f);
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.x + inset, bounds.y + inset),
                 Vec2(bounds.Right() - inset, bounds.Bottom() - inset),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.Right() - inset, bounds.y + inset),
                 Vec2(bounds.x + inset, bounds.Bottom() - inset),
                 thickness,
                 color);
    }

    void DrawEyeIcon(UI::UIRenderer& renderer,
                     EditorVectorIconDrawStats& stats,
                     const UI::Rect& bounds,
                     const UI::UIColor& color,
                     bool off)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 left(bounds.x + bounds.width * 0.14f,
                        bounds.y + bounds.height * 0.50f);
        const Vec2 upperLeft(bounds.x + bounds.width * 0.34f,
                             bounds.y + bounds.height * 0.30f);
        const Vec2 upperRight(bounds.x + bounds.width * 0.66f,
                              bounds.y + bounds.height * 0.30f);
        const Vec2 right(bounds.x + bounds.width * 0.86f,
                         bounds.y + bounds.height * 0.50f);
        const Vec2 lowerRight(bounds.x + bounds.width * 0.66f,
                              bounds.y + bounds.height * 0.70f);
        const Vec2 lowerLeft(bounds.x + bounds.width * 0.34f,
                             bounds.y + bounds.height * 0.70f);

        DrawLine(renderer, stats, left, upperLeft, thickness, color);
        DrawLine(renderer, stats, upperLeft, upperRight, thickness, color);
        DrawLine(renderer, stats, upperRight, right, thickness, color);
        DrawLine(renderer, stats, right, lowerRight, thickness, color);
        DrawLine(renderer, stats, lowerRight, lowerLeft, thickness, color);
        DrawLine(renderer, stats, lowerLeft, left, thickness, color);

        const float pupilSize =
            std::clamp(std::min(bounds.width, bounds.height) * 0.18f,
                       2.0f,
                       5.0f);
        renderer.DrawRect(UI::Rect(bounds.x + bounds.width * 0.5f - pupilSize * 0.5f,
                                   bounds.y + bounds.height * 0.5f - pupilSize * 0.5f,
                                   pupilSize,
                                   pupilSize),
                          color);
        ++stats.rectCount;

        if (off)
        {
            DrawLine(renderer,
                     stats,
                     Vec2(bounds.x + bounds.width * 0.18f,
                          bounds.y + bounds.height * 0.82f),
                     Vec2(bounds.x + bounds.width * 0.82f,
                          bounds.y + bounds.height * 0.18f),
                     thickness,
                     color);
        }
    }

    void DrawChevronIcon(UI::UIRenderer& renderer,
                         EditorVectorIconDrawStats& stats,
                         const UI::Rect& bounds,
                         const UI::UIColor& color,
                         bool up)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float left = bounds.x + bounds.width * 0.22f;
        const float centerX = bounds.x + bounds.width * 0.50f;
        const float right = bounds.x + bounds.width * 0.78f;
        const float top = bounds.y + bounds.height * 0.36f;
        const float bottom = bounds.y + bounds.height * 0.64f;
        const Vec2 center(centerX, up ? top : bottom);
        DrawLine(renderer,
                 stats,
                 Vec2(left, up ? bottom : top),
                 center,
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 center,
                 Vec2(right, up ? bottom : top),
                 thickness,
                 color);
    }

    void DrawMoreHorizontalIcon(UI::UIRenderer& renderer,
                                EditorVectorIconDrawStats& stats,
                                const UI::Rect& bounds,
                                const UI::UIColor& color)
    {
        const float dotSize =
            std::clamp(std::min(bounds.width, bounds.height) * 0.14f,
                       2.0f,
                       4.5f);
        const float y = bounds.y + (bounds.height - dotSize) * 0.5f;
        const std::array<float, 3> centers{
            bounds.x + bounds.width * 0.32f,
            bounds.x + bounds.width * 0.50f,
            bounds.x + bounds.width * 0.68f,
        };
        for (float centerX : centers)
        {
            renderer.DrawRect(UI::Rect(centerX - dotSize * 0.5f,
                                       y,
                                       dotSize,
                                       dotSize),
                              color);
            ++stats.rectCount;
        }
    }

    void DrawMoveIcon(UI::UIRenderer& renderer,
                      EditorVectorIconDrawStats& stats,
                      const UI::Rect& bounds,
                      const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 center = bounds.Center();
        const float insetX = bounds.width * 0.18f;
        const float insetY = bounds.height * 0.18f;
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.x + insetX, center.y),
                 Vec2(bounds.Right() - insetX, center.y),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x, bounds.y + insetY),
                 Vec2(center.x, bounds.Bottom() - insetY),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.x + insetX, center.y),
                 Vec2(bounds.x + insetX + bounds.width * 0.12f,
                      center.y - bounds.height * 0.12f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.x + insetX, center.y),
                 Vec2(bounds.x + insetX + bounds.width * 0.12f,
                      center.y + bounds.height * 0.12f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.Right() - insetX, center.y),
                 Vec2(bounds.Right() - insetX - bounds.width * 0.12f,
                      center.y - bounds.height * 0.12f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.Right() - insetX, center.y),
                 Vec2(bounds.Right() - insetX - bounds.width * 0.12f,
                      center.y + bounds.height * 0.12f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x, bounds.y + insetY),
                 Vec2(center.x - bounds.width * 0.12f,
                      bounds.y + insetY + bounds.height * 0.12f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x, bounds.y + insetY),
                 Vec2(center.x + bounds.width * 0.12f,
                      bounds.y + insetY + bounds.height * 0.12f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x, bounds.Bottom() - insetY),
                 Vec2(center.x - bounds.width * 0.12f,
                      bounds.Bottom() - insetY - bounds.height * 0.12f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x, bounds.Bottom() - insetY),
                 Vec2(center.x + bounds.width * 0.12f,
                      bounds.Bottom() - insetY - bounds.height * 0.12f),
                 thickness,
                 color);
    }

    void DrawRotateIcon(UI::UIRenderer& renderer,
                        EditorVectorIconDrawStats& stats,
                        const UI::Rect& bounds,
                        const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float left = bounds.x + bounds.width * 0.22f;
        const float top = bounds.y + bounds.height * 0.22f;
        const float right = bounds.x + bounds.width * 0.78f;
        const float bottom = bounds.y + bounds.height * 0.78f;
        DrawLine(renderer, stats, Vec2(left, top), Vec2(right, top), thickness, color);
        DrawLine(renderer, stats, Vec2(right, top), Vec2(right, bottom), thickness, color);
        DrawLine(renderer, stats, Vec2(right, bottom), Vec2(left, bottom), thickness, color);
        DrawLine(renderer, stats, Vec2(left, bottom), Vec2(left, top), thickness, color);
        DrawLine(renderer,
                 stats,
                 Vec2(right, top),
                 Vec2(right - bounds.width * 0.20f, top + bounds.height * 0.02f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(right, top),
                 Vec2(right - bounds.width * 0.02f, top + bounds.height * 0.20f),
                 thickness,
                 color);
    }

    void DrawScaleIcon(UI::UIRenderer& renderer,
                       EditorVectorIconDrawStats& stats,
                       const UI::Rect& bounds,
                       const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 innerTopLeft(bounds.x + bounds.width * 0.34f,
                                bounds.y + bounds.height * 0.34f);
        const Vec2 innerBottomRight(bounds.x + bounds.width * 0.66f,
                                    bounds.y + bounds.height * 0.66f);
        DrawBorder(renderer,
                   stats,
                   UI::Rect(innerTopLeft.x,
                            innerTopLeft.y,
                            innerBottomRight.x - innerTopLeft.x,
                            innerBottomRight.y - innerTopLeft.y),
                   color,
                   thickness);

        const Vec2 outerTopLeft(bounds.x + bounds.width * 0.16f,
                                bounds.y + bounds.height * 0.16f);
        const Vec2 outerBottomRight(bounds.x + bounds.width * 0.84f,
                                    bounds.y + bounds.height * 0.84f);
        DrawLine(renderer, stats, innerTopLeft, outerTopLeft, thickness, color);
        DrawLine(renderer, stats, innerBottomRight, outerBottomRight, thickness, color);
        DrawLine(renderer,
                 stats,
                 outerTopLeft,
                 Vec2(outerTopLeft.x + bounds.width * 0.20f, outerTopLeft.y),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 outerTopLeft,
                 Vec2(outerTopLeft.x, outerTopLeft.y + bounds.height * 0.20f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 outerBottomRight,
                 Vec2(outerBottomRight.x - bounds.width * 0.20f, outerBottomRight.y),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 outerBottomRight,
                 Vec2(outerBottomRight.x, outerBottomRight.y - bounds.height * 0.20f),
                 thickness,
                 color);
    }

    void DrawGridIcon(UI::UIRenderer& renderer,
                      EditorVectorIconDrawStats& stats,
                      const UI::Rect& bounds,
                      const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const UI::Rect rect(bounds.x + bounds.width * 0.18f,
                            bounds.y + bounds.height * 0.18f,
                            bounds.width * 0.64f,
                            bounds.height * 0.64f);
        DrawBorder(renderer, stats, rect, color, thickness);
        DrawVLine(renderer,
                  stats,
                  rect.x + rect.width * 0.33f,
                  rect.y,
                  rect.height,
                  thickness,
                  color.WithAlpha(color.a * 0.78f));
        DrawVLine(renderer,
                  stats,
                  rect.x + rect.width * 0.66f,
                  rect.y,
                  rect.height,
                  thickness,
                  color.WithAlpha(color.a * 0.78f));
        DrawHLine(renderer,
                  stats,
                  rect.x,
                  rect.y + rect.height * 0.50f,
                  rect.width,
                  thickness,
                  color.WithAlpha(color.a * 0.78f));
    }

    void DrawStatsIcon(UI::UIRenderer& renderer,
                       EditorVectorIconDrawStats& stats,
                       const UI::Rect& bounds,
                       const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float bottom = bounds.y + bounds.height * 0.80f;
        const float barWidth = std::max(thickness, bounds.width * 0.13f);
        const float gap = bounds.width * 0.10f;
        const float firstX = bounds.x + bounds.width * 0.24f;
        const std::array<float, 3> heights = {
            bounds.height * 0.28f,
            bounds.height * 0.48f,
            bounds.height * 0.62f,
        };
        for (uint32 index = 0; index < static_cast<uint32>(heights.size()); ++index)
        {
            const float x = firstX + static_cast<float>(index) * (barWidth + gap);
            renderer.DrawRect(UI::Rect(x, bottom - heights[index], barWidth, heights[index]),
                              color);
            ++stats.rectCount;
        }
        DrawHLine(renderer,
                  stats,
                  bounds.x + bounds.width * 0.18f,
                  bottom + thickness,
                  bounds.width * 0.66f,
                  thickness,
                  color.WithAlpha(color.a * 0.75f));
    }

    void DrawSnapIcon(UI::UIRenderer& renderer,
                      EditorVectorIconDrawStats& stats,
                      const UI::Rect& bounds,
                      const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const float left = bounds.x + bounds.width * 0.24f;
        const float right = bounds.x + bounds.width * 0.76f;
        const float top = bounds.y + bounds.height * 0.22f;
        const float mid = bounds.y + bounds.height * 0.55f;
        const float bottom = bounds.y + bounds.height * 0.78f;
        DrawLine(renderer, stats, Vec2(left, top), Vec2(left, mid), thickness, color);
        DrawLine(renderer, stats, Vec2(right, top), Vec2(right, mid), thickness, color);
        DrawLine(renderer, stats, Vec2(left, mid), Vec2(right, mid), thickness, color);
        DrawLine(renderer,
                 stats,
                 Vec2(left, bottom),
                 Vec2(left + bounds.width * 0.14f, bottom),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(right - bounds.width * 0.14f, bottom),
                 Vec2(right, bottom),
                 thickness,
                 color);
    }

    void DrawOrbitIcon(UI::UIRenderer& renderer,
                       EditorVectorIconDrawStats& stats,
                       const UI::Rect& bounds,
                       const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 center = bounds.Center();
        const float rx = bounds.width * 0.34f;
        const float ry = bounds.height * 0.24f;
        constexpr uint32 segmentCount = 14u;
        Vec2 previous(center.x + rx, center.y);
        for (uint32 segment = 1; segment <= segmentCount; ++segment)
        {
            const float angle =
                static_cast<float>(segment) / static_cast<float>(segmentCount) *
                6.28318530718f;
            const Vec2 point(center.x + std::cos(angle) * rx,
                             center.y + std::sin(angle) * ry);
            DrawLine(renderer, stats, previous, point, thickness, color);
            previous = point;
        }
        DrawLine(renderer,
                 stats,
                 Vec2(center.x + rx, center.y),
                 Vec2(center.x + rx - bounds.width * 0.14f,
                      center.y - bounds.height * 0.12f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x + rx, center.y),
                 Vec2(center.x + rx - bounds.width * 0.14f,
                      center.y + bounds.height * 0.12f),
                 thickness,
                 color);
    }

    void DrawFlyIcon(UI::UIRenderer& renderer,
                     EditorVectorIconDrawStats& stats,
                     const UI::Rect& bounds,
                     const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 nose(bounds.x + bounds.width * 0.78f,
                        bounds.y + bounds.height * 0.50f);
        const Vec2 tail(bounds.x + bounds.width * 0.22f,
                        bounds.y + bounds.height * 0.50f);
        DrawLine(renderer, stats, tail, nose, thickness, color);
        DrawLine(renderer,
                 stats,
                 nose,
                 Vec2(bounds.x + bounds.width * 0.52f,
                      bounds.y + bounds.height * 0.26f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 nose,
                 Vec2(bounds.x + bounds.width * 0.52f,
                      bounds.y + bounds.height * 0.74f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 tail,
                 Vec2(bounds.x + bounds.width * 0.38f,
                      bounds.y + bounds.height * 0.34f),
                 thickness,
                 color.WithAlpha(color.a * 0.8f));
        DrawLine(renderer,
                 stats,
                 tail,
                 Vec2(bounds.x + bounds.width * 0.38f,
                      bounds.y + bounds.height * 0.66f),
                 thickness,
                 color.WithAlpha(color.a * 0.8f));
    }

    void DrawTopIcon(UI::UIRenderer& renderer,
                     EditorVectorIconDrawStats& stats,
                     const UI::Rect& bounds,
                     const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const UI::Rect plane(bounds.x + bounds.width * 0.24f,
                             bounds.y + bounds.height * 0.34f,
                             bounds.width * 0.52f,
                             bounds.height * 0.34f);
        DrawBorder(renderer, stats, plane, color, thickness);
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.x + bounds.width * 0.50f,
                      bounds.y + bounds.height * 0.18f),
                 Vec2(bounds.x + bounds.width * 0.50f, plane.y),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.x + bounds.width * 0.50f,
                      bounds.y + bounds.height * 0.18f),
                 Vec2(bounds.x + bounds.width * 0.40f,
                      bounds.y + bounds.height * 0.30f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.x + bounds.width * 0.50f,
                      bounds.y + bounds.height * 0.18f),
                 Vec2(bounds.x + bounds.width * 0.60f,
                      bounds.y + bounds.height * 0.30f),
                 thickness,
                 color);
    }

    void DrawSunIcon(UI::UIRenderer& renderer,
                     EditorVectorIconDrawStats& stats,
                     const UI::Rect& bounds,
                     const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 center = bounds.Center();
        const float core = std::min(bounds.width, bounds.height) * 0.20f;
        DrawBorder(renderer,
                   stats,
                   UI::Rect(center.x - core * 0.5f,
                            center.y - core * 0.5f,
                            core,
                            core),
                   color,
                   thickness);
        constexpr std::array<Vec2, 4> directions = {
            Vec2{1.0f, 0.0f},
            Vec2{0.0f, 1.0f},
            Vec2{0.707f, 0.707f},
            Vec2{0.707f, -0.707f},
        };
        for (const Vec2& direction : directions)
        {
            const Vec2 start = center + direction * (core * 0.85f);
            const Vec2 end = center + direction * (std::min(bounds.width, bounds.height) * 0.38f);
            DrawLine(renderer, stats, start, end, thickness, color);
            DrawLine(renderer, stats, center - (start - center), center - (end - center), thickness, color);
        }
    }

    void DrawWireframeIcon(UI::UIRenderer& renderer,
                           EditorVectorIconDrawStats& stats,
                           const UI::Rect& bounds,
                           const UI::UIColor& color)
    {
        DrawGridIcon(renderer, stats, bounds, color);
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.x + bounds.width * 0.18f,
                      bounds.y + bounds.height * 0.18f),
                 Vec2(bounds.x + bounds.width * 0.82f,
                      bounds.y + bounds.height * 0.82f),
                 thickness,
                 color.WithAlpha(color.a * 0.70f));
    }

    void DrawFocusIcon(UI::UIRenderer& renderer,
                       EditorVectorIconDrawStats& stats,
                       const UI::Rect& bounds,
                       const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 center = bounds.Center();
        const float inner = std::min(bounds.width, bounds.height) * 0.12f;
        const float outer = std::min(bounds.width, bounds.height) * 0.36f;
        DrawLine(renderer,
                 stats,
                 Vec2(center.x - outer, center.y),
                 Vec2(center.x - inner, center.y),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x + inner, center.y),
                 Vec2(center.x + outer, center.y),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x, center.y - outer),
                 Vec2(center.x, center.y - inner),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 Vec2(center.x, center.y + inner),
                 Vec2(center.x, center.y + outer),
                 thickness,
                 color);
        DrawBorder(renderer,
                   stats,
                   UI::Rect(center.x - inner,
                            center.y - inner,
                            inner * 2.0f,
                            inner * 2.0f),
                   color,
                   thickness);
    }

    void DrawGlobeIcon(UI::UIRenderer& renderer,
                       EditorVectorIconDrawStats& stats,
                       const UI::Rect& bounds,
                       const UI::UIColor& color)
    {
        DrawOrbitIcon(renderer, stats, bounds, color);
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 center = bounds.Center();
        DrawLine(renderer,
                 stats,
                 Vec2(center.x, bounds.y + bounds.height * 0.22f),
                 Vec2(center.x, bounds.y + bounds.height * 0.78f),
                 thickness,
                 color.WithAlpha(color.a * 0.80f));
        DrawLine(renderer,
                 stats,
                 Vec2(bounds.x + bounds.width * 0.24f, center.y),
                 Vec2(bounds.x + bounds.width * 0.76f, center.y),
                 thickness,
                 color.WithAlpha(color.a * 0.80f));
    }

    void DrawLocalAxisIcon(UI::UIRenderer& renderer,
                           EditorVectorIconDrawStats& stats,
                           const UI::Rect& bounds,
                           const UI::UIColor& color)
    {
        const float thickness =
            EditorVectorIconLibrary::GetStrokeWidth(std::min(bounds.width, bounds.height));
        const Vec2 origin(bounds.x + bounds.width * 0.32f,
                          bounds.y + bounds.height * 0.68f);
        DrawLine(renderer,
                 stats,
                 origin,
                 Vec2(bounds.x + bounds.width * 0.76f,
                      bounds.y + bounds.height * 0.68f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 origin,
                 Vec2(bounds.x + bounds.width * 0.32f,
                      bounds.y + bounds.height * 0.24f),
                 thickness,
                 color);
        DrawLine(renderer,
                 stats,
                 origin,
                 Vec2(bounds.x + bounds.width * 0.58f,
                      bounds.y + bounds.height * 0.42f),
                 thickness,
                 color);
    }

    void DrawFallbackIcon(UI::UIRenderer& renderer,
                          EditorVectorIconDrawStats& stats,
                          const UI::Rect& bounds,
                          const UI::UIColor& color,
                          const std::string& label,
                          float fontSize)
    {
        std::string glyph = "?";
        if (!label.empty())
        {
            const unsigned char first = static_cast<unsigned char>(label.front());
            glyph.assign(1, static_cast<char>(std::toupper(first)));
        }
        renderer.DrawText(glyph,
                          bounds,
                          fontSize,
                          color,
                          UI::TextAlign::Center,
                          UI::VerticalAlign::Middle);
        ++stats.textFallbackCount;
    }
} // namespace

bool EditorVectorIconLibrary::IsKnownIcon(const std::string& name)
{
    return name == "file-plus" ||
           name == "folder-open" ||
           name == "save" ||
           name == "undo" ||
           name == "redo" ||
           name == "trash" ||
           name == "settings" ||
           name == "command" ||
           name == "layout-save" ||
           name == "refresh" ||
           name == "panel-bottom" ||
           name == "cube-plus" ||
           name == "check" ||
           name == "close" ||
           name == "eye" ||
           name == "eye-off" ||
           name == "chevron-up" ||
           name == "chevron-down" ||
           name == "more-horizontal" ||
           name == "move" ||
           name == "rotate-cw" ||
           name == "snap" ||
           name == "grid" ||
           name == "stats" ||
           name == "orbit" ||
           name == "fly" ||
           name == "top" ||
           name == "sun" ||
           name == "wireframe" ||
           name == "focus" ||
           name == "reset" ||
           name == "globe" ||
           name == "local-axis" ||
           IsScaleIconName(name);
}

float EditorVectorIconLibrary::GetStrokeWidth(float iconSize)
{
    return std::clamp(iconSize * 0.075f, 1.25f, 3.0f);
}

EditorVectorIconDrawStats EditorVectorIconLibrary::Draw(
    UI::UIRenderer& renderer,
    const EditorVectorIconDrawDesc& desc)
{
    EditorVectorIconDrawStats stats;
    stats.knownIcon = IsKnownIcon(desc.name);

    if (desc.name == "file-plus")
    {
        DrawFilePlusIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "folder-open")
    {
        DrawFolderOpenIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "save")
    {
        DrawSaveIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "undo")
    {
        DrawArrowIcon(renderer, stats, desc.bounds, desc.color, false);
    }
    else if (desc.name == "redo")
    {
        DrawArrowIcon(renderer, stats, desc.bounds, desc.color, true);
    }
    else if (desc.name == "trash")
    {
        DrawTrashIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "settings")
    {
        DrawSettingsIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "command")
    {
        DrawCommandIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "layout-save")
    {
        DrawLayoutSaveIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "refresh")
    {
        DrawRefreshIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "panel-bottom")
    {
        DrawPanelBottomIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "cube-plus")
    {
        DrawCubePlusIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "check")
    {
        DrawCheckIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "close")
    {
        DrawCloseIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "eye")
    {
        DrawEyeIcon(renderer, stats, desc.bounds, desc.color, false);
    }
    else if (desc.name == "eye-off")
    {
        DrawEyeIcon(renderer, stats, desc.bounds, desc.color, true);
    }
    else if (desc.name == "chevron-up")
    {
        DrawChevronIcon(renderer, stats, desc.bounds, desc.color, true);
    }
    else if (desc.name == "chevron-down")
    {
        DrawChevronIcon(renderer, stats, desc.bounds, desc.color, false);
    }
    else if (desc.name == "more-horizontal")
    {
        DrawMoreHorizontalIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "move")
    {
        DrawMoveIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "rotate-cw")
    {
        DrawRotateIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (IsScaleIconName(desc.name))
    {
        DrawScaleIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "snap")
    {
        DrawSnapIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "grid")
    {
        DrawGridIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "stats")
    {
        DrawStatsIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "orbit")
    {
        DrawOrbitIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "fly")
    {
        DrawFlyIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "top")
    {
        DrawTopIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "sun")
    {
        DrawSunIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "wireframe")
    {
        DrawWireframeIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "focus")
    {
        DrawFocusIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "reset")
    {
        DrawRotateIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "globe")
    {
        DrawGlobeIcon(renderer, stats, desc.bounds, desc.color);
    }
    else if (desc.name == "local-axis")
    {
        DrawLocalAxisIcon(renderer, stats, desc.bounds, desc.color);
    }
    else
    {
        DrawFallbackIcon(renderer,
                         stats,
                         desc.bounds,
                         desc.color,
                         desc.fallbackLabel,
                         desc.fallbackFontSize);
    }

    return stats;
}

} // namespace RVX::Editor
