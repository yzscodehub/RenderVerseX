#pragma once

/**
 * @file BehaviorTree.h
 * @brief Behavior tree for AI decision making
 */

#include "AI/AITypes.h"
#include "AI/BehaviorTree/BTNode.h"
#include "AI/BehaviorTree/Blackboard.h"
#include "Core/Types.h"

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>

namespace RVX::AI
{

/**
 * @brief Behavior tree for AI decision making
 * 
 * A BehaviorTree organizes AI logic into a tree of nodes that are evaluated
 * each tick. The tree starts from the root and traverses down until a leaf
 * node (Task) is reached.
 * 
 * Node types:
 * - **Selector**: Tries children in order until one succeeds (OR logic)
 * - **Sequence**: Runs children in order until one fails (AND logic)
 * - **Parallel**: Runs all children simultaneously
 * - **Decorator**: Modifies child behavior (conditions, loops, etc.)
 * - **Task**: Performs actual actions
 * 
 * Usage:
 * @code
 * // Create a simple patrol and chase behavior
 * auto tree = std::make_shared<BehaviorTree>("GuardBehavior");
 * 
 * auto root = std::make_shared<BTSelector>("Root");
 * 
 * // Chase sequence
 * auto chaseSeq = std::make_shared<BTSequence>("ChaseSequence");
 * chaseSeq->AddChild(std::make_shared<BTCondition>("HasTarget", 
 *     [](BTContext& ctx) { return ctx.blackboard->HasKey("Target"); }));
 * chaseSeq->AddChild(std::make_shared<MoveToTask>("ChaseTarget", "Target"));
 * chaseSeq->AddChild(std::make_shared<AttackTask>("Attack"));
 * 
 * // Patrol task (fallback)
 * auto patrol = std::make_shared<PatrolTask>("Patrol");
 * 
 * root->AddChild(chaseSeq);
 * root->AddChild(patrol);
 * 
 * tree->SetRoot(root);
 * @endcode
 */
class BehaviorTree
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    BehaviorTree(const std::string& name = "BehaviorTree");
    ~BehaviorTree();

    BehaviorTree(const BehaviorTree&) = delete;
    BehaviorTree& operator=(const BehaviorTree&) = delete;

    // =========================================================================
    // Properties
    // =========================================================================

    /**
     * @brief Get tree name
     */
    const std::string& GetName() const { return m_name; }

    /**
     * @brief Set tree name
     */
    void SetName(const std::string& name) { m_name = name; }

    /**
     * @brief Get the root node
     */
    BTNode* GetRoot() const { return m_root.get(); }

    /**
     * @brief Set the root node
     */
    void SetRoot(BTNodePtr root);

    /**
     * @brief Get the blackboard
     */
    Blackboard* GetBlackboard() { return &m_blackboard; }

    /**
     * @brief Get the blackboard (const)
     */
    const Blackboard* GetBlackboard() const { return &m_blackboard; }

    // =========================================================================
    // Execution
    // =========================================================================

    /**
     * @brief Execute one tick of the behavior tree
     * @param entityId Entity this tree controls
     * @param deltaTime Time since last tick
     * @param userData Optional user data passed to nodes
     * @return Status of the root node
     */
    BTStatus Tick(uint64 entityId, float deltaTime, void* userData = nullptr);

    /**
     * @brief Abort the currently running branch
     */
    void Abort();

    /**
     * @brief Reset the entire tree
     */
    void Reset();

    /**
     * @brief Check if tree is currently running
     */
    bool IsRunning() const { return m_isRunning; }

    // =========================================================================
    // Node Access
    // =========================================================================

    /**
     * @brief Find a node by name
     */
    BTNode* FindNode(const std::string& name) const;

    /**
     * @brief Find a node by ID
     */
    BTNode* FindNode(uint32 nodeId) const;

    /**
     * @brief Get all nodes of a specific type
     */
    template<typename T>
    std::vector<T*> GetNodesOfType() const
    {
        std::vector<T*> result;
        CollectNodesOfType<T>(m_root.get(), result);
        return result;
    }

    // =========================================================================
    // Cloning
    // =========================================================================

    /**
     * @brief Create a deep copy of this tree
     */
    std::unique_ptr<BehaviorTree> Clone() const;

private:
    std::string m_name;
    BTNodePtr m_root;
    Blackboard m_blackboard;
    bool m_isRunning = false;

    // Node lookup cache
    mutable std::unordered_map<std::string, BTNode*> m_nodesByName;
    mutable std::unordered_map<uint32, BTNode*> m_nodesById;
    mutable bool m_cacheValid = false;

    void RebuildCache() const;
    void CollectNodes(BTNode* node, 
                      std::unordered_map<std::string, BTNode*>& byName,
                      std::unordered_map<uint32, BTNode*>& byId) const;

    template<typename T>
    void CollectNodesOfType(BTNode* node, std::vector<T*>& result) const
    {
        if (!node) return;
        if (auto* typed = dynamic_cast<T*>(node))
        {
            result.push_back(typed);
        }
        for (const auto& child : node->GetChildren())
        {
            CollectNodesOfType<T>(child.get(), result);
        }
    }
};

} // namespace RVX::AI
