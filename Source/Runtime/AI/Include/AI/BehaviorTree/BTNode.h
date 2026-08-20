#pragma once

/**
 * @file BTNode.h
 * @brief Base class for behavior tree nodes
 */

#include "AI/AITypes.h"
#include "AI/BehaviorTree/Blackboard.h"
#include "Core/Types.h"

#include <string>
#include <vector>
#include <memory>

namespace RVX::AI
{

class BehaviorTree;
class BTNode;

using BTNodePtr = std::shared_ptr<BTNode>;

/**
 * @brief Context passed to behavior tree nodes during execution
 */
struct BTContext
{
    BehaviorTree* tree = nullptr;
    Blackboard* blackboard = nullptr;
    uint64 entityId = 0;
    float deltaTime = 0.0f;
    void* userData = nullptr;
};

/**
 * @brief Base class for all behavior tree nodes
 * 
 * Behavior tree nodes form a tree structure that controls AI decision making.
 * Each node returns Success, Failure, or Running status.
 * 
 * Node types:
 * - **Composite**: Has multiple children (Selector, Sequence, Parallel)
 * - **Decorator**: Wraps a single child (Condition, Inverter, Repeater)
 * - **Task**: Leaf node that performs actions
 * - **Service**: Background task that updates while parent is active
 */
class BTNode
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    BTNode(const std::string& name = "");
    virtual ~BTNode() = default;

    // =========================================================================
    // Properties
    // =========================================================================

    /**
     * @brief Get node name
     */
    const std::string& GetName() const { return m_name; }

    /**
     * @brief Set node name
     */
    void SetName(const std::string& name) { m_name = name; }

    /**
     * @brief Get node type
     */
    virtual BTNodeType GetType() const = 0;

    /**
     * @brief Get the parent node
     */
    BTNode* GetParent() const { return m_parent; }

    /**
     * @brief Get child nodes
     */
    const std::vector<BTNodePtr>& GetChildren() const { return m_children; }

    /**
     * @brief Get unique node ID
     */
    uint32 GetNodeId() const { return m_nodeId; }

    // =========================================================================
    // Tree Building
    // =========================================================================

    /**
     * @brief Add a child node
     */
    void AddChild(BTNodePtr child);

    /**
     * @brief Remove a child node
     */
    void RemoveChild(BTNode* child);

    /**
     * @brief Clear all children
     */
    void ClearChildren();

    // =========================================================================
    // Execution
    // =========================================================================

    /**
     * @brief Execute the node
     * @param context Execution context
     * @return Node status
     */
    BTStatus Tick(BTContext& context);

    /**
     * @brief Abort the node execution
     */
    virtual void Abort(BTContext& context);

    /**
     * @brief Reset the node state
     */
    virtual void Reset();

    /**
     * @brief Check if node is currently running
     */
    bool IsRunning() const { return m_status == BTStatus::Running; }

    /**
     * @brief Get last execution status
     */
    BTStatus GetStatus() const { return m_status; }

protected:
    // =========================================================================
    // Virtual Methods (Override in derived classes)
    // =========================================================================

    /**
     * @brief Called when node starts execution
     */
    virtual void OnEnter(BTContext& context) { (void)context; }

    /**
     * @brief Execute node logic
     */
    virtual BTStatus OnTick(BTContext& context) = 0;

    /**
     * @brief Called when node finishes (Success or Failure)
     */
    virtual void OnExit(BTContext& context, BTStatus status) 
    { 
        (void)context; 
        (void)status; 
    }

    /**
     * @brief Called when node is aborted
     */
    virtual void OnAbort(BTContext& context) { (void)context; }

    // Helper to get blackboard value
    template<typename T>
    std::optional<T> GetBlackboardValue(BTContext& context, const std::string& key) const
    {
        if (context.blackboard)
        {
            return context.blackboard->GetValue<T>(key);
        }
        return std::nullopt;
    }

    // Helper to set blackboard value
    template<typename T>
    void SetBlackboardValue(BTContext& context, const std::string& key, const T& value)
    {
        if (context.blackboard)
        {
            context.blackboard->SetValue<T>(key, value);
        }
    }

private:
    friend class BehaviorTree;

    std::string m_name;
    uint32 m_nodeId = 0;
    BTStatus m_status = BTStatus::Invalid;
    bool m_wasRunning = false;

    BTNode* m_parent = nullptr;

    static uint32 s_nextNodeId;

protected:
    std::vector<BTNodePtr> m_children;
};

} // namespace RVX::AI
