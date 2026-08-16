#include "ECS/Registry.h"

#include "ECS/Commands.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace RVX::ECS
{
    namespace
    {
        std::atomic<uint64> s_nextSceneRuntimeId = 1;
    }

    Registry::Registry(uint32 structuralJournalCapacity)
        : m_sceneRuntimeId(s_nextSceneRuntimeId.fetch_add(1, std::memory_order_relaxed))
        , m_journal(structuralJournalCapacity)
    {
    }

    Registry::~Registry()
    {
        // EntityRef/Query/EntityTransaction use this token before touching the
        // non-owning Registry address. Registry access itself remains confined
        // to its owner thread; concurrent destruction is not supported.
        m_lifetime->alive.store(false, std::memory_order_release);
        m_lifetime.reset();
    }

    bool Registry::IsOwnerThread() const
    {
        return m_lifetime != nullptr && m_lifetime->ownerThread == std::this_thread::get_id();
    }

    Registry::StructuralMutationGuard Registry::AcquireStructuralMutationGuard()
    {
        AssertOwnerThread();
        return StructuralMutationGuard(*this);
    }

    Registry::RegistryState::RegistryState(const RegistryState& other)
        : slots(other.slots)
        , freeIndices(other.freeIndices)
        , fragmentTypeIds(other.fragmentTypeIds)
        , nextFragmentTypeId(other.nextFragmentTypeId)
    {
        pools.reserve(other.pools.size());
        for (const auto& [type, pool] : other.pools)
        {
            pools.emplace(type, pool->Clone());
        }
    }

    void Registry::RegistryState::Swap(RegistryState& other) noexcept
    {
        slots.swap(other.slots);
        freeIndices.swap(other.freeIndices);
        pools.swap(other.pools);
        fragmentTypeIds.swap(other.fragmentTypeIds);
        std::swap(nextFragmentTypeId, other.nextFragmentTypeId);
    }

    Registry::Registry(const Registry& source, StagingTag)
        : m_sceneRuntimeId(source.m_sceneRuntimeId)
        , m_state(source.m_state)
        , m_journal(source.m_journal)
        , m_nextWriteVersion(source.m_nextWriteVersion)
    {
    }

    EntityHandle Registry::CreateEntity()
    {
        AssertOwnerThread();
        if (!CanMutateStructure())
        {
            return EntityHandle::Invalid();
        }

        try
        {
            return CreateEntityImmediate();
        }
        catch (...)
        {
            return EntityHandle::Invalid();
        }
    }

    bool Registry::DestroyEntity(EntityHandle entity)
    {
        AssertOwnerThread();
        if (!CanMutateStructure())
        {
            return false;
        }

        try
        {
            return DestroyEntityImmediate(entity);
        }
        catch (...)
        {
            return false;
        }
    }

    bool Registry::IsAlive(EntityHandle entity) const
    {
        AssertOwnerThread();
        return IsAliveInState(entity);
    }

    bool Registry::IsEnabled(EntityHandle entity) const
    {
        AssertOwnerThread();
        return IsAliveInState(entity) && m_state.slots[entity.GetIndex()].enabled;
    }

    bool Registry::SetEnabled(EntityHandle entity, bool enabled)
    {
        AssertOwnerThread();
        if (!CanMutateStructure())
        {
            return false;
        }

        try
        {
            return SetEnabledImmediate(entity, enabled);
        }
        catch (...)
        {
            return false;
        }
    }

    EntityRef Registry::GetRef(EntityHandle entity)
    {
        AssertOwnerThread();
        return IsAlive(entity) ? EntityRef(this, m_lifetime, m_sceneRuntimeId, entity) : EntityRef{};
    }

    EntityCommandBuffer Registry::CreateCommandBuffer()
    {
        AssertOwnerThread();
        return EntityCommandBuffer(m_sceneRuntimeId, m_lifetime->ownerThread);
    }

    EntityTransaction Registry::BeginTransaction()
    {
        AssertOwnerThread();
        return EntityTransaction(*this);
    }

    void Registry::TrimStructuralJournalBefore(uint64 sequence)
    {
        AssertOwnerThread();
        m_journal.TrimBefore(sequence);
    }

    uint32 Registry::GetEntityCount() const
    {
        AssertOwnerThread();
        return static_cast<uint32>(m_state.slots.size() - m_state.freeIndices.size());
    }

    EntityHandle Registry::CreateEntityImmediate()
    {
        // Reserve every potentially allocating side structure before marking a
        // slot alive. AppendPrepared is noexcept after this reserve, so a
        // failed allocation cannot leave an unjournaled live entity behind.
        m_journal.ReserveForAppend();
        if (!m_state.freeIndices.empty())
        {
            const uint32 index = m_state.freeIndices.back();
            EntitySlot& slot = m_state.slots[index];
            slot.alive = true;
            slot.enabled = true;
            m_state.freeIndices.pop_back();

            const EntityHandle entity = EntityHandle::Create(index, slot.generation);
            m_journal.AppendPrepared(StructuralChangeKind::EntityCreated, entity, std::type_index(typeid(void)));
            return entity;
        }

        if (m_state.slots.size() >= std::numeric_limits<uint32>::max())
        {
            return EntityHandle::Invalid();
        }

        const uint32 index = static_cast<uint32>(m_state.slots.size());
        m_state.slots.push_back({.generation = 0, .alive = false, .enabled = false});
        EntitySlot& slot = m_state.slots.back();
        slot.alive = true;
        slot.enabled = true;
        const EntityHandle entity = EntityHandle::Create(index, 0);
        m_journal.AppendPrepared(StructuralChangeKind::EntityCreated, entity, std::type_index(typeid(void)));
        return entity;
    }

    bool Registry::DestroyEntityImmediate(EntityHandle entity)
    {
        if (!IsAliveInState(entity))
        {
            return false;
        }

        struct PoolRemoval
        {
            FragmentTypeId typeId = RVX_INVALID_FRAGMENT_TYPE_ID;
            std::type_index type = std::type_index(typeid(void));
            IFragmentPool* pool = nullptr;
        };

        std::vector<PoolRemoval> removals;
        removals.reserve(m_state.pools.size());
        for (const auto& [type, pool] : m_state.pools)
        {
            if (pool->Contains(entity))
            {
                const auto idFound = m_state.fragmentTypeIds.find(type);
                if (idFound == m_state.fragmentTypeIds.end())
                {
                    return false;
                }
                removals.push_back({.typeId = idFound->second, .type = type, .pool = pool.get()});
            }
        }

        std::sort(removals.begin(), removals.end(), [](const PoolRemoval& left, const PoolRemoval& right)
        {
            return left.typeId < right.typeId;
        });

        m_journal.ReserveForAppend(static_cast<uint32>(removals.size()) + 1u);
        if (m_state.freeIndices.size() == m_state.freeIndices.capacity())
        {
            const size_t currentCapacity = m_state.freeIndices.capacity();
            const size_t requiredCapacity = m_state.freeIndices.size() + 1u;
            const size_t geometricCapacity = currentCapacity == 0 ? 8u : currentCapacity + currentCapacity / 2u;
            m_state.freeIndices.reserve(std::max(requiredCapacity, geometricCapacity));
        }

        for (const PoolRemoval& removal : removals)
        {
            if (removal.pool->Remove(entity))
            {
                m_journal.AppendPrepared(StructuralChangeKind::FragmentRemoved,
                                        entity,
                                        removal.type,
                                        removal.typeId);
            }
        }

        EntitySlot& slot = m_state.slots[entity.GetIndex()];
        slot.alive = false;
        slot.enabled = false;
        ++slot.generation;
        m_state.freeIndices.push_back(entity.GetIndex());
        m_journal.AppendPrepared(StructuralChangeKind::EntityDestroyed, entity, std::type_index(typeid(void)));
        return true;
    }

    bool Registry::SetEnabledImmediate(EntityHandle entity, bool enabled)
    {
        if (!IsAliveInState(entity))
        {
            return false;
        }

        EntitySlot& slot = m_state.slots[entity.GetIndex()];
        if (slot.enabled == enabled)
        {
            return false;
        }

        m_journal.ReserveForAppend();
        slot.enabled = enabled;
        m_journal.AppendPrepared(enabled ? StructuralChangeKind::EntityEnabled : StructuralChangeKind::EntityDisabled,
                                entity,
                                std::type_index(typeid(void)));
        return true;
    }

    bool Registry::IsAliveInState(EntityHandle entity) const
    {
        if (!entity.IsValid() || entity.GetIndex() >= m_state.slots.size())
        {
            return false;
        }

        const EntitySlot& slot = m_state.slots[entity.GetIndex()];
        return slot.alive && slot.generation == entity.GetGeneration();
    }

    void Registry::BeginIteration()
    {
        AssertOwnerThread();
        ++m_iterationDepth;
    }

    void Registry::EndIteration()
    {
        AssertOwnerThread();
        --m_iterationDepth;
    }

    void Registry::AssertOwnerThread() const
    {
        RVX_DEBUG_ASSERT_MSG(IsOwnerThread(), "Registry access must remain on its owner thread");
    }

    bool EntityRef::IsValid() const
    {
        const std::shared_ptr<Detail::RegistryLifetime> lifetime = m_lifetime.lock();
        return lifetime != nullptr && lifetime->alive.load(std::memory_order_acquire) &&
               m_registry != nullptr && m_sceneRuntimeId == m_registry->GetSceneRuntimeId() &&
               m_registry->IsAlive(m_handle);
    }

    bool EntityRef::SetEnabled(bool enabled)
    {
        return IsValid() && m_registry->SetEnabled(m_handle, enabled);
    }
} // namespace RVX::ECS
