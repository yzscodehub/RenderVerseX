/**
 * @file BlendNode.cpp
 * @brief BlendNode and ClipNode implementation
 */

#include "Animation/Blend/BlendNode.h"
#include "Animation/Runtime/AnimationEvaluator.h"
#include "Animation/Core/Interpolation.h"

namespace RVX::Animation
{

// ============================================================================
// ClipNode Implementation
// ============================================================================

ClipNode::ClipNode(AnimationClip::ConstPtr clip)
    : m_clip(clip)
{
    if (m_clip)
    {
        m_name = m_clip->name;
    }
}

void ClipNode::SetClip(AnimationClip::ConstPtr clip)
{
    m_clip = clip;
    Reset();
}

float ClipNode::GetNormalizedTime() const
{
    if (!m_clip || m_clip->duration <= 0)
        return 0.0f;
    return static_cast<float>(m_currentTime) / static_cast<float>(m_clip->duration);
}

void ClipNode::SetNormalizedTime(float t)
{
    if (m_clip)
    {
        m_currentTime = static_cast<TimeUs>(t * static_cast<float>(m_clip->duration));
    }
}

float ClipNode::Evaluate(const BlendContext& context, SkeletonPose& outPose)
{
    if (!m_clip || !m_active)
        return 0.0f;

    AnimationEvaluator evaluator;
    EvaluationOptions options;
    options.wrapModeOverride = m_wrapMode;
    
    evaluator.Evaluate(*m_clip, m_currentTime, outPose, options);
    
    return m_weight;
}

void ClipNode::Update(const BlendContext& context)
{
    if (!m_clip || !m_active)
        return;

    // Advance time
    TimeUs deltaUs = SecondsToTimeUs(static_cast<double>(context.deltaTime * m_speed));
    m_currentTime += deltaUs;

    // Apply wrap mode
    m_currentTime = ApplyWrapMode(m_currentTime, m_clip->duration, m_wrapMode);

    // Check if finished (for Once mode)
    m_finished = IsAnimationFinished(m_currentTime, m_clip->duration, m_wrapMode);
}

void ClipNode::Reset()
{
    m_currentTime = 0;
    m_finished = false;
}

float ClipNode::GetDuration() const
{
    if (!m_clip)
    {
        return 0.0f;
    }
    return static_cast<float>(TimeUsToSeconds(m_clip->duration));
}

} // namespace RVX::Animation
