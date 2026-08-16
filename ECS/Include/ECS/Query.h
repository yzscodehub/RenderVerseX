#pragma once

#include "ECS/Registry.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace RVX::ECS
{
    /**
     * @brief Membership query over typed sparse-set pools.
     *
     * Each invocation selects the least-populated required pool as its driver,
     * then verifies the remaining memberships. Disabled entities are excluded.
     * Query is a non-owning owner-thread view. If its originating Registry has
     * already been destroyed, Each returns zero without dereferencing it.
     */
    template<QueryTerm... T>
    class Query
    {
    public:
        static_assert(sizeof...(T) != 0, "A query must require at least one fragment type");
        static_assert((QueryAccess<T>::RequiresPresent || ...),
                      "A query needs at least one Read/Write/With driver fragment");

        template<typename Function>
        uint32 Each(Function&& function) const
        {
            return EachImpl(false, false, false, function);
        }

        /** @brief Iterate matching entities in ascending generation-safe handle order. */
        template<typename Function>
        uint32 EachStable(Function&& function) const
        {
            return EachImpl(false, false, true, function);
        }

        /** @brief Iterate matching entities even when their entity enable bit is false. */
        template<typename Function>
        uint32 EachIncludingDisabled(Function&& function) const
        {
            return EachImpl(true, false, false, function);
        }

        /** @brief Stable-order variant that includes disabled entities. */
        template<typename Function>
        uint32 EachStableIncludingDisabled(Function&& function) const
        {
            return EachImpl(true, false, true, function);
        }

        /** @brief Iterate disabled entities and disabled fragment membership. */
        template<typename Function>
        uint32 EachIncludingAllDisabled(Function&& function) const
        {
            return EachImpl(true, true, false, function);
        }

        /** @brief Stable-order variant that includes disabled entities and fragments. */
        template<typename Function>
        uint32 EachStableIncludingAllDisabled(Function&& function) const
        {
            return EachImpl(true, true, true, function);
        }

    private:
        friend class Registry;

        template<typename Function>
        uint32 EachImpl(bool includeDisabledEntities,
                        bool includeDisabledFragments,
                        bool stableOrder,
                        Function& function) const
        {
            const std::shared_ptr<Detail::RegistryLifetime> lifetime = m_lifetime.lock();
            if (m_registry == nullptr || lifetime == nullptr || !lifetime->alive.load(std::memory_order_acquire))
            {
                return 0;
            }

            m_registry->AssertOwnerThread();
            const IFragmentPool* driver = m_registry->FindSmallestRequiredPool<T...>();
            if (driver == nullptr)
            {
                return 0;
            }

            struct IterationScope
            {
                explicit IterationScope(Registry& registry)
                    : m_registry(registry)
                {
                    m_registry.BeginIteration();
                }

                ~IterationScope()
                {
                    m_registry.EndIteration();
                }

                Registry& m_registry;
            } iteration(*m_registry);

            const uint32 entityCount = driver->GetEntityCount();
            std::vector<EntityHandle> stableEntities;
            if (stableOrder)
            {
                stableEntities.reserve(entityCount);
            }

            uint32 visited = 0;
            for (uint32 denseIndex = 0; denseIndex < entityCount; ++denseIndex)
            {
                const EntityHandle entity = driver->GetEntityAt(denseIndex);
                if (!m_registry->IsAlive(entity) ||
                    (!includeDisabledEntities && !m_registry->IsEnabled(entity)) ||
                    !(m_registry->MatchesQueryTerm<T>(entity, includeDisabledFragments) && ...))
                {
                    continue;
                }

                if (stableOrder)
                {
                    stableEntities.push_back(entity);
                }
                else
                {
                    InvokeResolved<0>(entity, function);
                    ++visited;
                }
            }

            if (stableOrder)
            {
                std::sort(stableEntities.begin(), stableEntities.end());
                for (const EntityHandle entity : stableEntities)
                {
                    InvokeResolved<0>(entity, function);
                }
                visited = static_cast<uint32>(stableEntities.size());
            }

            return visited;
        }

        explicit Query(Registry& registry)
            : m_registry(&registry)
            , m_lifetime(registry.m_lifetime)
        {
        }

        Registry* m_registry = nullptr;
        std::weak_ptr<Detail::RegistryLifetime> m_lifetime;

        template<QueryTerm Access>
        decltype(auto) ResolveAccess(EntityHandle entity) const
        {
            using FragmentType = typename QueryAccess<Access>::FragmentType;
            if constexpr (QueryAccess<Access>::IsWritable)
            {
                return *m_registry->GetForQueryWrite<FragmentType>(entity);
            }
            else
            {
                return *m_registry->TryGet<FragmentType>(entity);
            }
        }

        template<size_t Index, typename Function, typename... Resolved>
        void InvokeResolved(EntityHandle entity,
                            Function& function,
                            Resolved&&... resolved) const
        {
            if constexpr (Index == sizeof...(T))
            {
                std::invoke(function, entity, std::forward<Resolved>(resolved)...);
            }
            else
            {
                using Term = std::tuple_element_t<Index, std::tuple<T...>>;
                if constexpr (QueryAccess<Term>::IsAccess)
                {
                    decltype(auto) access = ResolveAccess<Term>(entity);
                    InvokeResolved<Index + 1>(entity,
                                              function,
                                              std::forward<Resolved>(resolved)...,
                                              access);
                }
                else
                {
                    InvokeResolved<Index + 1>(entity,
                                              function,
                                              std::forward<Resolved>(resolved)...);
                }
            }
        }
    };

    template<QueryTerm... T>
    Query<T...> Registry::Query()
    {
        AssertOwnerThread();
        return ECS::Query<T...>(*this);
    }
} // namespace RVX::ECS
