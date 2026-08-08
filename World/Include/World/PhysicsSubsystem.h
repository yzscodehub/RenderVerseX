#pragma once

/**
 * @file PhysicsSubsystem.h
 * @brief World-owned physics simulation subsystem
 */

#include "Core/Subsystem/WorldSubsystem.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/SceneIdentity.h"
#include "Scene/SceneSystemScheduler.h"

#include <memory>
#include <vector>

namespace RVX
{
    class RigidBodyComponent;
    class Scene;

    /**
     * @brief Per-world physics simulation and Scene synchronization.
     *
     * PhysicsSubsystem owns the PhysicsWorld for a World, maintains its
     * RigidBodyComponent handle set from the Scene change feed, registers those
     * components with the simulation, then performs Scene -> Physics -> Scene
     * transform synchronization.
     */
    class PhysicsSubsystem : public WorldSubsystem
    {
    public:
        const char* GetName() const override { return "PhysicsSubsystem"; }

        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;
        bool ShouldTick() const override { return false; }
        TickPhase GetTickPhase() const override { return TickPhase::Update; }

        void SetConfig(const Physics::PhysicsWorldConfig& config);
        void RebindScene();
        const Physics::PhysicsWorldConfig& GetConfig() const { return m_config; }

        Physics::PhysicsWorld* GetPhysicsWorld() { return m_physicsWorld.get(); }
        const Physics::PhysicsWorld* GetPhysicsWorld() const { return m_physicsWorld.get(); }

        size_t GetLastRegisteredBodyComponentCount() const { return m_lastRegisteredBodyComponentCount; }
        uint32 GetLastPhysicsStepCount() const { return m_lastPhysicsStepCount; }
        float GetPhysicsAccumulatorSeconds() const { return m_physicsAccumulatorSeconds; }

    private:
        void GatherRigidBodyComponents(std::vector<RigidBodyComponent*>& outComponents) const;
        void RebuildRigidBodyHandles();
        void ApplyComponentChanges();
        void AttachComponents(std::vector<RigidBodyComponent*>& components);
        void DetachComponents();

        Physics::PhysicsWorldConfig m_config;
        std::unique_ptr<Physics::PhysicsWorld> m_physicsWorld;
        size_t m_lastRegisteredBodyComponentCount = 0;
        uint32 m_lastPhysicsStepCount = 0;
        float m_physicsAccumulatorSeconds = 0.0f;
        SceneSystemHandle m_sceneSystemHandle = InvalidSceneSystemHandle;
        Scene* m_registeredScene = nullptr;
        std::vector<ComponentHandle> m_rigidBodyHandles;
        uint64 m_lastComponentChangeSequence = 0;
    };

} // namespace RVX
