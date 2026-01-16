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
        
        /// @brief Get current input state (convenience accessor)
        const InputState& GetState() const { return m_input.GetState(); }

        void OnUpdate(float) override
        {
            m_input.ClearFrameState();
            if (m_backend)
            {
                m_backend->Poll(m_input.GetMutableState());
            }
        }

    private:
        Input m_input;
        std::unique_ptr<InputBackend> m_backend;
    };
} // namespace RVX
