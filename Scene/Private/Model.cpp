#include "Scene/Model.h"

namespace RVX
{
    std::vector<Node::Ptr> Model::GetAllNodes() const
    {
        std::vector<Node::Ptr> nodes;
        if (m_root)
        {
            CollectAllNodesRecursive(m_root, nodes);
        }
        return nodes;
    }

    Node::Ptr Model::GetNodeByName(const std::string& name) const
    {
        if (m_root)
        {
            return FindNodeByNameRecursive(m_root, name);
        }
        return nullptr;
    }

    void Model::ComputeBoundingBox()
    {
        if (!m_root) return;
        
        if (auto bounds = m_root->ComputeWorldBoundingBox())
        {
            m_bbox = *bounds;
        }
    }

    std::vector<Node::Ptr> Model::GetBoneNodes() const
    {
        std::vector<Node::Ptr> boneNodes;
        if (m_root)
        {
            CollectBoneNodesRecursive(m_root, boneNodes);
        }
        return boneNodes;
    }

    void Model::CollectAllNodesRecursive(const Node::Ptr& node, std::vector<Node::Ptr>& outNodes) const
    {
        outNodes.push_back(node);
        for (const auto& child : node->GetChildren())
        {
            CollectAllNodesRecursive(child, outNodes);
        }
    }

    void Model::CollectBoneNodesRecursive(const Node::Ptr& node, std::vector<Node::Ptr>& outNodes) const
    {
        if (node->IsBone())
        {
            outNodes.push_back(node);
        }
        for (const auto& child : node->GetChildren())
        {
            CollectBoneNodesRecursive(child, outNodes);
        }
    }

    Node::Ptr Model::FindNodeByNameRecursive(const Node::Ptr& node, const std::string& name) const
    {
        if (node->GetName() == name)
        {
            return node;
        }
        for (const auto& child : node->GetChildren())
        {
            if (auto found = FindNodeByNameRecursive(child, name))
            {
                return found;
            }
        }
        return nullptr;
    }

} // namespace RVX
