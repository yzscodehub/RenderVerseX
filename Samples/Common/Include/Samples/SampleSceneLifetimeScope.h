#pragma once

/**
 * @file SampleSceneLifetimeScope.h
 * @brief Exact-generation lifetime scope for procedural pure ECS sample entities.
 */

#include "Scene/ECS/SceneEcsRuntime.h"

#include <cstddef>
#include <span>
#include <vector>

namespace RVX
{
    /** @brief Value diagnostics for one sample-owned ECS entity lifetime scope. */
    struct SampleSceneLifetimeDiagnostics
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        size_t ownedEntityCount = 0;
        size_t aliveEntityCount = 0;
        size_t pendingDestroyEntityCount = 0;
        size_t cleanupRequiredEntityCount = 0;
        size_t retiringEntityCount = 0;
        size_t recyclableEntityCount = 0;
        size_t recycledEntityCount = 0;
        size_t rejectedEntityCount = 0;
        size_t ownedEntityStorageCapacity = 0;
    };

    /**
     * @brief Owns value-only refs for one runtime and proves recycle before completion.
     *
     * Creation commits through SceneEcsRuntime's atomic spawn transaction. Teardown requests
     * the exact owned generations as one batch, then retains them through every lifecycle phase
     * until the Registry has recycled those generations.
     */
    class SampleSceneLifetimeScope final
    {
    public:
        explicit SampleSceneLifetimeScope(SceneECS::SceneEcsRuntime& runtime) noexcept;

        SampleSceneLifetimeScope(const SampleSceneLifetimeScope&) = delete;
        SampleSceneLifetimeScope& operator=(const SampleSceneLifetimeScope&) = delete;

        /** @brief Atomically create and adopt one baseline ECS entity. */
        [[nodiscard]] SceneECS::SceneEntityRef CreateAndAdopt(
            const SceneECS::RuntimeEntityDesc& desc = {});
        /** @brief Atomically create one entity with all authored data Fragments. */
        template<ECS::Fragment... FragmentTypes>
        [[nodiscard]] SceneECS::SceneEntityRef CreateAndAdoptWithFragments(
            const SceneECS::RuntimeEntityDesc& desc,
            FragmentTypes... fragments)
        {
            if (!ReserveForAdoption(1u))
            {
                RecordRejectedEntity();
                RefreshDiagnostics();
                return {};
            }

            SceneECS::SceneSpawnTransaction transaction =
                m_runtime.BeginSpawnTransaction();
            const SceneECS::SceneSpawnEntityId pending =
                transaction.Create(desc);
            if (!pending.IsValid() ||
                !(transaction.Add<FragmentTypes>(pending, fragments) && ...))
            {
                RecordRejectedEntity();
                RefreshDiagnostics();
                return {};
            }

            const SceneECS::SceneSpawnCommitResult result =
                transaction.Commit();
            const SceneECS::SceneEntityRef entity =
                result.GetEntityRef(pending);
            if (!result.IsApplied() || !IsOwnedAlive(entity) ||
                !InsertValidatedEntities(std::span(&entity, 1u)))
            {
                const ECS::EntityHandle handle = result.GetEntity(pending);
                if (handle.IsValid())
                {
                    RequestDestroyUnowned(std::span(&handle, 1u));
                }
                RecordRejectedEntity();
                RefreshDiagnostics();
                return {};
            }

            RefreshDiagnostics();
            return entity;
        }
        /** @brief Atomically create and adopt a complete baseline ECS entity batch. */
        [[nodiscard]] bool CreateAndAdoptBatch(
            std::span<const SceneECS::RuntimeEntityDesc> descs,
            std::vector<SceneECS::SceneEntityRef>& outRefs);

        /** @brief Adopt one Alive entity from this exact runtime and generation. */
        [[nodiscard]] bool Adopt(SceneECS::SceneEntityRef entity);
        /** @brief Atomically adopt unique Alive entities from this exact runtime. */
        [[nodiscard]] bool AdoptBatch(std::span<const SceneECS::SceneEntityRef> entities);

        /** @brief Preallocate contiguous ownership storage before a future spawn transaction. */
        [[nodiscard]] bool ReserveOwnershipCapacity(size_t entityCount);

        /**
         * @brief Idempotently request destruction for every currently Alive owned generation.
         *
         * Completion is never claimed here: Collect() removes an owned ref only after its exact
         * generation has been recycled from the Registry.
         */
        [[nodiscard]] bool RequestDestroyAll();
        /** @brief Observe lifecycle phases and retire only already recycled exact generations. */
        void Collect();

        [[nodiscard]] const SampleSceneLifetimeDiagnostics& GetDiagnostics() const
        {
            return m_diagnostics;
        }
        [[nodiscard]] bool HasUnresolvedDestroyWork() const noexcept;

    private:
        [[nodiscard]] bool IsOwnedAlive(SceneECS::SceneEntityRef entity) const;
        [[nodiscard]] bool IsAlreadyOwned(ECS::EntityHandle entity) const noexcept;
        [[nodiscard]] bool ReserveForAdoption(size_t count);
        [[nodiscard]] bool InsertValidatedEntities(
            std::span<const SceneECS::SceneEntityRef> entities) noexcept;
        void RequestDestroyUnowned(std::span<const ECS::EntityHandle> entities) noexcept;
        void RecordRejectedEntity() noexcept;
        void RefreshDiagnostics();

        SceneECS::SceneEcsRuntime& m_runtime;
        std::vector<SceneECS::SceneEntityRef> m_ownedEntities;
        SampleSceneLifetimeDiagnostics m_diagnostics;
        size_t m_recycledEntityCount = 0;
        size_t m_rejectedEntityCount = 0;
    };
} // namespace RVX
