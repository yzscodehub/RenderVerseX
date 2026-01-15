#pragma once

#include "Core/SystemManager.h"
#include <functional>

namespace RVX
{
    class Engine
    {
    public:
        SystemManager& GetSystemManager() { return m_systems; }

        void Init();
        void Tick(float deltaTime);
        void Shutdown();

        void Run(const std::function<bool()>& shouldExit, const std::function<float()>& getDeltaTime);

    private:
        SystemManager m_systems;
    };
} // namespace RVX
