/**
 * @file EditorPendingAction.h
 * @brief Deferred editor action continuation queue
 */

#pragma once

#include "Core/Types.h"

#include <functional>
#include <string>

namespace RVX::Editor
{

using EditorPendingActionCallback = std::function<void()>;

struct EditorPendingActionDesc
{
    std::string id;
    std::string label;
    EditorPendingActionCallback callback;
};

struct EditorPendingActionState
{
    bool pending = false;
    std::string id;
    std::string label;
    uint64 revision = 0;
};

/**
 * @brief Stores one editor action while a modal decision is pending.
 */
class EditorPendingActionQueue
{
public:
    bool Begin(EditorPendingActionDesc desc);
    bool Complete();
    bool Cancel();
    void Clear();

    bool HasPendingAction() const { return m_state.pending; }
    const EditorPendingActionState& GetState() const { return m_state; }
    const std::string& GetLastError() const { return m_lastError; }

private:
    bool Fail(std::string error);
    void ClearError();

    EditorPendingActionState m_state;
    EditorPendingActionCallback m_callback;
    std::string m_lastError;
};

} // namespace RVX::Editor
