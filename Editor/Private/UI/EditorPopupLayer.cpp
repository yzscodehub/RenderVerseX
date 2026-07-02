/**
 * @file EditorPopupLayer.cpp
 * @brief Native editor popup overlay layer implementation
 */

#include "Editor/UI/EditorPopupLayer.h"

#include "UI/UIContext.h"
#include "UI/UICanvas.h"

#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_POPUP_LAYER = "Editor.PopupLayer";

    class EditorPopupRoot final : public UI::Panel
    {
    public:
        using OutsideClickCallback = EditorPopupLayer::OutsideClickCallback;

        void SetOutsideClickCallback(OutsideClickCallback callback)
        {
            m_outsideClickCallback = std::move(callback);
        }

        bool HandleEvent(const UI::UIEvent& event) override
        {
            if (m_visibility != UI::Visibility::Visible)
            {
                return false;
            }

            if (event.type == UI::UIEventType::MouseDown)
            {
                if (m_outsideClickCallback)
                {
                    m_outsideClickCallback();
                }
                return true;
            }

            return event.type == UI::UIEventType::MouseUp;
        }

        UI::Widget* HitTest(const Vec2& point) override
        {
            if (m_visibility != UI::Visibility::Visible || !m_interactive ||
                !GetGlobalRect().Contains(point))
            {
                return nullptr;
            }

            for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
            {
                UI::Widget* hit = (*it)->HitTest(point);
                if (hit)
                {
                    return hit;
                }
            }

            return this;
        }

    private:
        OutsideClickCallback m_outsideClickCallback;
    };

    void RemovePopupLayer(UI::UICanvas& canvas)
    {
        UI::Widget::Ptr root = canvas.FindWidget(RVX_EDITOR_POPUP_LAYER);
        if (root)
        {
            canvas.RemoveWidget(std::move(root));
        }
    }
}

void EditorPopupLayer::Begin(UI::UIContext& ui)
{
    m_ui = &ui;
    m_pendingPopups.clear();
    m_root.reset();
    m_lastBuildStats = {};
    RemovePopupLayer(ui.GetCanvas());
}

void EditorPopupLayer::AddPopup(UI::Panel::Ptr popup)
{
    if (popup)
    {
        m_pendingPopups.push_back(std::move(popup));
    }
}

void EditorPopupLayer::End()
{
    if (!m_ui || m_pendingPopups.empty())
    {
        return;
    }

    auto root = std::make_shared<EditorPopupRoot>();
    root->SetName(RVX_EDITOR_POPUP_LAYER);
    root->SetPosition(0.0f, 0.0f);
    root->SetSize(static_cast<float>(m_ui->GetWidth()),
                  static_cast<float>(m_ui->GetHeight()));
    root->SetBackgroundColor(UI::UIColor::Transparent());
    root->SetBorderWidth(0.0f);
    root->SetOutsideClickCallback(m_outsideClickCallback);

    for (UI::Panel::Ptr& popup : m_pendingPopups)
    {
        root->AddChild(std::move(popup));
        ++m_lastBuildStats.popupCount;
    }

    m_root = root;
    m_ui->GetCanvas().AddWidget(root);
    m_pendingPopups.clear();
}

void EditorPopupLayer::Clear(UI::UIContext& ui)
{
    m_ui = nullptr;
    m_pendingPopups.clear();
    m_root.reset();
    m_lastBuildStats = {};
    RemovePopupLayer(ui.GetCanvas());
}

} // namespace RVX::Editor
