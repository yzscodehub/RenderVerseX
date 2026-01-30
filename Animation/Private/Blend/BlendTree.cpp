/**
 * @file BlendTree.cpp
 * @brief BlendTree implementation
 */

#include "Animation/Blend/BlendTree.h"

namespace RVX::Animation
{

BlendTree::BlendTree(Skeleton::ConstPtr skeleton)
    : m_skeleton(skeleton)
    , m_outputPose(skeleton)
{
}

void BlendTree::SetSkeleton(Skeleton::ConstPtr skeleton)
{
    m_skeleton = skeleton;
    m_outputPose.SetSkeleton(skeleton);
}

void BlendTree::SetRootNode(BlendNodePtr root)
{
    m_rootNode = root;
}

void BlendTree::SetParameter(const std::string& name, float value)
{
    m_context.SetParameter(name, value);
}

float BlendTree::GetParameter(const std::string& name, float defaultValue) const
{
    return m_context.GetParameter(name, defaultValue);
}

bool BlendTree::HasParameter(const std::string& name) const
{
    return m_context.parameters.find(name) != m_context.parameters.end();
}

std::vector<std::string> BlendTree::GetParameterNames() const
{
    std::vector<std::string> names;
    names.reserve(m_context.parameters.size());
    for (const auto& [name, value] : m_context.parameters)
    {
        names.push_back(name);
    }
    return names;
}

void BlendTree::ClearParameters()
{
    m_context.parameters.clear();
}

void BlendTree::Update(float deltaTime)
{
    if (!m_rootNode || !m_skeleton)
        return;

    m_context.deltaTime = deltaTime;

    // Update all nodes
    m_rootNode->Update(m_context);

    // Evaluate and produce output pose
    m_outputPose.ResetToBindPose();
    m_rootNode->Evaluate(m_context, m_outputPose);
}

void BlendTree::Reset()
{
    if (m_rootNode)
    {
        m_rootNode->Reset();
    }
    m_outputPose.ResetToBindPose();
}

float BlendTree::GetDuration() const
{
    if (!m_rootNode)
    {
        return 0.0f;
    }
    return m_rootNode->GetDuration();
}

BlendNode* BlendTree::FindNode(const std::string& name) const
{
    if (!m_rootNode)
        return nullptr;
    return FindNodeRecursive(m_rootNode.get(), name);
}

BlendNode* BlendTree::FindNodeRecursive(BlendNode* node, const std::string& name) const
{
    if (!node)
        return nullptr;

    if (node->GetName() == name)
        return node;

    for (BlendNode* child : node->GetChildren())
    {
        BlendNode* found = FindNodeRecursive(child, name);
        if (found)
            return found;
    }

    return nullptr;
}

} // namespace RVX::Animation
