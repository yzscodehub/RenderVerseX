/**
 * @file EditorPopupLayer.h
 * @brief Native editor popup overlay layer
 */

#pragma once

#include "Core/Types.h"
#include "UI/Widgets/Panel.h"

#include <functional>
#include <vector>

namespace RVX::UI
{
    class UIContext;
    class UICanvas;
}

namespace RVX::Editor
{

struct EditorPopupLayerStats
{
    uint32 popupCount = 0;
};

/**
 * @brief Defers native popup widgets so they render and receive input above panels.
 */
class EditorPopupLayer
{
public:
    using OutsideClickCallback = std::function<void()>;

    void Begin(UI::UIContext& ui);
    void AddPopup(UI::Panel::Ptr popup);
    void End();
    void Clear(UI::UIContext& ui);

    void SetOutsideClickCallback(OutsideClickCallback callback)
    {
        m_outsideClickCallback = std::move(callback);
    }

    const EditorPopupLayerStats& GetLastBuildStats() const
    {
        return m_lastBuildStats;
    }

private:
    UI::UIContext* m_ui = nullptr;
    UI::Panel::Ptr m_root;
    std::vector<UI::Panel::Ptr> m_pendingPopups;
    OutsideClickCallback m_outsideClickCallback;
    EditorPopupLayerStats m_lastBuildStats;
};

} // namespace RVX::Editor
