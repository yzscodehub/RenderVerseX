#include "HAL/Input/InputBackendFactory.h"

#include "GLFW/GLFWGamepadBackend.h"
#include "GLFW/GLFWInputBackend.h"

namespace RVX::HAL
{
    std::unique_ptr<IInputBackend> CreateInputBackendForWindow(void* internalWindowHandle)
    {
        if (!internalWindowHandle)
        {
            return nullptr;
        }

        return std::make_unique<GLFWInputBackend>(static_cast<GLFWwindow*>(internalWindowHandle));
    }

    std::unique_ptr<IGamepadBackend> CreateGamepadBackend()
    {
        return std::make_unique<GLFWGamepadBackend>();
    }

} // namespace RVX::HAL
