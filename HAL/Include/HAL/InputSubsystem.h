#pragma once

/**
 * @file InputSubsystem.h
 * @brief Input handling subsystem
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Input/Input.h"
#include "Input/InputState.h"
#include "Platform/InputBackend.h"
#include <memory>
#include <array>

namespace RVX
{
    class Window;

    /**
     * @brief Input subsystem - manages input state and events
     * 
     * This subsystem provides polling-based input.
     * 
     * Usage:
     * @code
     * auto* inputSys = engine.GetSubsystem<InputSubsystem>();
     * 
     * // Polling-based
     * if (inputSys->IsKeyDown(KeyCode::W)) {
     *     MoveForward();
     * }
     * 
     * if (inputSys->IsKeyPressed(KeyCode::Space)) {
     *     Jump();  // Only triggers once when pressed
     * }
     * @endcode
     */
    class InputSubsystem : public EngineSubsystem
    {
    public:
        const char* GetName() const override { return "InputSubsystem"; }

        /// Dependencies - requires WindowSubsystem
        const char** GetDependencies(int& outCount) const override
        {
            static const char* deps[] = { "WindowSubsystem" };
            outCount = 1;
            return deps;
        }

        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;
        bool ShouldTick() const override { return true; }

        /// Set the window to capture input from
        void SetWindow(Window* window);

        // =====================================================================
        // Polling API
        // =====================================================================

        /// Check if a key is currently pressed
        bool IsKeyDown(int keyCode) const;

        /// Check if a key was just pressed this frame
        bool IsKeyPressed(int keyCode) const;

        /// Check if a key was just released this frame
        bool IsKeyReleased(int keyCode) const;

        /// Check if a mouse button is currently pressed
        bool IsMouseButtonDown(int button) const;

        /// Check if a mouse button was just pressed this frame
        bool IsMouseButtonPressed(int button) const;

        /// Check if a mouse button was just released this frame
        bool IsMouseButtonReleased(int button) const;

        /// Get current mouse position
        void GetMousePosition(float& x, float& y) const;

        /// Get mouse movement since last frame
        void GetMouseDelta(float& dx, float& dy) const;

        /// Get scroll wheel delta
        void GetScrollDelta(float& x, float& y) const;

        /// Get the underlying Input object (for advanced use)
        Input& GetInput() { return m_input; }
        const Input& GetInput() const { return m_input; }

    private:
        Input m_input;
        std::unique_ptr<InputBackend> m_backend;
        Window* m_window = nullptr;
        
        // Per-frame state tracking
        std::array<bool, RVX_MAX_KEYS> m_keysPressed{};
        std::array<bool, RVX_MAX_KEYS> m_keysReleased{};
        std::array<bool, RVX_MAX_MOUSE_BUTTONS> m_mouseButtonsPressed{};
        std::array<bool, RVX_MAX_MOUSE_BUTTONS> m_mouseButtonsReleased{};
        
        float m_lastMouseX = 0.0f;
        float m_lastMouseY = 0.0f;
    };

} // namespace RVX
