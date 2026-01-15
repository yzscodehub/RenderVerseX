#include "Core/SystemManager.h"
#include <queue>

namespace RVX
{
    void SystemManager::AddDependency(const char* systemName, const char* dependsOn)
    {
        if (!systemName || !dependsOn)
            return;
        m_dependencies[systemName].push_back(dependsOn);
        m_dirtyOrder = true;
    }

    void SystemManager::InitAll()
    {
        BuildOrder();
        for (auto* system : m_ordered)
        {
            system->OnInit();
        }
    }

    void SystemManager::UpdateAll(float deltaTime)
    {
        BuildOrder();
        for (auto* system : m_ordered)
        {
            system->OnUpdate(deltaTime);
        }
    }

    void SystemManager::RenderAll()
    {
        BuildOrder();
        for (auto* system : m_ordered)
        {
            system->OnRender();
        }
    }

    void SystemManager::ShutdownAll()
    {
        BuildOrder();
        for (auto it = m_ordered.rbegin(); it != m_ordered.rend(); ++it)
        {
            (*it)->OnShutdown();
        }
    }

    void SystemManager::Clear()
    {
        m_ordered.clear();
        m_systemLookup.clear();
        m_dependencies.clear();
        m_systems.clear();
        m_dirtyOrder = true;
    }

    void SystemManager::BuildOrder()
    {
        if (!m_dirtyOrder)
            return;

        m_ordered.clear();
        if (m_systems.empty())
        {
            m_dirtyOrder = false;
            return;
        }

        std::unordered_map<std::string, int> indegree;
        std::unordered_map<std::string, std::vector<std::string>> graph;
        for (const auto& system : m_systems)
        {
            indegree[system->GetName()] = 0;
        }

        for (const auto& [systemName, deps] : m_dependencies)
        {
            for (const auto& dep : deps)
            {
                graph[dep].push_back(systemName);
                indegree[systemName]++;
            }
        }

        std::queue<std::string> q;
        for (const auto& [name, degree] : indegree)
        {
            if (degree == 0)
                q.push(name);
        }

        std::vector<std::string> orderedNames;
        while (!q.empty())
        {
            auto name = q.front();
            q.pop();
            orderedNames.push_back(name);

            for (const auto& next : graph[name])
            {
                if (--indegree[next] == 0)
                    q.push(next);
            }
        }

        if (orderedNames.size() != m_systems.size())
        {
            // fallback to registration order
            for (const auto& system : m_systems)
                m_ordered.push_back(system.get());
        }
        else
        {
            for (const auto& name : orderedNames)
            {
                auto it = m_systemLookup.find(name);
                if (it != m_systemLookup.end())
                    m_ordered.push_back(it->second);
            }
        }

        m_dirtyOrder = false;
    }
} // namespace RVX
