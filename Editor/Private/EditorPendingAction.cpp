/**
 * @file EditorPendingAction.cpp
 * @brief Deferred editor action continuation queue implementation
 */

#include "Editor/EditorPendingAction.h"

#include <utility>

namespace RVX::Editor
{

bool EditorPendingActionQueue::Begin(EditorPendingActionDesc desc)
{
    if (m_state.pending)
    {
        return Fail("An editor action is already pending: " + m_state.id);
    }
    if (desc.id.empty())
    {
        return Fail("Cannot begin an unnamed editor action");
    }
    if (!desc.callback)
    {
        return Fail("Cannot begin editor action '" + desc.id +
                    "' without a continuation");
    }

    m_state.pending = true;
    m_state.id = std::move(desc.id);
    m_state.label = std::move(desc.label);
    ++m_state.revision;
    m_callback = std::move(desc.callback);
    ClearError();
    return true;
}

bool EditorPendingActionQueue::Complete()
{
    if (!m_state.pending)
    {
        return Fail("Cannot complete because no editor action is pending");
    }

    EditorPendingActionCallback callback = std::move(m_callback);
    m_state.pending = false;
    m_state.id.clear();
    m_state.label.clear();
    ++m_state.revision;
    ClearError();
    if (callback)
    {
        callback();
    }
    return true;
}

bool EditorPendingActionQueue::Cancel()
{
    if (!m_state.pending)
    {
        return Fail("Cannot cancel because no editor action is pending");
    }

    m_callback = {};
    m_state.pending = false;
    m_state.id.clear();
    m_state.label.clear();
    ++m_state.revision;
    ClearError();
    return true;
}

void EditorPendingActionQueue::Clear()
{
    m_callback = {};
    m_state = {};
    ClearError();
}

bool EditorPendingActionQueue::Fail(std::string error)
{
    m_lastError = std::move(error);
    return false;
}

void EditorPendingActionQueue::ClearError()
{
    m_lastError.clear();
}

} // namespace RVX::Editor
