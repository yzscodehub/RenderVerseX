#include "Resource/DependencyGraph.h"
#include <algorithm>
#include <stack>
#include <queue>

namespace RVX::Resource
{

DependencyGraph::DependencyGraph() = default;
DependencyGraph::~DependencyGraph() = default;

void DependencyGraph::AddResource(ResourceId id, const std::vector<ResourceId>& dependencies)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_dependencies[id] = dependencies;

    // Add reverse edges
    for (ResourceId dep : dependencies)
    {
        m_dependents[dep].push_back(id);
    }
}

void DependencyGraph::RemoveResource(ResourceId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Remove from dependencies
    auto depIt = m_dependencies.find(id);
    if (depIt != m_dependencies.end())
    {
        // Remove reverse edges
        for (ResourceId dep : depIt->second)
        {
            auto& depVec = m_dependents[dep];
            depVec.erase(std::remove(depVec.begin(), depVec.end(), id), depVec.end());
        }
        m_dependencies.erase(depIt);
    }

    // Remove from dependents
    auto deptIt = m_dependents.find(id);
    if (deptIt != m_dependents.end())
    {
        m_dependents.erase(deptIt);
    }
}

void DependencyGraph::UpdateDependencies(ResourceId id, const std::vector<ResourceId>& dependencies)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Remove old reverse edges
    auto oldDepIt = m_dependencies.find(id);
    if (oldDepIt != m_dependencies.end())
    {
        for (ResourceId dep : oldDepIt->second)
        {
            auto& depVec = m_dependents[dep];
            depVec.erase(std::remove(depVec.begin(), depVec.end(), id), depVec.end());
        }
    }

    // Update dependencies
    m_dependencies[id] = dependencies;

    // Add new reverse edges
    for (ResourceId dep : dependencies)
    {
        m_dependents[dep].push_back(id);
    }
}

void DependencyGraph::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dependencies.clear();
    m_dependents.clear();
}

std::vector<ResourceId> DependencyGraph::GetDependencies(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_dependencies.find(id);
    if (it != m_dependencies.end())
    {
        return it->second;
    }
    return {};
}

std::vector<ResourceId> DependencyGraph::GetAllDependencies(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::unordered_set<ResourceId> visited;
    std::vector<ResourceId> result;
    CollectAllDependencies(id, visited, result);
    return result;
}

std::vector<ResourceId> DependencyGraph::GetDependents(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_dependents.find(id);
    if (it != m_dependents.end())
    {
        return it->second;
    }
    return {};
}

std::vector<ResourceId> DependencyGraph::GetAllDependents(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::unordered_set<ResourceId> visited;
    std::vector<ResourceId> result;
    CollectAllDependents(id, visited, result);
    return result;
}

std::vector<ResourceId> DependencyGraph::GetLoadOrder(ResourceId id) const
{
    return GetLoadOrder(std::vector<ResourceId>{id});
}

std::vector<ResourceId> DependencyGraph::GetLoadOrder(const std::vector<ResourceId>& ids) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Topological sort using Kahn's algorithm
    std::unordered_set<ResourceId> relevantNodes;
    std::queue<ResourceId> toProcess;

    // First, collect all relevant nodes
    for (ResourceId id : ids)
    {
        toProcess.push(id);
    }

    while (!toProcess.empty())
    {
        ResourceId id = toProcess.front();
        toProcess.pop();

        if (relevantNodes.find(id) != relevantNodes.end())
        {
            continue;
        }
        relevantNodes.insert(id);

        auto it = m_dependencies.find(id);
        if (it != m_dependencies.end())
        {
            for (ResourceId dep : it->second)
            {
                toProcess.push(dep);
            }
        }
    }

    // Calculate in-degrees for relevant nodes
    std::unordered_map<ResourceId, int> inDegree;
    for (ResourceId id : relevantNodes)
    {
        inDegree[id] = 0;
    }

    for (ResourceId id : relevantNodes)
    {
        auto it = m_dependencies.find(id);
        if (it != m_dependencies.end())
        {
            for (ResourceId dep : it->second)
            {
                if (relevantNodes.find(dep) != relevantNodes.end())
                {
                    inDegree[id]++;
                }
            }
        }
    }

    // Process nodes with zero in-degree
    std::queue<ResourceId> zeroInDegree;
    for (const auto& [id, degree] : inDegree)
    {
        if (degree == 0)
        {
            zeroInDegree.push(id);
        }
    }

    std::vector<ResourceId> result;
    while (!zeroInDegree.empty())
    {
        ResourceId id = zeroInDegree.front();
        zeroInDegree.pop();
        result.push_back(id);

        // Reduce in-degree of dependents
        auto it = m_dependents.find(id);
        if (it != m_dependents.end())
        {
            for (ResourceId dependent : it->second)
            {
                if (relevantNodes.find(dependent) != relevantNodes.end())
                {
                    inDegree[dependent]--;
                    if (inDegree[dependent] == 0)
                    {
                        zeroInDegree.push(dependent);
                    }
                }
            }
        }
    }

    return result;
}

bool DependencyGraph::HasCircularDependency(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::unordered_set<ResourceId> visited;
    std::unordered_set<ResourceId> recursionStack;
    return DetectCycle(id, visited, recursionStack);
}

std::vector<std::vector<ResourceId>> DependencyGraph::FindAllCircles() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<std::vector<ResourceId>> circles;
    std::unordered_set<ResourceId> visited;

    for (const auto& [id, deps] : m_dependencies)
    {
        if (visited.find(id) == visited.end())
        {
            std::unordered_set<ResourceId> recursionStack;
            if (DetectCycle(id, visited, recursionStack))
            {
                // Found a cycle, extract it
                std::vector<ResourceId> cycle(recursionStack.begin(), recursionStack.end());
                circles.push_back(cycle);
            }
        }
    }

    return circles;
}

bool DependencyGraph::Contains(ResourceId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dependencies.find(id) != m_dependencies.end();
}

size_t DependencyGraph::GetResourceCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dependencies.size();
}

size_t DependencyGraph::GetTotalEdges() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t total = 0;
    for (const auto& [id, deps] : m_dependencies)
    {
        total += deps.size();
    }
    return total;
}

void DependencyGraph::CollectAllDependencies(ResourceId id, std::unordered_set<ResourceId>& visited,
                                              std::vector<ResourceId>& result) const
{
    auto it = m_dependencies.find(id);
    if (it == m_dependencies.end())
    {
        return;
    }

    for (ResourceId dep : it->second)
    {
        if (visited.find(dep) == visited.end())
        {
            visited.insert(dep);
            result.push_back(dep);
            CollectAllDependencies(dep, visited, result);
        }
    }
}

void DependencyGraph::CollectAllDependents(ResourceId id, std::unordered_set<ResourceId>& visited,
                                            std::vector<ResourceId>& result) const
{
    auto it = m_dependents.find(id);
    if (it == m_dependents.end())
    {
        return;
    }

    for (ResourceId dependent : it->second)
    {
        if (visited.find(dependent) == visited.end())
        {
            visited.insert(dependent);
            result.push_back(dependent);
            CollectAllDependents(dependent, visited, result);
        }
    }
}

bool DependencyGraph::DetectCycle(ResourceId id, std::unordered_set<ResourceId>& visited,
                                   std::unordered_set<ResourceId>& recursionStack) const
{
    visited.insert(id);
    recursionStack.insert(id);

    auto it = m_dependencies.find(id);
    if (it != m_dependencies.end())
    {
        for (ResourceId dep : it->second)
        {
            if (recursionStack.find(dep) != recursionStack.end())
            {
                return true; // Cycle detected
            }

            if (visited.find(dep) == visited.end())
            {
                if (DetectCycle(dep, visited, recursionStack))
                {
                    return true;
                }
            }
        }
    }

    recursionStack.erase(id);
    return false;
}

} // namespace RVX::Resource
