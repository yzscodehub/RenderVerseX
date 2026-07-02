/**
 * @file EditorDockingModel.cpp
 * @brief Native editor dock layout model implementation
 */

#include "Editor/UI/EditorDockingModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace RVX::Editor
{

void EditorDockingModel::Clear()
{
    m_placements.clear();
    m_areaSizing = {};
    m_bottomAreaCollapsed = false;
    m_dropPreview = {};
    m_tabStackStates.clear();
}

bool EditorDockingModel::DockPanel(EditorDockPanelPlacement placement)
{
    if (placement.panelId.empty())
    {
        return false;
    }

    if (placement.hasFloatingBounds)
    {
        placement.floatingBounds = ClampFloatingBounds(placement.floatingBounds);
    }
    if (placement.area == EditorUIPanelDockArea::Floating)
    {
        placement.tabStackId.clear();
        if (placement.floatingZOrder == 0)
        {
            placement.floatingZOrder = AllocateFloatingZOrder();
        }
    }
    else
    {
        placement.floatingZOrder = 0;
    }

    if (EditorDockPanelPlacement* existing = FindPlacement(placement.panelId))
    {
        const std::string previousStackId = existing->tabStackId;
        *existing = std::move(placement);
        EnsureTabStackActivePanel(previousStackId);
        EnsureTabStackActivePanel(existing->tabStackId);
        return true;
    }

    if (placement.order == 0)
    {
        placement.order = static_cast<uint32>(m_placements.size());
    }
    m_placements.push_back(std::move(placement));
    EnsureTabStackActivePanel(m_placements.back().tabStackId);
    return true;
}

bool EditorDockingModel::UndockPanel(const std::string& panelId)
{
    const auto it = std::find_if(m_placements.begin(),
                                 m_placements.end(),
                                 [&panelId](const EditorDockPanelPlacement& placement) {
                                     return placement.panelId == panelId;
                                 });
    if (it == m_placements.end())
    {
        return false;
    }

    const std::string tabStackId = it->tabStackId;
    m_placements.erase(it);
    ClearDockDropPreview(panelId);
    EnsureTabStackActivePanel(tabStackId);
    return true;
}

EditorDockPanelPlacement* EditorDockingModel::FindPlacement(const std::string& panelId)
{
    const auto it = std::find_if(m_placements.begin(),
                                 m_placements.end(),
                                 [&panelId](const EditorDockPanelPlacement& placement) {
                                     return placement.panelId == panelId;
                                 });
    return it == m_placements.end() ? nullptr : &(*it);
}

const EditorDockPanelPlacement* EditorDockingModel::FindPlacement(const std::string& panelId) const
{
    const auto it = std::find_if(m_placements.begin(),
                                 m_placements.end(),
                                 [&panelId](const EditorDockPanelPlacement& placement) {
                                     return placement.panelId == panelId;
                                 });
    return it == m_placements.end() ? nullptr : &(*it);
}

void EditorDockingModel::SetPanelVisible(const std::string& panelId, bool visible)
{
    if (EditorDockPanelPlacement* placement = FindPlacement(panelId))
    {
        placement->visible = visible;
        EnsureTabStackActivePanel(placement->tabStackId);
    }
}

bool EditorDockingModel::SetDockAreaSize(EditorUIPanelDockArea area, float normalizedSize)
{
    switch (area)
    {
        case EditorUIPanelDockArea::Left:
            m_areaSizing.leftWidthRatio = ClampAreaSize(normalizedSize);
            return true;
        case EditorUIPanelDockArea::Right:
            m_areaSizing.rightWidthRatio = ClampAreaSize(normalizedSize);
            return true;
        case EditorUIPanelDockArea::Bottom:
            m_areaSizing.bottomHeightRatio = ClampAreaSize(normalizedSize);
            return true;
        case EditorUIPanelDockArea::Center:
        case EditorUIPanelDockArea::Floating:
            return false;
    }

    return false;
}

float EditorDockingModel::GetDockAreaSize(EditorUIPanelDockArea area) const
{
    switch (area)
    {
        case EditorUIPanelDockArea::Left:
            return m_areaSizing.leftWidthRatio;
        case EditorUIPanelDockArea::Right:
            return m_areaSizing.rightWidthRatio;
        case EditorUIPanelDockArea::Bottom:
            return m_areaSizing.bottomHeightRatio;
        case EditorUIPanelDockArea::Center:
        case EditorUIPanelDockArea::Floating:
            return 1.0f;
    }

    return 1.0f;
}

bool EditorDockingModel::SetDockAreaCollapsed(EditorUIPanelDockArea area,
                                              bool collapsed)
{
    switch (area)
    {
        case EditorUIPanelDockArea::Bottom:
            m_bottomAreaCollapsed = collapsed;
            return true;
        case EditorUIPanelDockArea::Center:
        case EditorUIPanelDockArea::Left:
        case EditorUIPanelDockArea::Right:
        case EditorUIPanelDockArea::Floating:
            return false;
    }

    return false;
}

bool EditorDockingModel::IsDockAreaCollapsed(EditorUIPanelDockArea area) const
{
    switch (area)
    {
        case EditorUIPanelDockArea::Bottom:
            return m_bottomAreaCollapsed;
        case EditorUIPanelDockArea::Center:
        case EditorUIPanelDockArea::Left:
        case EditorUIPanelDockArea::Right:
        case EditorUIPanelDockArea::Floating:
            return false;
    }

    return false;
}

void EditorDockingModel::SetAreaSizing(const EditorDockAreaSizing& sizing)
{
    m_areaSizing.leftWidthRatio = ClampAreaSize(sizing.leftWidthRatio);
    m_areaSizing.rightWidthRatio = ClampAreaSize(sizing.rightWidthRatio);
    m_areaSizing.bottomHeightRatio = ClampAreaSize(sizing.bottomHeightRatio);
}

bool EditorDockingModel::SetFloatingPanelBounds(
    const std::string& panelId,
    const EditorDockFloatingBounds& bounds)
{
    EditorDockPanelPlacement* placement = FindPlacement(panelId);
    if (!placement || placement->area != EditorUIPanelDockArea::Floating)
    {
        return false;
    }

    placement->floatingBounds = ClampFloatingBounds(bounds);
    placement->hasFloatingBounds = true;
    return true;
}

bool EditorDockingModel::BringFloatingPanelToFront(const std::string& panelId)
{
    EditorDockPanelPlacement* placement = FindPlacement(panelId);
    if (!placement || placement->area != EditorUIPanelDockArea::Floating)
    {
        return false;
    }

    placement->floatingZOrder = AllocateFloatingZOrder();
    return true;
}

bool EditorDockingModel::SetPanelDockArea(const std::string& panelId,
                                          EditorUIPanelDockArea area)
{
    EditorDockPanelPlacement* placement = FindPlacement(panelId);
    if (!placement)
    {
        return false;
    }

    const std::string previousStackId = placement->tabStackId;
    const EditorUIPanelDockArea previousArea = placement->area;
    placement->area = area;
    placement->order = AllocateDockAreaOrder(area);
    if (area == EditorUIPanelDockArea::Floating || area != previousArea ||
        !previousStackId.empty())
    {
        placement->tabStackId.clear();
    }
    if (area == EditorUIPanelDockArea::Floating)
    {
        placement->floatingZOrder = AllocateFloatingZOrder();
    }
    else
    {
        placement->hasFloatingBounds = false;
        placement->floatingZOrder = 0;
    }
    ClearDockDropPreview(panelId);
    EnsureTabStackActivePanel(previousStackId);
    EnsureTabStackActivePanel(placement->tabStackId);
    return true;
}

bool EditorDockingModel::SetPanelDockTabStack(const std::string& panelId,
                                              EditorUIPanelDockArea area,
                                              const std::string& stackId)
{
    if (panelId.empty() || stackId.empty() ||
        area == EditorUIPanelDockArea::Floating)
    {
        return false;
    }

    EditorDockPanelPlacement* placement = FindPlacement(panelId);
    if (!placement)
    {
        return false;
    }

    bool targetStackExists = false;
    for (const EditorDockPanelPlacement& candidate : m_placements)
    {
        if (candidate.panelId != panelId && candidate.visible &&
            candidate.area == area && candidate.tabStackId == stackId)
        {
            targetStackExists = true;
            break;
        }
    }
    if (!targetStackExists)
    {
        return false;
    }

    const std::string previousStackId = placement->tabStackId;
    placement->area = area;
    placement->tabStackId = stackId;
    placement->order = AllocateDockAreaOrder(area);
    placement->hasFloatingBounds = false;
    placement->floatingZOrder = 0;
    ClearDockDropPreview(panelId);
    EnsureTabStackActivePanel(previousStackId);
    EnsureTabStackActivePanel(stackId);
    return SetActiveDockTab(panelId);
}

bool EditorDockingModel::SetDockDropPreview(const std::string& panelId,
                                            EditorUIPanelDockArea area,
                                            const std::string& tabStackId)
{
    if (panelId.empty() || area == EditorUIPanelDockArea::Floating)
    {
        return false;
    }

    const EditorDockPanelPlacement* placement = FindPlacement(panelId);
    if (!placement || !placement->visible)
    {
        return false;
    }

    const bool floatingDrag = placement->area == EditorUIPanelDockArea::Floating;
    const bool dockTabDrag = !floatingDrag && !placement->tabStackId.empty();
    if (!floatingDrag && !dockTabDrag)
    {
        return false;
    }

    if (!tabStackId.empty())
    {
        bool targetStackExists = false;
        for (const EditorDockPanelPlacement& candidate : m_placements)
        {
            if (candidate.panelId != panelId && candidate.visible &&
                candidate.area == area && candidate.tabStackId == tabStackId)
            {
                targetStackExists = true;
                break;
            }
        }
        if (!targetStackExists)
        {
            return false;
        }
    }

    m_dropPreview.active = true;
    m_dropPreview.panelId = panelId;
    m_dropPreview.area = area;
    m_dropPreview.tabStackId = tabStackId;
    return true;
}

void EditorDockingModel::ClearDockDropPreview(const std::string& panelId)
{
    if (!m_dropPreview.active)
    {
        return;
    }

    if (!panelId.empty() && m_dropPreview.panelId != panelId)
    {
        return;
    }

    m_dropPreview = {};
}

bool EditorDockingModel::SetPanelTabStack(const std::string& panelId,
                                          const std::string& stackId)
{
    EditorDockPanelPlacement* placement = FindPlacement(panelId);
    if (!placement || placement->area == EditorUIPanelDockArea::Floating)
    {
        return false;
    }

    const std::string previousStackId = placement->tabStackId;
    placement->tabStackId = stackId;
    EnsureTabStackActivePanel(previousStackId);
    EnsureTabStackActivePanel(placement->tabStackId);
    return true;
}

bool EditorDockingModel::SetActiveDockTab(const std::string& panelId)
{
    const EditorDockPanelPlacement* placement = FindPlacement(panelId);
    if (!placement || !placement->visible ||
        placement->area == EditorUIPanelDockArea::Floating ||
        placement->tabStackId.empty())
    {
        return false;
    }

    EditorDockTabStackState* state = FindTabStackState(placement->tabStackId);
    if (!state)
    {
        m_tabStackStates.push_back({placement->tabStackId, panelId});
        return true;
    }

    state->activePanelId = panelId;
    return true;
}

bool EditorDockingModel::MoveDockTab(const std::string& panelId,
                                     const std::string& targetPanelId,
                                     bool afterTarget)
{
    EditorDockPanelPlacement* source = FindPlacement(panelId);
    const EditorDockPanelPlacement* target = FindPlacement(targetPanelId);
    if (!source || !target || panelId.empty() || targetPanelId.empty() ||
        panelId == targetPanelId || !source->visible || !target->visible ||
        source->area == EditorUIPanelDockArea::Floating ||
        target->area == EditorUIPanelDockArea::Floating ||
        source->area != target->area || source->tabStackId.empty() ||
        source->tabStackId != target->tabStackId)
    {
        return false;
    }

    std::vector<EditorDockPanelPlacement*> stackPlacements;
    for (EditorDockPanelPlacement& placement : m_placements)
    {
        if (placement.visible && placement.area == source->area &&
            placement.tabStackId == source->tabStackId)
        {
            stackPlacements.push_back(&placement);
        }
    }

    if (stackPlacements.size() < 2)
    {
        return false;
    }

    std::stable_sort(stackPlacements.begin(),
                     stackPlacements.end(),
                     [](const EditorDockPanelPlacement* lhs,
                        const EditorDockPanelPlacement* rhs) {
                         if (!lhs || !rhs)
                         {
                             return lhs != nullptr;
                         }
                         if (lhs->order != rhs->order)
                         {
                             return lhs->order < rhs->order;
                         }
                         return lhs->panelId < rhs->panelId;
                     });

    std::vector<uint32> orderSlots;
    orderSlots.reserve(stackPlacements.size());
    for (const EditorDockPanelPlacement* placement : stackPlacements)
    {
        orderSlots.push_back(placement ? placement->order : 0u);
    }

    auto sourceIt = std::find(stackPlacements.begin(), stackPlacements.end(), source);
    auto targetIt = std::find_if(stackPlacements.begin(),
                                 stackPlacements.end(),
                                 [&targetPanelId](const EditorDockPanelPlacement* placement) {
                                     return placement &&
                                            placement->panelId == targetPanelId;
                                 });
    if (sourceIt == stackPlacements.end() || targetIt == stackPlacements.end())
    {
        return false;
    }

    EditorDockPanelPlacement* moved = *sourceIt;
    stackPlacements.erase(sourceIt);
    targetIt = std::find_if(stackPlacements.begin(),
                            stackPlacements.end(),
                            [&targetPanelId](const EditorDockPanelPlacement* placement) {
                                return placement &&
                                       placement->panelId == targetPanelId;
                            });
    if (targetIt == stackPlacements.end())
    {
        return false;
    }
    if (afterTarget)
    {
        ++targetIt;
    }
    stackPlacements.insert(targetIt, moved);

    for (size_t index = 0; index < stackPlacements.size(); ++index)
    {
        if (stackPlacements[index])
        {
            stackPlacements[index]->order = orderSlots[index];
        }
    }
    EnsureTabStackActivePanel(source->tabStackId);
    return true;
}

bool EditorDockingModel::DetachDockTabToFloating(
    const std::string& panelId,
    const EditorDockFloatingBounds& bounds)
{
    EditorDockPanelPlacement* placement = FindPlacement(panelId);
    if (!placement || panelId.empty() || !placement->visible ||
        placement->area == EditorUIPanelDockArea::Floating ||
        placement->tabStackId.empty())
    {
        return false;
    }

    const std::string previousStackId = placement->tabStackId;
    placement->area = EditorUIPanelDockArea::Floating;
    placement->tabStackId.clear();
    placement->order = AllocateDockAreaOrder(EditorUIPanelDockArea::Floating);
    placement->floatingBounds = ClampFloatingBounds(bounds);
    placement->hasFloatingBounds = true;
    placement->floatingZOrder = AllocateFloatingZOrder();
    ClearDockDropPreview(panelId);
    EnsureTabStackActivePanel(previousStackId);
    return true;
}

std::string EditorDockingModel::GetActiveDockTab(const std::string& stackId) const
{
    if (const EditorDockTabStackState* state = FindTabStackState(stackId))
    {
        return state->activePanelId;
    }

    return {};
}

std::string EditorDockingModel::ResolveActiveDockTab(const std::string& stackId) const
{
    if (stackId.empty())
    {
        return {};
    }

    if (const EditorDockTabStackState* state = FindTabStackState(stackId))
    {
        if (const EditorDockPanelPlacement* activePlacement =
                FindPlacement(state->activePanelId))
        {
            if (activePlacement->visible &&
                activePlacement->area != EditorUIPanelDockArea::Floating &&
                activePlacement->tabStackId == stackId)
            {
                return activePlacement->panelId;
            }
        }
    }

    const EditorDockPanelPlacement* fallback = nullptr;
    for (const EditorDockPanelPlacement& placement : m_placements)
    {
        if (!placement.visible || placement.area == EditorUIPanelDockArea::Floating ||
            placement.tabStackId != stackId)
        {
            continue;
        }

        if (!fallback || placement.order < fallback->order ||
            (placement.order == fallback->order && placement.panelId < fallback->panelId))
        {
            fallback = &placement;
        }
    }

    return fallback ? fallback->panelId : std::string();
}

uint32 EditorDockingModel::GetPanelCount(EditorUIPanelDockArea area) const
{
    return static_cast<uint32>(std::count_if(m_placements.begin(),
                                             m_placements.end(),
                                             [area](const EditorDockPanelPlacement& placement) {
                                                 return placement.area == area;
                                             }));
}

uint32 EditorDockingModel::GetTabStackPanelCount(const std::string& stackId) const
{
    if (stackId.empty())
    {
        return 0;
    }

    return static_cast<uint32>(std::count_if(m_placements.begin(),
                                             m_placements.end(),
                                             [&stackId](const EditorDockPanelPlacement& placement) {
                                                 return placement.visible &&
                                                        placement.area !=
                                                            EditorUIPanelDockArea::Floating &&
                                                        placement.tabStackId == stackId;
                                             }));
}

float EditorDockingModel::ClampAreaSize(float value)
{
    if (!std::isfinite(value))
    {
        return 0.20f;
    }

    return std::clamp(value, 0.08f, 0.45f);
}

EditorDockFloatingBounds EditorDockingModel::ClampFloatingBounds(
    const EditorDockFloatingBounds& bounds)
{
    EditorDockFloatingBounds clamped = bounds;
    if (!std::isfinite(clamped.width))
    {
        clamped.width = 0.32f;
    }
    if (!std::isfinite(clamped.height))
    {
        clamped.height = 0.32f;
    }
    clamped.width = std::clamp(clamped.width, 0.10f, 0.90f);
    clamped.height = std::clamp(clamped.height, 0.10f, 0.90f);

    if (!std::isfinite(clamped.x))
    {
        clamped.x = 0.08f;
    }
    if (!std::isfinite(clamped.y))
    {
        clamped.y = 0.08f;
    }

    clamped.x = std::clamp(clamped.x, 0.0f, std::max(0.0f, 1.0f - clamped.width));
    clamped.y = std::clamp(clamped.y, 0.0f, std::max(0.0f, 1.0f - clamped.height));
    return clamped;
}

EditorDockTabStackState* EditorDockingModel::FindTabStackState(
    const std::string& stackId)
{
    const auto it = std::find_if(m_tabStackStates.begin(),
                                 m_tabStackStates.end(),
                                 [&stackId](const EditorDockTabStackState& state) {
                                     return state.stackId == stackId;
                                 });
    return it == m_tabStackStates.end() ? nullptr : &(*it);
}

const EditorDockTabStackState* EditorDockingModel::FindTabStackState(
    const std::string& stackId) const
{
    const auto it = std::find_if(m_tabStackStates.begin(),
                                 m_tabStackStates.end(),
                                 [&stackId](const EditorDockTabStackState& state) {
                                     return state.stackId == stackId;
                                 });
    return it == m_tabStackStates.end() ? nullptr : &(*it);
}

void EditorDockingModel::EnsureTabStackActivePanel(const std::string& stackId)
{
    if (stackId.empty())
    {
        RemoveEmptyTabStackStates();
        return;
    }

    const std::string activePanelId = ResolveActiveDockTab(stackId);
    if (activePanelId.empty())
    {
        const auto it = std::remove_if(m_tabStackStates.begin(),
                                       m_tabStackStates.end(),
                                       [&stackId](const EditorDockTabStackState& state) {
                                           return state.stackId == stackId;
                                       });
        m_tabStackStates.erase(it, m_tabStackStates.end());
        return;
    }

    EditorDockTabStackState* state = FindTabStackState(stackId);
    if (!state)
    {
        m_tabStackStates.push_back({stackId, activePanelId});
        return;
    }

    state->activePanelId = activePanelId;
}

void EditorDockingModel::RemoveEmptyTabStackStates()
{
    const auto it = std::remove_if(
        m_tabStackStates.begin(),
        m_tabStackStates.end(),
        [this](const EditorDockTabStackState& state) {
            return state.stackId.empty() || ResolveActiveDockTab(state.stackId).empty();
        });
    m_tabStackStates.erase(it, m_tabStackStates.end());
}

uint32 EditorDockingModel::AllocateDockAreaOrder(EditorUIPanelDockArea area) const
{
    uint32 maxOrder = 0;
    for (const EditorDockPanelPlacement& placement : m_placements)
    {
        if (placement.area == area)
        {
            maxOrder = std::max(maxOrder, placement.order);
        }
    }
    return maxOrder + 1u;
}

uint32 EditorDockingModel::AllocateFloatingZOrder()
{
    uint32 maxZOrder = 0;
    for (const EditorDockPanelPlacement& placement : m_placements)
    {
        if (placement.area == EditorUIPanelDockArea::Floating)
        {
            maxZOrder = std::max(maxZOrder, placement.floatingZOrder);
        }
    }

    if (maxZOrder < std::numeric_limits<uint32>::max() - 1u)
    {
        return maxZOrder + 1u;
    }

    std::vector<EditorDockPanelPlacement*> floatingPlacements;
    floatingPlacements.reserve(m_placements.size());
    for (EditorDockPanelPlacement& placement : m_placements)
    {
        if (placement.area == EditorUIPanelDockArea::Floating)
        {
            floatingPlacements.push_back(&placement);
        }
    }

    std::stable_sort(floatingPlacements.begin(),
                     floatingPlacements.end(),
                     [](const EditorDockPanelPlacement* lhs,
                        const EditorDockPanelPlacement* rhs) {
                         if (lhs->floatingZOrder != rhs->floatingZOrder)
                         {
                             return lhs->floatingZOrder < rhs->floatingZOrder;
                         }
                         if (lhs->order != rhs->order)
                         {
                             return lhs->order < rhs->order;
                         }
                         return lhs->panelId < rhs->panelId;
                     });

    uint32 compactedZOrder = 1;
    for (EditorDockPanelPlacement* placement : floatingPlacements)
    {
        placement->floatingZOrder = compactedZOrder++;
    }

    return compactedZOrder;
}

} // namespace RVX::Editor
