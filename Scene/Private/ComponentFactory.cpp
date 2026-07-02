#include "Scene/ComponentFactory.h"

#include <utility>

namespace RVX
{
    // =========================================================================
    // Static Storage
    // =========================================================================

    std::unordered_map<std::string, ComponentFactory::Creator>& ComponentFactory::GetCreators()
    {
        static std::unordered_map<std::string, Creator> s_creators;
        return s_creators;
    }

    // =========================================================================
    // Registration
    // =========================================================================

    void ComponentFactory::Register(const std::string& typeName, Creator creator)
    {
        GetCreators()[typeName] = std::move(creator);
    }

    void ComponentFactory::Unregister(const std::string& typeName)
    {
        GetCreators().erase(typeName);
    }

    void ComponentFactory::ClearAll()
    {
        GetCreators().clear();
    }

    // =========================================================================
    // Component Creation
    // =========================================================================

    void ComponentFactory::CreateComponents(SceneEntity* entity, const Node* node,
                                              const Resource::ModelResource* model)
    {
        if (!entity || !node || !model)
        {
            return;
        }

        // Call all registered creators
        for (const auto& [typeName, creator] : GetCreators())
        {
            creator(entity, node, model);
        }
    }

    ActorComponent* ComponentFactory::CreateComponent(const std::string& typeName,
                                                      SceneEntity* entity,
                                                      const Node* node,
                                                      const Resource::ModelResource* model)
    {
        auto& creators = GetCreators();
        auto it = creators.find(typeName);
        if (it != creators.end())
        {
            return it->second(entity, node, model);
        }
        return nullptr;
    }

    // =========================================================================
    // Query
    // =========================================================================

    bool ComponentFactory::IsRegistered(const std::string& typeName)
    {
        return GetCreators().find(typeName) != GetCreators().end();
    }

    std::vector<std::string> ComponentFactory::GetRegisteredTypes()
    {
        std::vector<std::string> types;
        for (const auto& [typeName, creator] : GetCreators())
        {
            types.push_back(typeName);
        }
        return types;
    }

} // namespace RVX
