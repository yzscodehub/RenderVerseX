#pragma once

#include "Core/ISystem.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    class SystemManager
    {
    public:
        template<typename T, typename... Args>
        T* RegisterSystem(Args&&... args)
        {
            auto system = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = system.get();
            m_systems.emplace_back(std::move(system));
            m_systemLookup[ptr->GetName()] = ptr;
            m_dirtyOrder = true;
            return ptr;
        }

        void AddDependency(const char* systemName, const char* dependsOn);
        void InitAll();
        void UpdateAll(float deltaTime);
        void RenderAll();
        void ShutdownAll();
        void Clear();

        template<typename T>
        T* GetSystem(const char* name)
        {
            auto it = m_systemLookup.find(name);
            if (it == m_systemLookup.end())
                return nullptr;
            return dynamic_cast<T*>(it->second);
        }

    private:
        void BuildOrder();

        std::vector<std::unique_ptr<ISystem>> m_systems;
        std::unordered_map<std::string, ISystem*> m_systemLookup;
        std::unordered_map<std::string, std::vector<std::string>> m_dependencies;
        std::vector<ISystem*> m_ordered;
        bool m_dirtyOrder = true;
    };
} // namespace RVX
