#include "Scene/ComponentFactory.h"

#include <utility>

namespace RVX
{
namespace
{
    ComponentFactory::ComponentClassDesc NormalizeComponentClassDesc(
        ComponentFactory::ComponentClassDesc desc)
    {
        if (desc.displayName.empty())
        {
            desc.displayName = desc.className;
        }
        if (desc.category.empty())
        {
            desc.category = "Components";
        }
        return desc;
    }
}

std::unordered_map<std::string, ComponentFactory::ClassCreator>& ComponentFactory::GetComponentClassCreators()
{
    static std::unordered_map<std::string, ClassCreator> s_creators;
    return s_creators;
}

std::unordered_map<std::string, ComponentFactory::ComponentClassDesc>&
ComponentFactory::GetComponentClassDescsStorage()
{
    static std::unordered_map<std::string, ComponentClassDesc> s_descs;
    return s_descs;
}

void ComponentFactory::RegisterComponentClass(const std::string& typeName, ClassCreator creator)
{
    RegisterComponentClass(ComponentClassDesc{typeName, typeName, "Components", true},
                           std::move(creator));
}

void ComponentFactory::RegisterComponentClass(ComponentClassDesc desc, ClassCreator creator)
{
    desc = NormalizeComponentClassDesc(std::move(desc));
    if (desc.className.empty())
    {
        return;
    }

    GetComponentClassCreators()[desc.className] = std::move(creator);
    GetComponentClassDescsStorage()[desc.className] = std::move(desc);
}

void ComponentFactory::ClearComponentClasses()
{
    GetComponentClassCreators().clear();
    GetComponentClassDescsStorage().clear();
}

std::unique_ptr<ActorComponent> ComponentFactory::CreateComponentByClassName(const std::string& typeName)
{
    auto& creators = GetComponentClassCreators();
    auto it = creators.find(typeName);
    if (it == creators.end())
        return nullptr;

    return it->second();
}

bool ComponentFactory::IsComponentClassRegistered(const std::string& typeName)
{
    return GetComponentClassCreators().find(typeName) != GetComponentClassCreators().end();
}

bool ComponentFactory::GetComponentClassDesc(const std::string& typeName,
                                             ComponentClassDesc& outDesc)
{
    if (GetComponentClassCreators().find(typeName) == GetComponentClassCreators().end())
    {
        return false;
    }

    const auto& descStorage = GetComponentClassDescsStorage();
    auto descIt = descStorage.find(typeName);
    if (descIt != descStorage.end())
    {
        outDesc = descIt->second;
        return true;
    }

    outDesc = NormalizeComponentClassDesc(
        ComponentClassDesc{typeName, typeName, "Components", true});
    return true;
}

std::vector<std::string> ComponentFactory::GetRegisteredComponentClassNames()
{
    std::vector<std::string> typeNames;
    const auto& creators = GetComponentClassCreators();
    typeNames.reserve(creators.size());
    for (const auto& [typeName, creator] : creators)
    {
        (void)creator;
        typeNames.push_back(typeName);
    }
    return typeNames;
}

std::vector<ComponentFactory::ComponentClassDesc> ComponentFactory::GetRegisteredComponentClassDescs()
{
    std::vector<ComponentClassDesc> descs;
    const auto& creators = GetComponentClassCreators();
    descs.reserve(creators.size());
    for (const auto& [typeName, creator] : creators)
    {
        (void)creator;
        ComponentClassDesc desc;
        if (GetComponentClassDesc(typeName, desc))
        {
            descs.push_back(std::move(desc));
        }
    }
    return descs;
}

} // namespace RVX
