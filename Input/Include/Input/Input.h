#pragma once

#include "Input/InputEvents.h"
#include "Input/InputState.h"
#include <vector>

namespace RVX
{
    class Input
    {
    public:
        void ClearFrameState();
        void OnEvent(const InputEvent& event);
        const InputState& GetState() const { return m_state; }
        std::vector<InputEvent> ConsumeEvents();

    private:
        InputState m_state;
        std::vector<InputEvent> m_events;
    };
} // namespace RVX
