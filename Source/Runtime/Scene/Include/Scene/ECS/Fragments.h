#pragma once

/**
 * @file Fragments.h
 * @brief Data-only Scene-domain fragments for the runtime ECS.
 */

#include "Core/MathTypes.h"
#include "ECS/Entity.h"
#include "ECS/Fragment.h"

namespace RVX::SceneECS
{
    /** @brief Entity-local transform expressed as translation, rotation, and scale. */
    struct LocalTransform
    {
        Vec3 translation{0.0f};
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 scale{1.0f};
        uint64 revision = 1;
    };

    /** @brief Authoritative parent edge. An invalid handle denotes a hierarchy root. */
    struct ParentRelation
    {
        ECS::EntityHandle parent = ECS::EntityHandle::Invalid();
    };

    /** @brief Authoritative transform resolved for the current simulation step. */
    struct SimulationWorldTransform
    {
        Mat4 matrix{1.0f};
        uint64 revision = 0;
        uint64 sourceLocalRevision = 0;
        uint64 sourceLocalWriteVersion = 0;
        uint64 sourceParentWriteVersion = 0;
        uint64 sourceParentWorldRevision = 0;
        ECS::EntityHandle resolvedParent = ECS::EntityHandle::Invalid();
    };

    /** @brief Simulation-world transform from the immediately preceding simulation step. */
    struct PreviousSimulationWorldTransform
    {
        Mat4 matrix{1.0f};
        uint64 sourceRevision = 0;
    };

    /** @brief Render-facing transform snapshot copied from simulation at extraction time. */
    struct RenderWorldTransform
    {
        Mat4 matrix{1.0f};
        uint64 sourceRevision = 0;
    };

    /** @brief Axis-aligned bounds in the coordinate space selected by its consumer. */
    struct Bounds
    {
        Vec3 center{0.0f};
        Vec3 extents{0.0f};
    };

    /** @brief Gameplay-facing active state, independent from Registry iteration enablement. */
    struct Active
    {
        bool value = true;
    };

    /** @brief Application-defined visibility or simulation layer. */
    struct Layer
    {
        uint32 value = 0;
    };

    /** @brief Marks an entity with a Particle-owned Feature-phase side table. */
    struct ParticleRuntimeTag {};
    /** @brief Marks an entity with a Water-owned Feature-phase side table. */
    struct WaterRuntimeTag {};
    /** @brief Marks an entity with a Terrain-owned Feature-phase side table. */
    struct TerrainRuntimeTag {};

    /** @brief Independent cleanup consumer domains that must acknowledge retirement. */
    enum class CleanupDomain : uint32
    {
        None = 0,
        Physics = 1u << 0u,
        Animation = 1u << 1u,
        Audio = 1u << 2u,
        ParticleFeature = 1u << 3u,
        WaterFeature = 1u << 4u,
        TerrainFeature = 1u << 5u,
        Script = 1u << 6u,
        Render = 1u << 7u,
        Resources = 1u << 8u,
        Simulation = (1u << 0u) | (1u << 1u) | (1u << 2u) |
                     (1u << 3u) | (1u << 4u) | (1u << 5u) | (1u << 6u),
        All = (1u << 0u) | (1u << 1u) | (1u << 2u) |
              (1u << 3u) | (1u << 4u) | (1u << 5u) | (1u << 6u) |
              (1u << 7u) | (1u << 8u),
    };

    using CleanupDomainMask = uint32;

    [[nodiscard]] constexpr CleanupDomainMask ToCleanupDomainMask(CleanupDomain domain)
    {
        return static_cast<CleanupDomainMask>(domain);
    }

    /** @brief Runtime lifecycle phase retained as data until slot recycling commits. */
    enum class EntityLifecyclePhase : uint8
    {
        Alive = 0,
        PendingDestroy,
        CleanupRequired,
        Retiring,
        Recyclable,
    };

    /** @brief Entity lifecycle state fragment. */
    struct EntityLifecycleState
    {
        EntityLifecyclePhase phase = EntityLifecyclePhase::Alive;
        CleanupDomainMask requiredCleanupDomains = ToCleanupDomainMask(CleanupDomain::None);
        CleanupDomainMask acknowledgedCleanupDomains = ToCleanupDomainMask(CleanupDomain::None);
    };

    /** @brief Why a value-owned cleanup entry was emitted. */
    enum class CleanupReason : uint8
    {
        RequestedDestroy = 0,
        RuntimeShutdown,
    };

    /**
     * @brief Value-only cleanup payload safe to retain after an entity is destroyed.
     *
     * This deliberately records only the owning runtime identity and generation-safe
     * entity handle; it never exposes object or component pointers.
     */
    struct CleanupRecord
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        uint64 sequence = 0;
        CleanupReason reason = CleanupReason::RequestedDestroy;
        CleanupDomainMask requiredCleanupDomains = ToCleanupDomainMask(CleanupDomain::None);
        CleanupDomainMask acknowledgedCleanupDomains = ToCleanupDomainMask(CleanupDomain::None);
    };

    static_assert(ECS::Fragment<LocalTransform>);
    static_assert(ECS::Fragment<ParentRelation>);
    static_assert(ECS::Fragment<SimulationWorldTransform>);
    static_assert(ECS::Fragment<PreviousSimulationWorldTransform>);
    static_assert(ECS::Fragment<RenderWorldTransform>);
    static_assert(ECS::Fragment<Bounds>);
    static_assert(ECS::Fragment<Active>);
    static_assert(ECS::Fragment<Layer>);
    static_assert(ECS::Fragment<ParticleRuntimeTag>);
    static_assert(ECS::Fragment<WaterRuntimeTag>);
    static_assert(ECS::Fragment<TerrainRuntimeTag>);
    static_assert(ECS::Fragment<EntityLifecycleState>);
} // namespace RVX::SceneECS
