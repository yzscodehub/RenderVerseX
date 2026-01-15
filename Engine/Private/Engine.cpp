#include "Engine/Engine.h"

namespace RVX
{
    void Engine::Init()
    {
        m_systems.InitAll();
    }

    void Engine::Tick(float deltaTime)
    {
        m_systems.UpdateAll(deltaTime);
        m_systems.RenderAll();
    }

    void Engine::Shutdown()
    {
        m_systems.ShutdownAll();
        m_systems.Clear();
    }

    void Engine::Run(const std::function<bool()>& shouldExit, const std::function<float()>& getDeltaTime)
    {
        Init();
        while (!shouldExit || !shouldExit())
        {
            float dt = getDeltaTime ? getDeltaTime() : 0.016f;
            Tick(dt);
        }
        Shutdown();
    }
} // namespace RVX
