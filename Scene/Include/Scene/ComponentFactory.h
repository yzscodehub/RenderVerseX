#pragma once

/**
 * @file ComponentFactory.h
 * @brief Factory for creating ECS components from Node data
 * 
 * Provides a unified way to convert Node indices to ECS Components
 * during ModelResource::Instantiate().
 */

#include "Scene/Node.h"
#include "Scene/SceneEntity.h"
#include "Scene/Component.h"
#include <functional>
#include <unordered_map>
#include <string>
#include <memory>

namespace RVX
{
    // Forward declarations
    class SceneEntity;

    namespace Resource
    {
        class ModelResource;
    }

    /**
     * @brief Factory for creating ECS components from Node data
     * 
     * ComponentFactory provides a registry-based system for converting
     * Node indices (meshIndex, materialIndices, etc.) into proper ECS
     * Component instances during model instantiation.
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
         * @brief Component creator function signature
         * 
         * @param entity The SceneEntity to attach the component to
         * @param node The source Node containing index data
         * @param model The ModelResource containing resource arrays
         * @return Created component (or nullptr if not applicable)
         */
        using Creator = std::function<Component*(SceneEntity*, const Node*, const Resource::ModelResource*)>;

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
         * @brief Register default component creators (MeshRenderer, etc.)
         * 
         * Call this during engine initialization.
         */
        static void RegisterDefaults();

        // =====================================================================
        // Component Creation
        // =====================================================================

        /**
         * @brief Create all applicable components for an entity from a node
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
         * @brief Create a specific component type
         * 
         * @param typeName The registered type name
         * @param entity The SceneEntity to attach to
         * @param node The source Node
         * @param model The ModelResource
         * @return Created component or nullptr
         */
        static Component* CreateComponent(const std::string& typeName,
                                           SceneEntity* entity,
                                           const Node* node,
                                           const Resource::ModelResource* model);

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
    };

} // namespace RVX
