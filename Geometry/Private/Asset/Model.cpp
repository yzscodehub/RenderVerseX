#include "Geometry/Asset/Model.h"

#include <unordered_set>

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

    bool Model::ComputeBoundingBox(const std::vector<Mesh::Ptr>& meshes)
    {
        m_bbox.Reset();
        if (!m_root || meshes.empty())
        {
            return false;
        }

        BoundingBox result;
        bool hasBounds = false;
        bool hasInvalidMeshReference = false;
        m_root->TraverseDepthFirst(
            [&](Node* node)
            {
                if (node == nullptr || hasInvalidMeshReference)
                {
                    return;
                }

                const auto addMeshBounds = [&](int meshIndex)
                {
                    if (meshIndex < 0 ||
                        static_cast<size_t>(meshIndex) >= meshes.size())
                    {
                        hasInvalidMeshReference = true;
                        return;
                    }

                    const Mesh::Ptr& mesh = meshes[static_cast<size_t>(meshIndex)];
                    if (!mesh || !mesh->GetBoundingBox() ||
                        !mesh->GetBoundingBox()->IsValid())
                    {
                        hasInvalidMeshReference = true;
                        return;
                    }

                    result.Expand(
                        mesh->GetBoundingBox()->Transformed(node->GetWorldMatrix()));
                    hasBounds = true;
                };

                const std::vector<int>& explicitMeshIndices = node->GetMeshIndices();
                if (explicitMeshIndices.empty())
                {
                    const int meshIndex = node->GetMeshIndex();
                    if (meshIndex != -1)
                    {
                        addMeshBounds(meshIndex);
                    }
                    return;
                }

                std::unordered_set<int> resolvedMeshIndices;
                for (const int meshIndex : explicitMeshIndices)
                {
                    if (resolvedMeshIndices.insert(meshIndex).second)
                    {
                        addMeshBounds(meshIndex);
                    }
                }
            });

        if (hasInvalidMeshReference || !hasBounds)
        {
            return false;
        }

        m_bbox = result;
        return true;
    }

    void Model::CollectAllNodesRecursive(const Node::Ptr& node, std::vector<Node::Ptr>& outNodes) const
    {
        outNodes.push_back(node);
        for (const auto& child : node->GetChildren())
        {
            CollectAllNodesRecursive(child, outNodes);
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
