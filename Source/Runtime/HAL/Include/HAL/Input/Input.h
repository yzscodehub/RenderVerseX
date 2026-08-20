#pragma once

/**
 * @file Input.h
 * @brief High-level input management
 * 
 * This provides the Input class which manages input state and events.
 * For low-level input backend, see HAL/Input/IInputBackend.h
 */

#include "HAL/Input/IInputBackend.h"
#include "HAL/Input/InputEvents.h"
#include "HAL/Input/InputState.h"
#include <vector>

namespace RVX::HAL
{
    /**
     * @brief High-level input state manager
     * 
     * Aggregates input from backends and provides event/state access.
     */
    class Input
    {
    public:
        /// Clear per-frame state (deltas, etc.)
        void ClearFrameState();
        
        /// Process an input event
        void OnEvent(const InputEvent& event);
        
        /// Get current input state (read-only)
        const InputState& GetState() const { return m_state; }
        
        /// Get mutable state for backend polling
        InputState& GetMutableState() { return m_state; }
        
        /// Consume and return all pending events
        std::vector<InputEvent> ConsumeEvents();

    private:
        InputState m_state;
        std::vector<InputEvent> m_events;
    };

} // namespace RVX::HAL

// Backward compatibility
namespace RVX
{
    using Input = HAL::Input;
}
