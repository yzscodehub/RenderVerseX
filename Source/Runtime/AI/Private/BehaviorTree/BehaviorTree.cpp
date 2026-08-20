/**
 * @file BehaviorTree.cpp
 * @brief Behavior tree implementation
 */

#include "AI/BehaviorTree/BehaviorTree.h"
#include "Core/Log.h"

namespace RVX::AI
{

// =========================================================================
// Construction
// =========================================================================

BehaviorTree::BehaviorTree(const std::string& name)
    : m_name(name)
{
}

BehaviorTree::~BehaviorTree()
{
    if (m_isRunning)
    {
        Abort();
    }
}

// =========================================================================
// Tree Setup
// =========================================================================

void BehaviorTree::SetRoot(BTNodePtr root)
{
    if (m_isRunning)
    {
        Abort();
    }

    m_root = std::move(root);
    m_cacheValid = false;
}

// =========================================================================
// Execution
// =========================================================================

BTStatus BehaviorTree::Tick(uint64 entityId, float deltaTime, void* userData)
{
    if (!m_root)
    {
        return BTStatus::Failure;
    }

    BTContext context;
    context.tree = this;
    context.blackboard = &m_blackboard;
    context.entityId = entityId;
    context.deltaTime = deltaTime;
    context.userData = userData;

    m_isRunning = true;
    BTStatus status = m_root->Tick(context);
    
    if (status != BTStatus::Running)
    {
        m_isRunning = false;
    }

    return status;
}

void BehaviorTree::Abort()
{
    if (!m_root || !m_isRunning)
    {
        return;
    }

    BTContext context;
    context.tree = this;
    context.blackboard = &m_blackboard;

    m_root->Abort(context);
    m_isRunning = false;
}

void BehaviorTree::Reset()
{
    if (m_isRunning)
    {
        Abort();
    }

    if (m_root)
    {
        m_root->Reset();
    }

    m_blackboard.Clear();
}

// =========================================================================
// Node Access
// =========================================================================

BTNode* BehaviorTree::FindNode(const std::string& name) const
{
    if (!m_cacheValid)
    {
        RebuildCache();
    }

    auto it = m_nodesByName.find(name);
    return it != m_nodesByName.end() ? it->second : nullptr;
}

BTNode* BehaviorTree::FindNode(uint32 nodeId) const
{
    if (!m_cacheValid)
    {
        RebuildCache();
    }

    auto it = m_nodesById.find(nodeId);
    return it != m_nodesById.end() ? it->second : nullptr;
}

// =========================================================================
// Cloning
// =========================================================================

std::unique_ptr<BehaviorTree> BehaviorTree::Clone() const
{
    // Note: Full cloning would require serialization/deserialization
    // or a virtual Clone method on each node type.
    // For now, we create an empty tree with the same name.
    
    auto clone = std::make_unique<BehaviorTree>(m_name + "_clone");
    
    // In a real implementation, you would deep clone the node hierarchy
    // clone->SetRoot(CloneNode(m_root));
    
    RVX_CORE_WARN("BehaviorTree::Clone - Full cloning not implemented, "
                  "returning empty tree. Implement node cloning for production use.");
    
    return clone;
}

// =========================================================================
// Internal Methods
// =========================================================================

void BehaviorTree::RebuildCache() const
{
    m_nodesByName.clear();
    m_nodesById.clear();

    if (m_root)
    {
        CollectNodes(m_root.get(), m_nodesByName, m_nodesById);
    }

    m_cacheValid = true;
}

void BehaviorTree::CollectNodes(BTNode* node,
                                std::unordered_map<std::string, BTNode*>& byName,
                                std::unordered_map<uint32, BTNode*>& byId) const
{
    if (!node)
    {
        return;
    }

    if (!node->GetName().empty())
    {
        byName[node->GetName()] = node;
    }
    byId[node->GetNodeId()] = node;

    for (const auto& child : node->GetChildren())
    {
        CollectNodes(child.get(), byName, byId);
    }
}

} // namespace RVX::AI
