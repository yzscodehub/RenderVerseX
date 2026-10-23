#pragma once

#include "Core/Handle.h"

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

namespace RVX::ECS
{
    class Registry;

    namespace Detail
    {
        /** @brief Shared liveness state for non-owning ECS facades. */
        struct RegistryLifetime final
        {
            std::atomic_bool alive = true;
            std::thread::id ownerThread = std::this_thread::get_id();
        };
    } // namespace Detail

    // =========================================================================
    // Entity identity
    // =========================================================================
    struct EntityHandleTag {};
    using EntityHandle = Handle<EntityHandleTag>;

    /** @brief Strong identifier for one independently running ECS registry. */
    class SceneRuntimeId
    {
    public:
        constexpr SceneRuntimeId() = default;
        explicit constexpr SceneRuntimeId(uint64 value)
            : m_value(value)
        {
        }

        [[nodiscard]] constexpr bool IsValid() const { return m_value != 0; }
        [[nodiscard]] constexpr uint64 GetValue() const { return m_value; }
        constexpr explicit operator bool() const { return IsValid(); }

        friend constexpr bool operator==(SceneRuntimeId, SceneRuntimeId) = default;

    private:
        uint64 m_value = 0;
    };

    /**
     * @brief Thin, non-owning facade over an entity in one runtime registry.
     *
     * EntityRef never owns fragment data. Fragment access is always routed to
     * Registry, retaining a single source of truth for ECS state. Its lifetime
     * token turns stale references into invalid references after Registry
     * destruction instead of dereferencing the former Registry address.
     *
     * Registry, EntityRef, Query, and command operations are owner-thread
     * APIs. They are not synchronization primitives and do not support
     * concurrent Registry destruction or MPSC command recording.
     */
    class EntityRef
    {
    public:
        constexpr EntityRef() = default;

        [[nodiscard]] bool IsValid() const;
        [[nodiscard]] SceneRuntimeId GetSceneRuntimeId() const { return m_sceneRuntimeId; }
        [[nodiscard]] EntityHandle GetHandle() const { return m_handle; }

        template<typename T>
        [[nodiscard]] bool Has() const;

        template<typename T>
        [[nodiscard]] const T* TryGet() const;

        template<typename T>
        bool Add(T value = {});

        template<typename T>
        bool Remove();

        bool SetEnabled(bool enabled);

    private:
        friend class Registry;

        EntityRef(Registry* registry,
                  std::weak_ptr<Detail::RegistryLifetime> lifetime,
                  SceneRuntimeId sceneRuntimeId,
                  EntityHandle handle)
            : m_registry(registry)
            , m_lifetime(std::move(lifetime))
            , m_sceneRuntimeId(sceneRuntimeId)
            , m_handle(handle)
        {
        }

        Registry* m_registry = nullptr;
        std::weak_ptr<Detail::RegistryLifetime> m_lifetime;
        SceneRuntimeId m_sceneRuntimeId;
        EntityHandle m_handle = EntityHandle::Invalid();
    };
} // namespace RVX::ECS
