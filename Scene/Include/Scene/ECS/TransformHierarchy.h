#pragma once

/**
 * @file TransformHierarchy.h
 * @brief Handle-only transform hierarchy resolution for the Scene runtime ECS.
 */

#include "ECS/Registry.h"
#include "Scene/ECS/Fragments.h"

#include <unordered_map>
#include <vector>

namespace RVX::SceneECS
{
    /** @brief Local-space policy to apply while changing an entity's parent edge. */
    enum class ReparentMode : uint8
    {
        KeepLocal = 0,
        KeepWorld,
    };

    /** @brief Explicit result of a hierarchy edit. */
    enum class ReparentResult : uint8
    {
        Applied = 0,
        InvalidChild,
        InvalidParent,
        MissingTransform,
        SelfParent,
        Cycle,
        NonInvertibleParent,
        MutationRejected,
    };

    /** @brief Result of applying a world-space pose through hierarchy-local state. */
    enum class SetWorldPoseResult : uint8
    {
        Applied = 0,
        InvalidEntity,
        MissingTransform,
        InvalidPose,
        NonInvertibleParent,
        MutationRejected,
    };

    /** @brief Deterministic work summary for one simulation transform resolve. */
    struct TransformResolveStats
    {
        uint32 entityCount = 0;
        uint32 resolvedCount = 0;
        uint32 unchangedCount = 0;
        uint32 invalidParentCount = 0;
        uint32 cycleCount = 0;
    };

    /**
     * @brief Resolves Scene ECS transform fragments without retaining object-model state.
     *
     * ParentRelation is the sole authoritative hierarchy edge. The cached parent
     * fields in SimulationWorldTransform only identify the inputs used for its
     * most recent resolve; they are never consulted to derive hierarchy topology.
     */
    class TransformHierarchy
    {
    public:
        explicit TransformHierarchy(ECS::Registry& registry);

        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;
        [[nodiscard]] ECS::EntityHandle GetParent(ECS::EntityHandle entity) const;
        /** @brief Derive all children from authoritative ParentRelation fragments. */
        [[nodiscard]] std::vector<ECS::EntityHandle> GetChildren(ECS::EntityHandle parent) const;

        bool SetLocalTransform(ECS::EntityHandle entity, const LocalTransform& transform);
        /**
         * @brief Apply a translation/rotation teleport while preserving hierarchy ownership.
         *
         * The input is world-space. The method derives a local transform from
         * ParentRelation and the resolved parent world matrix, so callers do
         * not bypass parent-relative transform invariants.
         */
        [[nodiscard]] SetWorldPoseResult SetWorldPose(ECS::EntityHandle entity,
                                                      const Vec3& worldTranslation,
                                                      const Quat& worldRotation);
        [[nodiscard]] ReparentResult Reparent(ECS::EntityHandle child,
                                              ECS::EntityHandle parent,
                                              ReparentMode mode = ReparentMode::KeepLocal);
        [[nodiscard]] ReparentResult Detach(ECS::EntityHandle child,
                                            ReparentMode mode = ReparentMode::KeepLocal);

        /** @brief Resolve then snapshot world transforms as the previous simulation state. */
        TransformResolveStats BeginSimulationFrame();
        /** @brief Resolve local and parent inputs into SimulationWorldTransform. */
        TransformResolveStats ResolveSimulationTransforms();
        /** @brief Copy the current simulation result into render-facing snapshot fragments. */
        uint32 SynchronizeRenderWorldTransforms();

    private:
        enum class ResolveState : uint8
        {
            Unvisited = 0,
            Visiting,
            Resolved,
            Failed,
        };

        [[nodiscard]] bool WouldCreateCycle(ECS::EntityHandle child,
                                            ECS::EntityHandle parent) const;
        ResolveState ResolveOne(ECS::EntityHandle entity,
                                std::vector<ECS::EntityHandle>& stack,
                                std::unordered_map<ECS::EntityHandle, ResolveState>& states,
                                TransformResolveStats& stats);

        ECS::Registry* m_registry = nullptr;
        uint64 m_nextSimulationWorldRevision = 1;
    };
} // namespace RVX::SceneECS
