#pragma once

/**
 * @file PhysicsSubsystem.h
 * @brief World-owned physics simulation subsystem
 */

#include "Core/Subsystem/WorldSubsystem.h"
#include "Physics/PhysicsWorld.h"

#include <memory>
#include <vector>

namespace RVX
{
    class RigidBodyComponent;

    /**
     * @brief Per-world physics simulation and Scene synchronization.
     *
     * PhysicsSubsystem owns the PhysicsWorld for a World, discovers
     * RigidBodyComponent instances in the SceneManager, registers them with the
     * physics simulation, then performs Scene -> Physics -> Scene transform sync.
     */
    class PhysicsSubsystem : public WorldSubsystem
    {
    public:
        const char* GetName() const override { return "PhysicsSubsystem"; }

        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;
        bool ShouldTick() const override { return true; }
        TickPhase GetTickPhase() const override { return TickPhase::Update; }

        void SetConfig(const Physics::PhysicsWorldConfig& config);
        const Physics::PhysicsWorldConfig& GetConfig() const { return m_config; }

        Physics::PhysicsWorld* GetPhysicsWorld() { return m_physicsWorld.get(); }
        const Physics::PhysicsWorld* GetPhysicsWorld() const { return m_physicsWorld.get(); }

        size_t GetLastRegisteredBodyComponentCount() const { return m_lastRegisteredBodyComponentCount; }
        uint32 GetLastPhysicsStepCount() const { return m_lastPhysicsStepCount; }
        float GetPhysicsAccumulatorSeconds() const { return m_physicsAccumulatorSeconds; }

    private:
        void GatherRigidBodyComponents(std::vector<RigidBodyComponent*>& outComponents) const;
        void AttachComponents(std::vector<RigidBodyComponent*>& components);
        void DetachComponents();

        Physics::PhysicsWorldConfig m_config;
        std::unique_ptr<Physics::PhysicsWorld> m_physicsWorld;
        size_t m_lastRegisteredBodyComponentCount = 0;
        uint32 m_lastPhysicsStepCount = 0;
        float m_physicsAccumulatorSeconds = 0.0f;
    };

} // namespace RVX
