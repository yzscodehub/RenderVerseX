#pragma once

/**
 * @file Services.h
 * @brief Global service locator for engine services
 * 
 * Provides centralized access to engine subsystems without
 * requiring direct coupling between modules.
 * 
 * Usage:
 * @code
 * // Register a service (typically done by the subsystem itself)
 * Services::Register<RenderSubsystem>(renderSubsystem);
 * 
 * // Access a service from anywhere
 * auto* render = Services::Get<RenderSubsystem>();
 * if (render) {
 *     render->DoSomething();
 * }
 * 
 * // Or with null check
 * if (auto* input = Services::Get<InputSubsystem>()) {
 *     input->ProcessInput();
 * }
 * @endcode
 */

#include "Core/Assert.h"
#include <typeindex>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace RVX
{
    /**
     * @brief Global service locator
     * 
     * The Services class provides a centralized registry for engine services.
     * This enables loose coupling between modules while still allowing
     * global access to commonly needed services.
     * 
     * Guidelines:
     * - Register services during subsystem initialization
     * - Unregister services during subsystem shutdown
     * - Always null-check returned pointers
     * - Prefer constructor injection over Services::Get when possible
     */
    class Services
    {
    public:
        /**
         * @brief Register a service
         * @tparam T Service type
         * @param service Pointer to service instance (must remain valid)
         */
        template<typename T>
        static void Register(T* service)
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            GetRegistry()[std::type_index(typeid(T))] = service;
        }

        /**
         * @brief Unregister a service
         * @tparam T Service type
         */
        template<typename T>
        static void Unregister()
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            GetRegistry().erase(std::type_index(typeid(T)));
        }

        /**
         * @brief Get a registered service
         * @tparam T Service type
         * @return Pointer to service or nullptr if not registered
         */
        template<typename T>
        static T* Get()
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            auto& registry = GetRegistry();
            auto it = registry.find(std::type_index(typeid(T)));
            if (it != registry.end())
            {
                return static_cast<T*>(it->second);
            }
            return nullptr;
        }

        /**
         * @brief Check if a service is registered
         */
        template<typename T>
        static bool Has()
        {
            return Get<T>() != nullptr;
        }

        /**
         * @brief Get a service, asserting it exists
         * @tparam T Service type
         * @return Reference to service (asserts if not found)
         */
        template<typename T>
        static T& Require()
        {
            T* service = Get<T>();
            RVX_ASSERT_MSG(service != nullptr, "Required service not registered: {}", typeid(T).name());
            return *service;
        }

        /**
         * @brief Clear all registered services
         */
        static void Clear()
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            GetRegistry().clear();
        }

        /**
         * @brief Get the number of registered services
         */
        static size_t GetCount()
        {
            std::lock_guard<std::mutex> lock(GetMutex());
            return GetRegistry().size();
        }

    private:
        Services() = delete;

        static std::unordered_map<std::type_index, void*>& GetRegistry()
        {
            static std::unordered_map<std::type_index, void*> registry;
            return registry;
        }

        static std::mutex& GetMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
    };

    /**
     * @brief RAII helper for service registration
     * 
     * Automatically registers service on construction and
     * unregisters on destruction.
     */
    template<typename T>
    class ServiceRegistration
    {
    public:
        explicit ServiceRegistration(T* service) : m_service(service)
        {
            if (m_service)
            {
                Services::Register<T>(m_service);
            }
        }

        ~ServiceRegistration()
        {
            if (m_service)
            {
                Services::Unregister<T>();
            }
        }

        // Non-copyable
        ServiceRegistration(const ServiceRegistration&) = delete;
        ServiceRegistration& operator=(const ServiceRegistration&) = delete;

        // Movable
        ServiceRegistration(ServiceRegistration&& other) noexcept
            : m_service(other.m_service)
        {
            other.m_service = nullptr;
        }

        ServiceRegistration& operator=(ServiceRegistration&& other) noexcept
        {
            if (this != &other)
            {
                if (m_service)
                {
                    Services::Unregister<T>();
                }
                m_service = other.m_service;
                other.m_service = nullptr;
            }
            return *this;
        }

    private:
        T* m_service;
    };

} // namespace RVX
