/**
 * @file EditorListLayout.cpp
 * @brief Shared native editor scrollable list layout helpers.
 */

#include "Editor/UI/EditorListLayout.h"

#include <algorithm>
#include <utility>

namespace RVX::Editor
{

EditorListLayoutFrame EditorListLayout::BeginScrollableList(
    EditorListLayoutDesc desc)
{
    EditorListLayoutFrame frame;
    if (!desc.context || !desc.context->contentContainer)
    {
        return frame;
    }

    const std::string prefix =
        desc.namePrefix.empty() ? "EditorList" : desc.namePrefix;
    const float width = std::max(0.0f, desc.bounds.width);
    const float height = std::max(0.0f, desc.bounds.height);
    const float contentHeight = std::max(height, desc.contentHeight);
    const float wheelStep =
        std::max(24.0f, std::max(1.0f, desc.rowHeight) * 3.0f);

    frame.viewport = UI::ScrollView::Create();
    frame.viewport->SetName(prefix + ".Viewport");
    frame.viewport->SetPosition(desc.bounds.x, desc.bounds.y);
    frame.viewport->SetSize(width, height);
    frame.viewport->SetBackgroundColor(UI::UIColor::Transparent());
    frame.viewport->SetBorderWidth(0.0f);
    frame.viewport->SetScrollbarNamePrefix(prefix + ".Scrollbar");
    frame.viewport->SetWheelStep(wheelStep);
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
    frame.contentPanel->SetSize(width, contentHeight);
    frame.parentContainer = desc.context->contentContainer;
    frame.viewportHeight = height;
    frame.contentHeight = contentHeight;
    frame.requestedScrollOffsetY = desc.scrollOffsetY;

    frame.viewport->SetContentSize(width, contentHeight);
    frame.viewport->SetScrollOffset(0.0f, desc.scrollOffsetY);
    return frame;
}

void EditorListLayout::EndScrollableList(EditorListLayoutFrame& frame)
{
    if (!frame)
    {
        return;
    }

    frame.viewport->SetContentSize(frame.contentPanel->GetWidth(),
                                   frame.contentHeight);
    frame.viewport->SetScrollOffset(0.0f, frame.requestedScrollOffsetY);

    if (frame.parentContainer && !frame.viewport->GetParent())
    {
        frame.parentContainer->AddChild(frame.viewport);
    }
}

} // namespace RVX::Editor
