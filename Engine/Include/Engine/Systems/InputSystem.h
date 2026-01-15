#pragma once

#include "Core/ISystem.h"
#include "Input/Input.h"
#include "Platform/InputBackend.h"
#include <memory>

namespace RVX
{
    class InputSystem : public ISystem
    {
    public:
        const char* GetName() const override { return "InputSystem"; }

        void SetBackend(std::unique_ptr<InputBackend> backend) { m_backend = std::move(backend); }
        const Input& GetInput() const { return m_input; }
        Input& GetInput() { return m_input; }

        void OnUpdate(float) override
        {
            m_input.ClearFrameState();
            if (m_backend)
            {
                m_inputState.mouseDeltaX = 0.0f;
                m_inputState.mouseDeltaY = 0.0f;
                m_inputState.mouseWheel = 0.0f;
                m_backend->Poll(m_inputState);
            }
        }

        const InputState& GetState() const { return m_inputState; }

    private:
        Input m_input;
        InputState m_inputState;
        std::unique_ptr<InputBackend> m_backend;
    };
} // namespace RVX
