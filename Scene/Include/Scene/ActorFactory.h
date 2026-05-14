#pragma once

/**
 * @file ActorFactory.h
 * @brief Registry for constructing Actor classes by name
 */

#include "Scene/Actor.h"

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace RVX
{
    /**
     * @brief Simple string registry for actor class construction.
     */
    class ActorFactory
    {
    public:
        using Creator = std::function<std::unique_ptr<Actor>()>;

        // =====================================================================
        // Registration
        // =====================================================================

        template<typename T>
        static void Register(const std::string& className);

        static void Register(const std::string& className, Creator creator);
        static void Unregister(const std::string& className);
        static void ClearAll();

        // =====================================================================
        // Creation
        // =====================================================================

        static std::unique_ptr<Actor> Create(const std::string& className);

        // =====================================================================
        // Query
        // =====================================================================

        static bool IsRegistered(const std::string& className);
        static std::vector<std::string> GetRegisteredClassNames();

    private:
        static std::unordered_map<std::string, Creator>& GetCreators();
    };

    template<typename T>
    void ActorFactory::Register(const std::string& className)
    {
        static_assert(std::is_base_of_v<Actor, T>, "T must derive from Actor");

        Register(className, []() {
            return std::make_unique<T>();
        });
    }

} // namespace RVX
