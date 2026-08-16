/** @file SampleSceneLifetimeScope.cpp @brief Pure ECS procedural entity lifetime scope. */

#include "Samples/SampleSceneLifetimeScope.h"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace RVX
{
    static_assert(std::is_nothrow_copy_constructible_v<SceneECS::SceneEntityRef>);
    static_assert(std::is_nothrow_copy_constructible_v<ECS::EntityHandle>);

    SampleSceneLifetimeScope::SampleSceneLifetimeScope(
        SceneECS::SceneEcsRuntime& runtime) noexcept
        : m_runtime(runtime)
    {
        RefreshDiagnostics();
    }

    SceneECS::SceneEntityRef SampleSceneLifetimeScope::CreateAndAdopt(
        const SceneECS::RuntimeEntityDesc& desc)
    {
        std::vector<SceneECS::SceneEntityRef> created;
        if (!CreateAndAdoptBatch(std::span(&desc, 1u), created) || created.size() != 1u)
        {
            return {};
        }
        return created.front();
    }

    bool SampleSceneLifetimeScope::CreateAndAdoptBatch(
        std::span<const SceneECS::RuntimeEntityDesc> descs,
        std::vector<SceneECS::SceneEntityRef>& outRefs)
    {
        if (descs.empty())
        {
            return true;
        }
        if (!ReserveForAdoption(descs.size()))
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        std::vector<SceneECS::SceneEntityRef> created;
        std::vector<ECS::EntityHandle> createdHandles;
        std::vector<SceneECS::SceneSpawnEntityId> pending;
        try
        {
            if (descs.size() > outRefs.max_size() - outRefs.size())
            {
                RecordRejectedEntity();
                RefreshDiagnostics();
                return false;
            }

            created.reserve(descs.size());
            createdHandles.reserve(descs.size());
            pending.reserve(descs.size());
            outRefs.reserve(outRefs.size() + descs.size());
        }
        catch (...)
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        SceneECS::SceneSpawnTransaction transaction = m_runtime.BeginSpawnTransaction();
        try
        {
            for (const SceneECS::RuntimeEntityDesc& desc : descs)
            {
                const SceneECS::SceneSpawnEntityId entity = transaction.Create(desc);
                if (!entity.IsValid())
                {
                    RecordRejectedEntity();
                    RefreshDiagnostics();
                    return false;
                }
                pending.push_back(entity);
            }
        }
        catch (...)
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        const SceneECS::SceneSpawnCommitResult result = transaction.Commit();
        if (!result.IsApplied())
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        for (const SceneECS::SceneSpawnEntityId pendingEntity : pending)
        {
            const SceneECS::SceneEntityRef entity = result.GetEntityRef(pendingEntity);
            const ECS::EntityHandle handle = result.GetEntity(pendingEntity);
            // Capacity and the value-copy contracts were established before Commit().
            created.push_back(entity);
            createdHandles.push_back(handle);
        }

        const bool allCreatedEntitiesAreAlive = std::all_of(
            created.begin(),
            created.end(),
            [this](SceneECS::SceneEntityRef entity) { return IsOwnedAlive(entity); });
        const bool allCreatedHandlesAreValid = std::all_of(
            createdHandles.begin(),
            createdHandles.end(),
            [](ECS::EntityHandle entity) { return entity.IsValid(); });
        if (!allCreatedEntitiesAreAlive || !allCreatedHandlesAreValid)
        {
            RequestDestroyUnowned(createdHandles);
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        if (!InsertValidatedEntities(created))
        {
            RequestDestroyUnowned(createdHandles);
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        for (const SceneECS::SceneEntityRef entity : created)
        {
            // The caller output capacity was reserved before Commit().
            outRefs.push_back(entity);
        }
        RefreshDiagnostics();
        return true;
    }

    bool SampleSceneLifetimeScope::Adopt(SceneECS::SceneEntityRef entity)
    {
        return AdoptBatch(std::span(&entity, 1u));
    }

    bool SampleSceneLifetimeScope::AdoptBatch(
        std::span<const SceneECS::SceneEntityRef> entities)
    {
        if (entities.empty())
        {
            return true;
        }

        try
        {
            for (size_t index = 0; index < entities.size(); ++index)
            {
                const SceneECS::SceneEntityRef entity = entities[index];
                const bool duplicateInBatch = std::any_of(
                    entities.begin(),
                    entities.begin() + index,
                    [entity](SceneECS::SceneEntityRef candidate)
                    { return candidate.entity == entity.entity; });
                if (!IsOwnedAlive(entity) || IsAlreadyOwned(entity.entity) || duplicateInBatch)
                {
                    RecordRejectedEntity();
                    RefreshDiagnostics();
                    return false;
                }
            }
        }
        catch (...)
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        if (!ReserveForAdoption(entities.size()) || !InsertValidatedEntities(entities))
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        RefreshDiagnostics();
        return true;
    }

    bool SampleSceneLifetimeScope::ReserveOwnershipCapacity(size_t entityCount)
    {
        if (entityCount > m_ownedEntities.max_size())
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        try
        {
            m_ownedEntities.reserve(entityCount);
            RefreshDiagnostics();
            return true;
        }
        catch (...)
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }
    }

    bool SampleSceneLifetimeScope::RequestDestroyAll()
    {
        Collect();

        std::vector<ECS::EntityHandle> alive;
        try
        {
            alive.reserve(m_ownedEntities.size());
            for (const SceneECS::SceneEntityRef entity : m_ownedEntities)
            {
                const SceneECS::EntityLifecycleState* lifecycle =
                    m_runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(entity.entity);
                if (lifecycle != nullptr && lifecycle->phase == SceneECS::EntityLifecyclePhase::Alive)
                {
                    alive.push_back(entity.entity);
                }
            }
        }
        catch (...)
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        if (!alive.empty() &&
            m_runtime.RequestDestroyBatch(
                alive,
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)) !=
                SceneECS::DestroyRequestResult::Accepted)
        {
            RecordRejectedEntity();
            RefreshDiagnostics();
            return false;
        }

        RefreshDiagnostics();
        return true;
    }

    void SampleSceneLifetimeScope::Collect()
    {
        const auto retainedBegin = std::remove_if(
            m_ownedEntities.begin(),
            m_ownedEntities.end(),
            [this](SceneECS::SceneEntityRef entity)
            {
                const SceneECS::SceneEntityRef current = m_runtime.GetEntityRef(entity.entity);
                if (current == entity)
                {
                    return false;
                }
                if (m_recycledEntityCount != std::numeric_limits<size_t>::max())
                {
                    ++m_recycledEntityCount;
                }
                return true;
            });
        m_ownedEntities.erase(retainedBegin, m_ownedEntities.end());
        RefreshDiagnostics();
    }

    bool SampleSceneLifetimeScope::HasUnresolvedDestroyWork() const noexcept
    {
        return !m_ownedEntities.empty();
    }

    bool SampleSceneLifetimeScope::IsAlreadyOwned(ECS::EntityHandle entity) const noexcept
    {
        return std::any_of(
            m_ownedEntities.begin(),
            m_ownedEntities.end(),
            [entity](SceneECS::SceneEntityRef candidate) { return candidate.entity == entity; });
    }

    bool SampleSceneLifetimeScope::IsOwnedAlive(SceneECS::SceneEntityRef entity) const
    {
        if (!entity.IsValid() || entity.sceneRuntimeId != m_runtime.GetSceneRuntimeId() ||
            m_runtime.GetEntityRef(entity.entity) != entity)
        {
            return false;
        }

        const SceneECS::EntityLifecycleState* lifecycle =
            m_runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(entity.entity);
        return lifecycle != nullptr && lifecycle->phase == SceneECS::EntityLifecyclePhase::Alive;
    }

    bool SampleSceneLifetimeScope::ReserveForAdoption(size_t count)
    {
        if (count > m_ownedEntities.max_size() - m_ownedEntities.size())
        {
            return false;
        }

        try
        {
            m_ownedEntities.reserve(m_ownedEntities.size() + count);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool SampleSceneLifetimeScope::InsertValidatedEntities(
        std::span<const SceneECS::SceneEntityRef> entities) noexcept
    {
        if (entities.size() > m_ownedEntities.capacity() - m_ownedEntities.size())
        {
            return false;
        }

        for (size_t index = 0; index < entities.size(); ++index)
        {
            const SceneECS::SceneEntityRef entity = entities[index];
            const bool duplicateInBatch = std::any_of(
                entities.begin(),
                entities.begin() + index,
                [entity](SceneECS::SceneEntityRef candidate)
                { return candidate.entity == entity.entity; });
            if (IsAlreadyOwned(entity.entity) || duplicateInBatch)
            {
                return false;
            }
        }

        for (const SceneECS::SceneEntityRef entity : entities)
        {
            m_ownedEntities.push_back(entity);
        }
        return true;
    }

    void SampleSceneLifetimeScope::RequestDestroyUnowned(
        std::span<const ECS::EntityHandle> entities) noexcept
    {
        if (!entities.empty())
        {
            static_cast<void>(m_runtime.RequestDestroyBatch(
                entities,
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)));
        }
    }

    void SampleSceneLifetimeScope::RecordRejectedEntity() noexcept
    {
        if (m_rejectedEntityCount != std::numeric_limits<size_t>::max())
        {
            ++m_rejectedEntityCount;
        }
    }

    void SampleSceneLifetimeScope::RefreshDiagnostics()
    {
        SampleSceneLifetimeDiagnostics diagnostics;
        diagnostics.sceneRuntimeId = m_runtime.GetSceneRuntimeId();
        diagnostics.ownedEntityCount = m_ownedEntities.size();
        diagnostics.ownedEntityStorageCapacity = m_ownedEntities.capacity();
        diagnostics.recycledEntityCount = m_recycledEntityCount;
        diagnostics.rejectedEntityCount = m_rejectedEntityCount;
        for (const SceneECS::SceneEntityRef entity : m_ownedEntities)
        {
            const SceneECS::EntityLifecycleState* lifecycle =
                m_runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(entity.entity);
            if (lifecycle == nullptr)
            {
                continue;
            }

            switch (lifecycle->phase)
            {
            case SceneECS::EntityLifecyclePhase::Alive:
                ++diagnostics.aliveEntityCount;
                break;
            case SceneECS::EntityLifecyclePhase::PendingDestroy:
                ++diagnostics.pendingDestroyEntityCount;
                break;
            case SceneECS::EntityLifecyclePhase::CleanupRequired:
                ++diagnostics.cleanupRequiredEntityCount;
                break;
            case SceneECS::EntityLifecyclePhase::Retiring:
                ++diagnostics.retiringEntityCount;
                break;
            case SceneECS::EntityLifecyclePhase::Recyclable:
                ++diagnostics.recyclableEntityCount;
                break;
            }
        }
        m_diagnostics = diagnostics;
    }
} // namespace RVX
