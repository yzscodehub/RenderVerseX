#pragma once

#include "Core/Assert.h"
#include "Core/Types.h"
#include "ECS/Entity.h"
#include "ECS/Fragment.h"
#include "ECS/FragmentPool.h"
#include "ECS/StructuralJournal.h"

#include <memory>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RVX::ECS
{
    class EntityCommandBuffer;
    class EntityTransaction;

    template<QueryTerm... T>
    class Query;

    /**
     * @brief Runtime sparse-set ECS authority for one scene execution domain.
     *
     * Registry state, queries, and command recording are owner-thread-only in
     * P1. EntityHandle values are registry-local; use EntityRef when a target
     * crosses a subsystem boundary. This class is not an MPSC container.
     */
    class Registry
    {
    public:
        /**
         * @brief Scoped gate that rejects structural mutations while leaving
         * reads and Write<T> access available.
         *
         * Scene scheduling uses this while invoking processors.  The guard is
         * intentionally Registry-generic: it protects every structural entry
         * point (including transactions) without imposing Scene policy on
         * standalone ECS users. Guards are owner-thread-only and nest safely.
         */
        class StructuralMutationGuard
        {
        public:
            StructuralMutationGuard(const StructuralMutationGuard&) = delete;
            StructuralMutationGuard& operator=(const StructuralMutationGuard&) = delete;

            StructuralMutationGuard(StructuralMutationGuard&& other) noexcept
                : m_registry(std::exchange(other.m_registry, nullptr))
            {
            }

            StructuralMutationGuard& operator=(StructuralMutationGuard&& other) noexcept
            {
                if (this != &other)
                {
                    Release();
                    m_registry = std::exchange(other.m_registry, nullptr);
                }
                return *this;
            }

            ~StructuralMutationGuard() { Release(); }

        private:
            friend class Registry;

            explicit StructuralMutationGuard(Registry& registry)
                : m_registry(&registry)
            {
                ++m_registry->m_structuralMutationGuardDepth;
            }

            void Release() noexcept
            {
                if (m_registry != nullptr)
                {
                    RVX_DEBUG_ASSERT(m_registry->m_structuralMutationGuardDepth != 0);
                    --m_registry->m_structuralMutationGuardDepth;
                    m_registry = nullptr;
                }
            }

            Registry* m_registry = nullptr;
        };

        explicit Registry(uint32 structuralJournalCapacity = 8192);
        ~Registry();

        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;
        Registry(Registry&&) = delete;
        Registry& operator=(Registry&&) = delete;

        // =====================================================================
        // Entity lifetime and enable state
        // =====================================================================
        [[nodiscard]] SceneRuntimeId GetSceneRuntimeId() const { return m_sceneRuntimeId; }
        /** @brief True only on the thread that constructed this Registry. */
        [[nodiscard]] bool IsOwnerThread() const;
        /** @brief Block structural mutation until this owner-thread scope exits. */
        [[nodiscard]] StructuralMutationGuard AcquireStructuralMutationGuard();
        [[nodiscard]] EntityHandle CreateEntity();
        bool DestroyEntity(EntityHandle entity);
        [[nodiscard]] bool IsAlive(EntityHandle entity) const;
        [[nodiscard]] bool IsEnabled(EntityHandle entity) const;
        bool SetEnabled(EntityHandle entity, bool enabled);
        bool Enable(EntityHandle entity) { return SetEnabled(entity, true); }
        bool Disable(EntityHandle entity) { return SetEnabled(entity, false); }

        [[nodiscard]] EntityRef GetRef(EntityHandle entity);

        // =====================================================================
        // Fragment access
        // =====================================================================
        template<Fragment T>
        bool Add(EntityHandle entity, T value = {})
        {
            AssertOwnerThread();
            if (!CanMutateStructure())
            {
                return false;
            }

            try
            {
                return AddImmediate<T>(entity, value);
            }
            catch (...)
            {
                return false;
            }
        }

        template<Fragment T>
        bool Remove(EntityHandle entity)
        {
            AssertOwnerThread();
            if (!CanMutateStructure())
            {
                return false;
            }

            try
            {
                return RemoveImmediate<T>(entity);
            }
            catch (...)
            {
                return false;
            }
        }

        template<Fragment T>
        [[nodiscard]] bool Has(EntityHandle entity) const
        {
            AssertOwnerThread();
            const FragmentPool<T>* pool = TryGetPool<T>();
            return IsAlive(entity) && pool != nullptr && pool->Contains(entity);
        }

        template<Fragment T>
        [[nodiscard]] const T* TryGet(EntityHandle entity) const
        {
            AssertOwnerThread();
            const FragmentPool<T>* pool = TryGetPool<T>();
            return IsAlive(entity) && pool != nullptr ? pool->TryGet(entity) : nullptr;
        }

        template<Fragment T>
        [[nodiscard]] bool IsFragmentEnabled(EntityHandle entity) const
        {
            AssertOwnerThread();
            const FragmentPool<T>* pool = TryGetPool<T>();
            return IsAlive(entity) && pool != nullptr && pool->IsEnabled(entity);
        }

        template<Fragment T>
        bool SetFragmentEnabled(EntityHandle entity, bool enabled)
        {
            AssertOwnerThread();
            if (!CanMutateStructure())
            {
                return false;
            }

            try
            {
                return SetFragmentEnabledImmediate<T>(entity, enabled);
            }
            catch (...)
            {
                return false;
            }
        }

        template<Fragment T, typename Function>
        bool Write(EntityHandle entity, Function&& function)
        {
            AssertOwnerThread();
            if (!IsAlive(entity))
            {
                return false;
            }

            FragmentPool<T>* pool = TryGetPool<T>();
            if (pool == nullptr)
            {
                return false;
            }

            T* value = pool->TryGetForWrite(entity);
            if (value == nullptr)
            {
                return false;
            }

            pool->MarkWritten(entity, m_nextWriteVersion++);
            std::forward<Function>(function)(*value);
            return true;
        }

        template<Fragment T>
        [[nodiscard]] uint64 GetFragmentWriteVersion() const
        {
            AssertOwnerThread();
            const FragmentPool<T>* pool = TryGetPool<T>();
            return pool != nullptr ? pool->GetWriteVersion() : 0;
        }

        template<Fragment T>
        [[nodiscard]] uint64 GetFragmentWriteVersion(EntityHandle entity) const
        {
            AssertOwnerThread();
            const FragmentPool<T>* pool = TryGetPool<T>();
            return pool != nullptr ? pool->GetEntityWriteVersion(entity) : 0;
        }

        template<typename T>
        bool AddTag(EntityHandle entity)
        {
            return Add<Tag<T>>(entity);
        }

        template<typename T>
        [[nodiscard]] bool HasTag(EntityHandle entity) const
        {
            return Has<Tag<T>>(entity);
        }

        // =====================================================================
        // Query and command access
        // =====================================================================
        template<QueryTerm... T>
        [[nodiscard]] Query<T...> Query();

        [[nodiscard]] EntityCommandBuffer CreateCommandBuffer();
        [[nodiscard]] EntityTransaction BeginTransaction();

        // =====================================================================
        // Change tracking
        // =====================================================================
        [[nodiscard]] const StructuralJournal& GetStructuralJournal() const
        {
            AssertOwnerThread();
            return m_journal;
        }
        [[nodiscard]] StructuralJournalRead ReadStructuralChanges(StructuralJournalCursor& cursor) const
        {
            AssertOwnerThread();
            return m_journal.Read(cursor);
        }
        void TrimStructuralJournalBefore(uint64 sequence);

        [[nodiscard]] uint32 GetEntityCount() const;

    private:
        friend class EntityCommandBuffer;
        friend class EntityTransaction;
        template<QueryTerm... T>
        friend class Query;

        struct EntitySlot
        {
            uint32 generation = 0;
            bool alive = false;
            bool enabled = false;
        };

        struct RegistryState
        {
            RegistryState() = default;
            RegistryState(const RegistryState& other);
            RegistryState& operator=(const RegistryState&) = delete;
            RegistryState(RegistryState&&) noexcept = default;
            RegistryState& operator=(RegistryState&&) noexcept = default;

            void Swap(RegistryState& other) noexcept;

            std::vector<EntitySlot> slots;
            std::vector<uint32> freeIndices;
            std::unordered_map<std::type_index, std::unique_ptr<IFragmentPool>> pools;
            std::unordered_map<std::type_index, FragmentTypeId> fragmentTypeIds;
            FragmentTypeId nextFragmentTypeId = 1;
        };

        struct StagingTag {};
        Registry(const Registry& source, StagingTag);

        template<typename Function>
        bool ExecuteTransaction(Function&& operation)
        {
            AssertOwnerThread();
            if (!CanMutateStructure())
            {
                return false;
            }

            try
            {
                Registry staging(*this, StagingTag{});
                if (!operation(staging))
                {
                    return false;
                }

                m_state.Swap(staging.m_state);
                m_journal.Swap(staging.m_journal);
                std::swap(m_nextWriteVersion, staging.m_nextWriteVersion);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] EntityHandle CreateEntityImmediate();
        bool DestroyEntityImmediate(EntityHandle entity);
        bool SetEnabledImmediate(EntityHandle entity, bool enabled);
        [[nodiscard]] bool IsAliveInState(EntityHandle entity) const;

        template<Fragment T>
        bool AddImmediate(EntityHandle entity, T value)
        {
            if (!IsAliveInState(entity))
            {
                return false;
            }

            m_journal.ReserveForAppend();
            FragmentPool<T>& pool = GetOrCreatePool<T>();
            if (!pool.Add(entity, value))
            {
                return false;
            }

            m_journal.AppendPrepared(StructuralChangeKind::FragmentAdded,
                                    entity,
                                    std::type_index(typeid(T)),
                                    GetRegisteredFragmentTypeId<T>());
            return true;
        }

        template<Fragment T>
        bool RemoveImmediate(EntityHandle entity)
        {
            if (!IsAliveInState(entity))
            {
                return false;
            }

            FragmentPool<T>* pool = TryGetPool<T>();
            if (pool == nullptr || !pool->Contains(entity))
            {
                return false;
            }

            m_journal.ReserveForAppend();
            pool->Remove(entity);
            m_journal.AppendPrepared(StructuralChangeKind::FragmentRemoved,
                                    entity,
                                    std::type_index(typeid(T)),
                                    GetRegisteredFragmentTypeId<T>());
            return true;
        }

        template<Fragment T>
        bool SetFragmentEnabledImmediate(EntityHandle entity, bool enabled)
        {
            if (!IsAliveInState(entity))
            {
                return false;
            }

            FragmentPool<T>* pool = TryGetPool<T>();
            if (pool == nullptr || !pool->Contains(entity) || pool->IsEnabled(entity) == enabled)
            {
                return false;
            }

            m_journal.ReserveForAppend();
            const uint64 version = m_nextWriteVersion++;
            if (!pool->SetEnabled(entity, enabled, version))
            {
                --m_nextWriteVersion;
                return false;
            }
            m_journal.AppendPrepared(enabled ? StructuralChangeKind::FragmentEnabled :
                                               StructuralChangeKind::FragmentDisabled,
                                     entity,
                                     std::type_index(typeid(T)),
                                     GetRegisteredFragmentTypeId<T>());
            return true;
        }

        template<Fragment T>
        [[nodiscard]] FragmentPool<T>* TryGetPool()
        {
            const auto found = m_state.pools.find(std::type_index(typeid(T)));
            return found == m_state.pools.end() ? nullptr : static_cast<FragmentPool<T>*>(found->second.get());
        }

        template<Fragment T>
        [[nodiscard]] const FragmentPool<T>* TryGetPool() const
        {
            const auto found = m_state.pools.find(std::type_index(typeid(T)));
            return found == m_state.pools.end() ? nullptr : static_cast<const FragmentPool<T>*>(found->second.get());
        }

        template<Fragment T>
        [[nodiscard]] FragmentPool<T>& GetOrCreatePool()
        {
            const std::type_index type = std::type_index(typeid(T));
            const auto found = m_state.pools.find(type);
            if (found != m_state.pools.end())
            {
                return *static_cast<FragmentPool<T>*>(found->second.get());
            }

            const auto [idFound, idInserted] = m_state.fragmentTypeIds.emplace(type, m_state.nextFragmentTypeId);
            try
            {
                auto pool = std::make_unique<FragmentPool<T>>();
                FragmentPool<T>* result = pool.get();
                const auto [poolFound, poolInserted] = m_state.pools.emplace(type, std::move(pool));
                if (!poolInserted)
                {
                    if (idInserted)
                    {
                        m_state.fragmentTypeIds.erase(idFound);
                    }
                    return *static_cast<FragmentPool<T>*>(poolFound->second.get());
                }

                if (idInserted)
                {
                    ++m_state.nextFragmentTypeId;
                }
                return *result;
            }
            catch (...)
            {
                if (idInserted)
                {
                    m_state.fragmentTypeIds.erase(idFound);
                }
                throw;
            }
        }

        template<Fragment T>
        [[nodiscard]] FragmentTypeId GetRegisteredFragmentTypeId() const
        {
            const auto found = m_state.fragmentTypeIds.find(std::type_index(typeid(T)));
            return found != m_state.fragmentTypeIds.end() ? found->second : RVX_INVALID_FRAGMENT_TYPE_ID;
        }

        template<QueryTerm... T>
        [[nodiscard]] const IFragmentPool* FindSmallestRequiredPool() const
        {
            AssertOwnerThread();
            const IFragmentPool* smallest = nullptr;
            bool hasRequiredPool = false;
            const auto consider = [this, &smallest, &hasRequiredPool]<QueryTerm Term>()
            {
                if constexpr (!QueryAccess<Term>::RequiresPresent)
                {
                    return true;
                }
                else
                {
                    hasRequiredPool = true;
                    using U = typename QueryAccess<Term>::FragmentType;
                    const FragmentPool<U>* pool = TryGetPool<U>();
                    if (pool == nullptr)
                    {
                        smallest = nullptr;
                        return false;
                    }
                    if (smallest == nullptr || pool->GetEntityCount() < smallest->GetEntityCount())
                    {
                        smallest = pool;
                    }
                    return true;
                }
            };

            const bool complete = (consider.template operator()<T>() && ...);
            return complete && hasRequiredPool ? smallest : nullptr;
        }

        template<QueryTerm Term>
        [[nodiscard]] bool MatchesQueryTerm(EntityHandle entity,
                                            bool includeDisabledFragments) const
        {
            using U = typename QueryAccess<Term>::FragmentType;
            const bool enabledMembership = Has<U>(entity) &&
                                           (includeDisabledFragments ||
                                            IsFragmentEnabled<U>(entity));
            if constexpr (QueryAccess<Term>::Excludes)
            {
                return !enabledMembership;
            }
            else
            {
                return enabledMembership;
            }
        }

        template<Fragment T>
        [[nodiscard]] T* GetForQueryWrite(EntityHandle entity)
        {
            AssertOwnerThread();
            FragmentPool<T>* pool = TryGetPool<T>();
            if (!IsAlive(entity) || pool == nullptr)
            {
                return nullptr;
            }

            T* value = pool->TryGetForWrite(entity);
            if (value != nullptr)
            {
                pool->MarkWritten(entity, m_nextWriteVersion++);
            }
            return value;
        }

        void BeginIteration();
        void EndIteration();
        void AssertOwnerThread() const;
        [[nodiscard]] bool CanMutateStructure() const
        {
            return m_iterationDepth == 0 && m_structuralMutationGuardDepth == 0;
        }

        SceneRuntimeId m_sceneRuntimeId;
        std::shared_ptr<Detail::RegistryLifetime> m_lifetime = std::make_shared<Detail::RegistryLifetime>();
        RegistryState m_state;
        StructuralJournal m_journal;
        uint32 m_iterationDepth = 0;
        uint32 m_structuralMutationGuardDepth = 0;
        uint64 m_nextWriteVersion = 1;
    };

    template<typename T>
    bool EntityRef::Has() const
    {
        return IsValid() && m_registry->Has<T>(m_handle);
    }

    template<typename T>
    const T* EntityRef::TryGet() const
    {
        return IsValid() ? m_registry->TryGet<T>(m_handle) : nullptr;
    }

    template<typename T>
    bool EntityRef::Add(T value)
    {
        return IsValid() && m_registry->Add<T>(m_handle, value);
    }

    template<typename T>
    bool EntityRef::Remove()
    {
        return IsValid() && m_registry->Remove<T>(m_handle);
    }
} // namespace RVX::ECS
