/**
 * @file Blend1D.cpp
 * @brief 1D blend space implementation
 */

#include "Animation/Blend/Blend1D.h"
#include "Animation/Blend/BlendNode.h"
#include <algorithm>
#include <cmath>

namespace RVX::Animation
{

Blend1D::Blend1D(const std::string& parameterName)
    : m_parameterName(parameterName)
{
}

void Blend1D::AddEntry(float position, BlendNodePtr node)
{
    m_entries.push_back(BlendEntry1D{position, node});
    m_sorted = false;
}

void Blend1D::AddClip(float position, AnimationClip::ConstPtr clip)
{
    auto node = std::make_shared<ClipNode>(clip);
    AddEntry(position, node);
}

void Blend1D::RemoveEntry(size_t index)
{
    if (index < m_entries.size())
    {
        m_entries.erase(m_entries.begin() + index);
    }
}

void Blend1D::ClearEntries()
{
    m_entries.clear();
    m_weights.clear();
}

void Blend1D::SortEntries()
{
    std::sort(m_entries.begin(), m_entries.end(),
        [](const BlendEntry1D& a, const BlendEntry1D& b) {
            return a.position < b.position;
        });
    m_sorted = true;
}

float Blend1D::Evaluate(const BlendContext& context, SkeletonPose& outPose)
{
    if (m_entries.empty() || !m_active)
        return 0.0f;

    if (!m_sorted)
        SortEntries();

    // Get parameter value
    m_currentValue = context.GetParameter(m_parameterName, 0.0f);
    
    // Calculate weights
    CalculateWeights(m_currentValue);

    // Single entry
    if (m_entries.size() == 1)
    {
        return m_entries[0].node->Evaluate(context, outPose) * m_weight;
    }

    // Blend multiple entries
    bool firstPose = true;
    SkeletonPose tempPose;
    
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        if (m_weights[i] <= 0.001f)
            continue;

        if (firstPose)
        {
            m_entries[i].node->Evaluate(context, outPose);
            firstPose = false;
        }
        else
        {
            if (tempPose.GetBoneCount() != outPose.GetBoneCount())
            {
                tempPose.SetSkeleton(outPose.GetSkeleton());
            }
            m_entries[i].node->Evaluate(context, tempPose);
            outPose.BlendWith(tempPose, m_weights[i]);
        }
    }

    return m_weight;
}

void Blend1D::Update(const BlendContext& context)
{
    for (auto& entry : m_entries)
    {
        if (entry.node)
        {
            entry.node->Update(context);
        }
    }
}

void Blend1D::Reset()
{
    for (auto& entry : m_entries)
    {
        if (entry.node)
        {
            entry.node->Reset();
        }
    }
}

std::vector<BlendNode*> Blend1D::GetChildren()
{
    std::vector<BlendNode*> children;
    children.reserve(m_entries.size());
    for (auto& entry : m_entries)
    {
        if (entry.node)
        {
            children.push_back(entry.node.get());
        }
    }
    return children;
}

float Blend1D::GetMinPosition() const
{
    if (m_entries.empty()) return 0.0f;
    
    float minPos = m_entries[0].position;
    for (const auto& entry : m_entries)
    {
        minPos = std::min(minPos, entry.position);
    }
    return minPos;
}

float Blend1D::GetMaxPosition() const
{
    if (m_entries.empty()) return 0.0f;
    
    float maxPos = m_entries[0].position;
    for (const auto& entry : m_entries)
    {
        maxPos = std::max(maxPos, entry.position);
    }
    return maxPos;
}

void Blend1D::CalculateWeights(float value)
{
    m_weights.resize(m_entries.size(), 0.0f);
    std::fill(m_weights.begin(), m_weights.end(), 0.0f);

    if (m_entries.empty()) return;
    if (m_entries.size() == 1)
    {
        m_weights[0] = 1.0f;
        return;
    }

    // Find the two surrounding entries
    size_t lowerIndex = 0;
    size_t upperIndex = 0;

    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].position <= value)
        {
            lowerIndex = i;
        }
        if (m_entries[i].position >= value && upperIndex == 0)
        {
            upperIndex = i;
            break;
        }
    }

    // Clamp to bounds
    if (value <= m_entries[0].position)
    {
        m_weights[0] = 1.0f;
        return;
    }
    
    if (value >= m_entries.back().position)
    {
        m_weights[m_entries.size() - 1] = 1.0f;
        return;
    }

    // Interpolate between two entries
    if (lowerIndex == upperIndex)
    {
        m_weights[lowerIndex] = 1.0f;
        return;
    }

    float range = m_entries[upperIndex].position - m_entries[lowerIndex].position;
    if (range > 0.0001f)
    {
        float t = (value - m_entries[lowerIndex].position) / range;
        m_weights[lowerIndex] = 1.0f - t;
        m_weights[upperIndex] = t;
    }
    else
    {
        m_weights[lowerIndex] = 0.5f;
        m_weights[upperIndex] = 0.5f;
    }
}

} // namespace RVX::Animation
