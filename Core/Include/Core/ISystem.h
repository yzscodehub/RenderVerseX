#pragma once

namespace RVX
{
    class ISystem
    {
    public:
        virtual ~ISystem() = default;
        virtual const char* GetName() const = 0;

        virtual void OnInit() {}
        virtual void OnShutdown() {}
        virtual void OnUpdate(float deltaTime) { (void)deltaTime; }
        virtual void OnRender() {}
    };
} // namespace RVX
