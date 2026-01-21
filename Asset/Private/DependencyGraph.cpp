#include "Asset/DependencyGraph.h"
#include <algorithm>
#include <stack>

namespace RVX::Asset
{

DependencyGraph::DependencyGraph() = default;
DependencyGraph::~DependencyGraph() = default;

void DependencyGraph::AddAsset(AssetId id, const std::vector<AssetId>& dependencies)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_dependencies[id] = dependencies;
    
    // Update reverse mapping
    for (AssetId depId : dependencies)
    {
        m_dependents[depId].push_back(id);
    }
}

void DependencyGraph::RemoveAsset(AssetId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Remove from dependencies
    auto depIt = m_dependencies.find(id);
    if (depIt != m_dependencies.end())
    {
        // Remove from dependents of each dependency
        for (AssetId depId : depIt->second)
        {
            auto& dependents = m_dependents[depId];
            dependents.erase(
                std::remove(dependents.begin(), dependents.end(), id),
                dependents.end()
            );
        }
        m_dependencies.erase(depIt);
    }
    
    // Remove from dependents
    m_dependents.erase(id);
}

void DependencyGraph::UpdateDependencies(AssetId id, const std::vector<AssetId>& dependencies)
{
    // Remove old and add new
    RemoveAsset(id);
    AddAsset(id, dependencies);
}

void DependencyGraph::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dependencies.clear();
    m_dependents.clear();
}

std::vector<AssetId> DependencyGraph::GetDependencies(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_dependencies.find(id);
    if (it != m_dependencies.end())
    {
        return it->second;
    }
    return {};
}

std::vector<AssetId> DependencyGraph::GetAllDependencies(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::unordered_set<AssetId> visited;
    std::vector<AssetId> result;
    CollectAllDependencies(id, visited, result);
    return result;
}

std::vector<AssetId> DependencyGraph::GetDependents(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_dependents.find(id);
    if (it != m_dependents.end())
    {
        return it->second;
    }
    return {};
}

std::vector<AssetId> DependencyGraph::GetAllDependents(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::unordered_set<AssetId> visited;
    std::vector<AssetId> result;
    CollectAllDependents(id, visited, result);
    return result;
}

std::vector<AssetId> DependencyGraph::GetLoadOrder(AssetId id) const
{
    return GetLoadOrder(std::vector<AssetId>{id});
}

std::vector<AssetId> DependencyGraph::GetLoadOrder(const std::vector<AssetId>& ids) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<AssetId> result;
    std::unordered_set<AssetId> visited;
    std::unordered_set<AssetId> inResult;
    
    std::function<void(AssetId)> visit = [&](AssetId id) {
        if (visited.count(id)) return;
        visited.insert(id);
        
        auto it = m_dependencies.find(id);
        if (it != m_dependencies.end())
        {
            for (AssetId depId : it->second)
            {
                visit(depId);
            }
        }
        
        if (!inResult.count(id))
        {
            result.push_back(id);
            inResult.insert(id);
        }
    };
    
    for (AssetId id : ids)
    {
        visit(id);
    }
    
    return result;
}

bool DependencyGraph::HasCircularDependency(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::unordered_set<AssetId> visited;
    std::unordered_set<AssetId> recursionStack;
    
    return DetectCycle(id, visited, recursionStack);
}

std::vector<std::vector<AssetId>> DependencyGraph::FindAllCircles() const
{
    // TODO: Implement cycle detection algorithm (Tarjan's or similar)
    return {};
}

bool DependencyGraph::Contains(AssetId id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dependencies.find(id) != m_dependencies.end();
}

size_t DependencyGraph::GetAssetCount() const
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

void DependencyGraph::CollectAllDependencies(AssetId id, std::unordered_set<AssetId>& visited,
                                              std::vector<AssetId>& result) const
{
    if (visited.count(id)) return;
    visited.insert(id);
    
    auto it = m_dependencies.find(id);
    if (it != m_dependencies.end())
    {
        for (AssetId depId : it->second)
        {
            CollectAllDependencies(depId, visited, result);
            result.push_back(depId);
        }
    }
}

void DependencyGraph::CollectAllDependents(AssetId id, std::unordered_set<AssetId>& visited,
                                            std::vector<AssetId>& result) const
{
    if (visited.count(id)) return;
    visited.insert(id);
    
    auto it = m_dependents.find(id);
    if (it != m_dependents.end())
    {
        for (AssetId depId : it->second)
        {
            result.push_back(depId);
            CollectAllDependents(depId, visited, result);
        }
    }
}

bool DependencyGraph::DetectCycle(AssetId id, std::unordered_set<AssetId>& visited,
                                   std::unordered_set<AssetId>& recursionStack) const
{
    visited.insert(id);
    recursionStack.insert(id);
    
    auto it = m_dependencies.find(id);
    if (it != m_dependencies.end())
    {
        for (AssetId depId : it->second)
        {
            if (!visited.count(depId))
            {
                if (DetectCycle(depId, visited, recursionStack))
                    return true;
            }
            else if (recursionStack.count(depId))
            {
                return true;
            }
        }
    }
    
    recursionStack.erase(id);
    return false;
}

} // namespace RVX::Asset
