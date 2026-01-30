/**
 * @file Blend2D.cpp
 * @brief 2D blend space implementation
 */

#include "Animation/Blend/Blend2D.h"
#include "Animation/Blend/BlendNode.h"
#include <algorithm>
#include <cmath>

namespace RVX::Animation
{

Blend2D::Blend2D(const std::string& paramX, const std::string& paramY)
    : m_parameterX(paramX)
    , m_parameterY(paramY)
{
}

void Blend2D::SetParameterNames(const std::string& paramX, const std::string& paramY)
{
    m_parameterX = paramX;
    m_parameterY = paramY;
}

void Blend2D::AddEntry(Vec2 position, BlendNodePtr node)
{
    m_entries.push_back(BlendEntry2D{position, node});
}

void Blend2D::AddEntry(float x, float y, BlendNodePtr node)
{
    AddEntry(Vec2(x, y), node);
}

void Blend2D::AddClip(Vec2 position, AnimationClip::ConstPtr clip)
{
    auto node = std::make_shared<ClipNode>(clip);
    AddEntry(position, node);
}

void Blend2D::AddClip(float x, float y, AnimationClip::ConstPtr clip)
{
    AddClip(Vec2(x, y), clip);
}

void Blend2D::RemoveEntry(size_t index)
{
    if (index < m_entries.size())
    {
        m_entries.erase(m_entries.begin() + index);
    }
}

void Blend2D::ClearEntries()
{
    m_entries.clear();
}

float Blend2D::Evaluate(const BlendContext& context, SkeletonPose& outPose)
{
    if (m_entries.empty() || !m_active)
        return 0.0f;

    // Get parameter values
    float x = context.GetParameter(m_parameterX, 0.0f);
    float y = context.GetParameter(m_parameterY, 0.0f);
    m_currentPosition = Vec2(x, y);

    // Calculate weights based on blend type
    switch (m_blendType)
    {
        case Blend2DType::FreeformDirectional:
        case Blend2DType::FreeformCartesian:
            CalculateWeightsFreeform(m_currentPosition);
            break;
        case Blend2DType::SimpleDirectional:
            CalculateWeightsSimpleDirectional(m_currentPosition);
            break;
    }

    // Single entry
    if (m_entries.size() == 1)
    {
        return m_entries[0].node->Evaluate(context, outPose) * m_weight;
    }

    // Blend entries
    bool firstPose = true;
    SkeletonPose tempPose;
    float totalWeight = 0.0f;

    for (auto& entry : m_entries)
    {
        if (entry.cachedWeight <= 0.001f)
            continue;

        totalWeight += entry.cachedWeight;
    }

    if (totalWeight <= 0.001f)
    {
        // Fallback to first entry
        return m_entries[0].node->Evaluate(context, outPose) * m_weight;
    }

    for (auto& entry : m_entries)
    {
        if (entry.cachedWeight <= 0.001f)
            continue;

        float normalizedWeight = entry.cachedWeight / totalWeight;

        if (firstPose)
        {
            entry.node->Evaluate(context, outPose);
            firstPose = false;
        }
        else
        {
            if (tempPose.GetBoneCount() != outPose.GetBoneCount())
            {
                tempPose.SetSkeleton(outPose.GetSkeleton());
            }
            entry.node->Evaluate(context, tempPose);
            outPose.BlendWith(tempPose, normalizedWeight);
        }
    }

    return m_weight;
}

void Blend2D::Update(const BlendContext& context)
{
    for (auto& entry : m_entries)
    {
        if (entry.node)
        {
            entry.node->Update(context);
        }
    }
}

void Blend2D::Reset()
{
    for (auto& entry : m_entries)
    {
        if (entry.node)
        {
            entry.node->Reset();
        }
    }
}

std::vector<BlendNode*> Blend2D::GetChildren()
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

Vec2 Blend2D::GetMinBounds() const
{
    if (m_entries.empty()) return Vec2(0.0f);
    
    Vec2 minBounds = m_entries[0].position;
    for (const auto& entry : m_entries)
    {
        minBounds.x = std::min(minBounds.x, entry.position.x);
        minBounds.y = std::min(minBounds.y, entry.position.y);
    }
    return minBounds;
}

Vec2 Blend2D::GetMaxBounds() const
{
    if (m_entries.empty()) return Vec2(0.0f);
    
    Vec2 maxBounds = m_entries[0].position;
    for (const auto& entry : m_entries)
    {
        maxBounds.x = std::max(maxBounds.x, entry.position.x);
        maxBounds.y = std::max(maxBounds.y, entry.position.y);
    }
    return maxBounds;
}

void Blend2D::CalculateWeightsFreeform(Vec2 position)
{
    // Gradient band algorithm for freeform blending
    // Weights are inversely proportional to distance
    
    float totalWeight = 0.0f;
    
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        m_entries[i].cachedWeight = CalculateGradientBandWeight(position, m_entries[i].position, i);
        totalWeight += m_entries[i].cachedWeight;
    }

    // Normalize
    if (totalWeight > 0.0f)
    {
        for (auto& entry : m_entries)
        {
            entry.cachedWeight /= totalWeight;
        }
    }
}

float Blend2D::CalculateGradientBandWeight(Vec2 samplePoint, Vec2 entryPos, size_t entryIndex)
{
    Vec2 diff = samplePoint - entryPos;
    float distSq = dot(diff, diff);
    
    // Small epsilon to avoid division by zero
    const float epsilon = 0.0001f;
    
    if (distSq < epsilon)
    {
        return 1.0f;  // At the exact position
    }

    // Calculate influence based on other entries
    float minInfluence = 1.0f;
    
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        if (i == entryIndex) continue;

        Vec2 otherPos = m_entries[i].position;
        Vec2 toOther = otherPos - entryPos;
        float toOtherLenSq = dot(toOther, toOther);
        
        if (toOtherLenSq < epsilon) continue;

        // Project sample onto line from entry to other
        float t = dot(diff, toOther) / toOtherLenSq;
        t = std::max(0.0f, std::min(1.0f, t));
        
        // Influence decreases as we move toward other entries
        float influence = 1.0f - t;
        minInfluence = std::min(minInfluence, influence);
    }

    return std::max(0.0f, minInfluence);
}

void Blend2D::CalculateWeightsSimpleDirectional(Vec2 position)
{
    // Simple 4/8 direction blend
    // Find the closest entries and blend
    
    if (m_entries.empty()) return;

    // Reset weights
    for (auto& entry : m_entries)
    {
        entry.cachedWeight = 0.0f;
    }

    float posLen = length(position);
    if (posLen < 0.001f)
    {
        // At center - find center entry or blend all
        for (auto& entry : m_entries)
        {
            if (length(entry.position) < 0.001f)
            {
                entry.cachedWeight = 1.0f;
                return;
            }
        }
        // No center entry, equal blend
        float equalWeight = 1.0f / m_entries.size();
        for (auto& entry : m_entries)
        {
            entry.cachedWeight = equalWeight;
        }
        return;
    }

    // Normalize direction
    Vec2 dir = position / posLen;

    // Find closest directional entries
    float bestDot = -2.0f;
    size_t bestIndex = 0;
    float secondBestDot = -2.0f;
    size_t secondBestIndex = 0;

    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        Vec2 entryDir = m_entries[i].position;
        float entryLen = length(entryDir);
        if (entryLen < 0.001f) continue;
        
        entryDir /= entryLen;
        float d = dot(dir, entryDir);
        
        if (d > bestDot)
        {
            secondBestDot = bestDot;
            secondBestIndex = bestIndex;
            bestDot = d;
            bestIndex = i;
        }
        else if (d > secondBestDot)
        {
            secondBestDot = d;
            secondBestIndex = i;
        }
    }

    // Calculate weights
    if (secondBestDot < -1.5f || bestIndex == secondBestIndex)
    {
        m_entries[bestIndex].cachedWeight = 1.0f;
    }
    else
    {
        float range = bestDot - secondBestDot;
        if (range > 0.001f)
        {
            float t = (bestDot - secondBestDot) / range;
            m_entries[bestIndex].cachedWeight = t;
            m_entries[secondBestIndex].cachedWeight = 1.0f - t;
        }
        else
        {
            m_entries[bestIndex].cachedWeight = 0.5f;
            m_entries[secondBestIndex].cachedWeight = 0.5f;
        }
    }
}

} // namespace RVX::Animation
