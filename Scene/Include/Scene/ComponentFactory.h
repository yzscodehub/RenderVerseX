#pragma once

/**
 * @file ComponentFactory.h
 * @brief Factory for creating actor components from model Node data
 * 
 * Provides a unified way to convert Node indices to actor components
 * during ModelResource::Instantiate().
 */

#include "Scene/ActorComponent.h"
#include "Scene/Component.h"
#include "Scene/Node.h"
#include "Scene/SceneEntity.h"

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace RVX
{
    // Forward declarations
    class SceneEntity;

    namespace Resource
    {
        class ModelResource;
    }

    /**
     * @brief Factory for creating actor components from model Node data
     * 
     * ComponentFactory provides a registry-based system for converting
     * Node indices (meshIndex, materialIndices, etc.) into actor component
     * instances during model instantiation.
     * 
     * Usage:
     * @code
     * // During engine initialization
     * ComponentFactory::RegisterDefaults();
     * 
     * // During model instantiation
     * for (each node in modelResource) {
     *     SceneEntity* entity = scene->CreateEntity(node->GetName());
     *     ComponentFactory::CreateComponents(entity, node, modelResource);
     * }
     * @endcode
     */
    class ComponentFactory
    {
    public:
        /**
         * @brief Model-node component creator function signature
         * 
         * @param entity The SceneEntity to attach the component to
         * @param node The source Node containing index data
         * @param model The ModelResource containing resource arrays
         * @return Created actor component (or nullptr if not applicable)
         */
        using Creator = std::function<ActorComponent*(SceneEntity*, const Node*, const Resource::ModelResource*)>;
        using ClassCreator = std::function<std::unique_ptr<ActorComponent>()>;

        // =====================================================================
        // Registration
        // =====================================================================

        /**
         * @brief Register a component creator
         * 
         * @param typeName Unique name for this component type
         * @param creator The creator function
         */
        static void Register(const std::string& typeName, Creator creator);

        /**
         * @brief Unregister a component creator
         */
        static void Unregister(const std::string& typeName);

        /**
         * @brief Clear all registered creators
         */
        static void ClearAll();

        /**
         * @brief Register default model-node component creators.
         * 
         * Call this during engine initialization.
         */
        static void RegisterDefaults();

        /**
         * @brief Register an actor component class for generic construction
         */
        template<typename T>
        static void RegisterComponentClass(const std::string& typeName);

        /**
         * @brief Register a generic actor component creator
         */
        static void RegisterComponentClass(const std::string& typeName, ClassCreator creator);

        /**
         * @brief Clear generic actor component class creators
         */
        static void ClearComponentClasses();

        // =====================================================================
        // Component Creation
        // =====================================================================

        /**
         * @brief Create all applicable model-node actor components for an entity.
         * 
         * Iterates through all registered creators and calls each one.
         * Each creator decides whether to create a component based on
         * the node's data.
         * 
         * @param entity The SceneEntity to attach components to
         * @param node The source Node with resource indices
         * @param model The ModelResource containing resource arrays
         */
        static void CreateComponents(SceneEntity* entity, const Node* node,
                                       const Resource::ModelResource* model);

        /**
         * @brief Create a specific model-node component type
         * 
         * @param typeName The registered type name
         * @param entity The SceneEntity to attach to
         * @param node The source Node
         * @param model The ModelResource
         * @return Created actor component or nullptr
         */
        static ActorComponent* CreateComponent(const std::string& typeName,
                                               SceneEntity* entity,
                                               const Node* node,
                                               const Resource::ModelResource* model);

        /**
         * @brief Create an actor component by registered class name
         */
        static std::unique_ptr<ActorComponent> CreateComponentByClassName(const std::string& typeName);

        // =====================================================================
        // Query
        // =====================================================================

        /**
         * @brief Check if a creator is registered for a type
         */
        static bool IsRegistered(const std::string& typeName);

        /**
         * @brief Get all registered type names
         */
        static std::vector<std::string> GetRegisteredTypes();

    private:
        static std::unordered_map<std::string, Creator>& GetCreators();
        static std::unordered_map<std::string, ClassCreator>& GetComponentClassCreators();
    };

    template<typename T>
    void ComponentFactory::RegisterComponentClass(const std::string& typeName)
    {
        static_assert(std::is_base_of_v<ActorComponent, T>, "T must derive from ActorComponent");

        RegisterComponentClass(typeName, []() {
            return std::make_unique<T>();
        });
    }

} // namespace RVX
