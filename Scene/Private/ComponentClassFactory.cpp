#include "Scene/ComponentFactory.h"

#include <utility>

namespace RVX
{

std::unordered_map<std::string, ComponentFactory::ClassCreator>& ComponentFactory::GetComponentClassCreators()
{
    static std::unordered_map<std::string, ClassCreator> s_creators;
    return s_creators;
}

void ComponentFactory::RegisterComponentClass(const std::string& typeName, ClassCreator creator)
{
    GetComponentClassCreators()[typeName] = std::move(creator);
}

void ComponentFactory::ClearComponentClasses()
{
    GetComponentClassCreators().clear();
}

std::unique_ptr<ActorComponent> ComponentFactory::CreateComponentByClassName(const std::string& typeName)
{
    auto& creators = GetComponentClassCreators();
    auto it = creators.find(typeName);
    if (it == creators.end())
        return nullptr;

    return it->second();
}

} // namespace RVX
