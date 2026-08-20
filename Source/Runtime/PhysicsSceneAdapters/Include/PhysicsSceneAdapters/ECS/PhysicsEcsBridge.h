#pragma once

/**
 * @file PhysicsEcsBridge.h
 * @brief Pure ECS adapter from Scene physics fragments to the Built-in physics world.
 */

#include "ECS/Entity.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/ECS/PhysicsFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <memory>
#include <vector>

namespace RVX::PhysicsSceneAdapters
{
    /** @brief Explicit registration result; this slice never claims a backend fallback. */
    enum class PhysicsEcsBridgeRegistrationResult : uint8
    {
        Registered = 0,
        AlreadyRegistered,
        InvalidRuntime,
        PhysicsWorldUnavailable,
        UnsupportedBackend,
        ProcessorRegistrationFailed,
    };

    /** @brief Value-only diagnostic for a bridge-owned entity/body association. */
    struct PhysicsEcsBodyBindingDiagnostic
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        Physics::BodyHandle body = Physics::BodyHandle::Invalid();
        SceneECS::PhysicsBodyBindingStatus status =
            SceneECS::PhysicsBodyBindingStatus::Unbound;
        uint64 lastConsumedRootMotionSequence = 0;
        uint64 lastConsumedRootMotionFixedStep = 0;
        bool awaitingCleanupAcknowledgement = false;
    };

    /** @brief Deterministic bridge counters and associations ordered by EntityHandle. */
    struct PhysicsEcsBridgeDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        PhysicsEcsBridgeRegistrationResult registration =
            PhysicsEcsBridgeRegistrationResult::InvalidRuntime;
        uint32 activeBodyCount = 0;
        uint32 pendingCleanupCount = 0;
        uint64 structuralContinuityLossCount = 0;
        uint64 cleanupContinuityLossCount = 0;
        uint64 authoritativeReconcileCount = 0;
        uint64 createFailureCount = 0;
        uint64 unsupportedColliderCount = 0;
        uint64 sceneToPhysicsPushCount = 0;
        uint64 physicsToScenePullCount = 0;
        uint64 rootMotionAppliedCount = 0;
        uint64 rootMotionRejectedCount = 0;
        uint64 rootMotionReplayCount = 0;
        uint64 rootMotionGapCount = 0;
        std::vector<PhysicsEcsBodyBindingDiagnostic> bindings;
    };

    /**
     * @brief Registers pure-ECS physics processors against one Scene runtime.
     *
     * The bridge owns only value handles and dense side tables. It does not
     * retain Actors, SceneEntity, Components, Registry pointers, or RigidBody
     * pointers. The referenced PhysicsWorld must outlive the bridge. Keep the
     * bridge alive until the owning runtime is no longer ticked; processor
     * callbacks automatically become inert after destruction.
     */
    class PhysicsEcsBridge
    {
    public:
        PhysicsEcsBridge(SceneECS::SceneEcsRuntime& runtime, Physics::PhysicsWorld& physicsWorld);
        ~PhysicsEcsBridge();

        PhysicsEcsBridge(const PhysicsEcsBridge&) = delete;
        PhysicsEcsBridge& operator=(const PhysicsEcsBridge&) = delete;
        PhysicsEcsBridge(PhysicsEcsBridge&&) = delete;
        PhysicsEcsBridge& operator=(PhysicsEcsBridge&&) = delete;

        /** @brief Register the fixed phase and cleanup processors once. */
        [[nodiscard]] PhysicsEcsBridgeRegistrationResult RegisterProcessors();
        [[nodiscard]] bool IsRegistered() const;
        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;

        /** @brief Look up a bridge-owned body only when the runtime identity matches. */
        [[nodiscard]] Physics::BodyHandle FindBody(ECS::SceneRuntimeId sceneRuntimeId,
                                                   ECS::EntityHandle entity) const;
        /** @brief Snapshot all associations in stable EntityHandle order. */
        [[nodiscard]] PhysicsEcsBridgeDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
} // namespace RVX::PhysicsSceneAdapters
