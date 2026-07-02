/**
 * @file EditorFormLayout.cpp
 * @brief Shared native editor form layout helpers.
 */

#include "Editor/UI/EditorFormLayout.h"

#include "Editor/UI/EditorShellMetrics.h"
#include <algorithm>
#include <utility>

namespace RVX::Editor
{

EditorFormLayoutFrame EditorFormLayout::BeginScrollableFrame(
    EditorFormLayoutDesc desc)
{
    EditorFormLayoutFrame frame;
    if (!desc.context || !desc.context->ui || !desc.context->contentContainer)
    {
        return frame;
    }

    const std::string prefix =
        desc.namePrefix.empty() ? "EditorForm" : desc.namePrefix;

    const UI::UITheme& theme = desc.context->ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float viewportWidth =
        std::max(0.0f, desc.context->contentContainer->GetWidth());
    const float viewportHeight =
        std::max(0.0f, desc.context->contentContainer->GetHeight());
    const float padding = shellMetrics.contentPadding;

    frame.viewport = UI::ScrollView::Create();
    frame.viewport->SetName(prefix + ".Viewport");
    frame.viewport->SetPosition(0.0f, 0.0f);
    frame.viewport->SetSize(viewportWidth, viewportHeight);
    frame.viewport->SetBackgroundColor(UI::UIColor::Transparent());
    frame.viewport->SetBorderWidth(0.0f);
    frame.viewport->SetScrollbarNamePrefix(prefix + ".Scrollbar");

    const float contentViewportWidth =
        std::max(0.0f, viewportWidth - frame.viewport->GetScrollbarWidth());

    frame.metrics.padding = padding;
    frame.metrics.spacing = shellMetrics.formSpacing;
    frame.metrics.lineHeight = shellMetrics.formLineHeight;
    frame.metrics.rowHeight = shellMetrics.formRowHeight;
    frame.metrics.contentWidth =
        std::max(0.0f, contentViewportWidth - padding * 2.0f);
    frame.metrics.viewportHeight = viewportHeight;
    frame.parentContainer = desc.context->contentContainer;
    frame.requestedScrollOffsetY = desc.scrollOffsetY;

    frame.viewport->SetWheelStep(frame.metrics.rowHeight * 3.0f);
    if (desc.onScrollChanged)
    {
        frame.viewport->SetOnScrollChanged(std::move(desc.onScrollChanged));
    }

    frame.contentPanel = frame.viewport->GetContentPanel();
    if (!frame.contentPanel)
    {
        frame.viewport.reset();
        return frame;
    }

    frame.contentPanel->SetName(prefix + ".Content");
    frame.contentPanel->SetBackgroundColor(UI::UIColor::Transparent());
    frame.contentPanel->SetBorderWidth(0.0f);
    frame.contentPanel->SetClipChildren(false);
    frame.contentPanel->SetSize(contentViewportWidth, viewportHeight);

    frame.contentContext = *desc.context;
    frame.contentContext.contentContainer = frame.contentPanel.get();
    frame.contentContext.contentBounds =
        UI::Rect(desc.context->contentBounds.x,
                 desc.context->contentBounds.y,
                 contentViewportWidth,
                 viewportHeight);

    frame.viewport->SetScrollOffset(0.0f, desc.scrollOffsetY);
    return frame;
}

void EditorFormLayout::EndScrollableFrame(EditorFormLayoutFrame& frame,
                                          float contentHeight)
{
    if (!frame)
    {
        return;
    }

    const float viewportHeight = frame.viewport->GetHeight();
    const float contentWidth = frame.contentPanel->GetWidth();
    const float finalContentHeight =
        std::max(viewportHeight, contentHeight);
    frame.contentPanel->SetSize(contentWidth, finalContentHeight);
    frame.viewport->SetContentSize(contentWidth, finalContentHeight);
    frame.viewport->SetScrollOffset(0.0f, frame.requestedScrollOffsetY);

    if (frame.parentContainer && !frame.viewport->GetParent())
    {
        frame.parentContainer->AddChild(frame.viewport);
    }
}

} // namespace RVX::Editor
