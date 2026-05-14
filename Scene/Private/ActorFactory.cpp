#include "Scene/ActorFactory.h"

#include <utility>

namespace RVX
{

std::unordered_map<std::string, ActorFactory::Creator>& ActorFactory::GetCreators()
{
    static std::unordered_map<std::string, Creator> s_creators;
    return s_creators;
}

void ActorFactory::Register(const std::string& className, Creator creator)
{
    GetCreators()[className] = std::move(creator);
}

void ActorFactory::Unregister(const std::string& className)
{
    GetCreators().erase(className);
}

void ActorFactory::ClearAll()
{
    GetCreators().clear();
}

std::unique_ptr<Actor> ActorFactory::Create(const std::string& className)
{
    auto& creators = GetCreators();
    auto it = creators.find(className);
    if (it == creators.end())
        return nullptr;

    return it->second();
}

bool ActorFactory::IsRegistered(const std::string& className)
{
    return GetCreators().find(className) != GetCreators().end();
}

std::vector<std::string> ActorFactory::GetRegisteredClassNames()
{
    std::vector<std::string> classNames;
    classNames.reserve(GetCreators().size());
    for (const auto& [className, creator] : GetCreators())
    {
        (void)creator;
        classNames.push_back(className);
    }
    return classNames;
}

} // namespace RVX
