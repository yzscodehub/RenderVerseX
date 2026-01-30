/**
 * @file AnimationState.cpp
 * @brief AnimationState implementation
 */

#include "Animation/State/AnimationState.h"
#include "Animation/Runtime/AnimationEvaluator.h"
#include "Animation/Core/Interpolation.h"
#include <algorithm>

namespace RVX::Animation
{

AnimationState::AnimationState(const std::string& name)
    : m_name(name)
{
}

void AnimationState::SetMotion(AnimationClip::ConstPtr clip)
{
    m_clip = clip;
    m_blendTree = nullptr;
    m_blendNode = nullptr;
    m_motionType = StateMotionType::Clip;
}

void AnimationState::SetMotion(BlendTree::Ptr blendTree)
{
    m_blendTree = blendTree;
    m_clip = nullptr;
    m_blendNode = nullptr;
    m_motionType = StateMotionType::BlendTree;
}

void AnimationState::SetMotion(BlendNodePtr blendNode)
{
    m_blendNode = blendNode;
    m_clip = nullptr;
    m_blendTree = nullptr;
    m_motionType = StateMotionType::BlendTree;
}

void AnimationState::Enter()
{
    m_currentTime = 0;
    m_finished = false;
    
    if (m_onEnter)
    {
        m_onEnter(this);
    }
}

void AnimationState::Exit()
{
    if (m_onExit)
    {
        m_onExit(this);
    }
}

void AnimationState::Update(const BlendContext& context, float deltaTime)
{
    float actualSpeed = m_speed;
    
    // Apply speed parameter if set
    if (!m_speedParameter.empty())
    {
        actualSpeed *= context.GetParameter(m_speedParameter, 1.0f);
    }

    switch (m_motionType)
    {
        case StateMotionType::Clip:
            if (m_clip)
            {
                TimeUs deltaUs = SecondsToTimeUs(static_cast<double>(deltaTime * actualSpeed));
                m_currentTime += deltaUs;

                if (m_loop)
                {
                    m_currentTime = ApplyWrapMode(m_currentTime, m_clip->duration, WrapMode::Loop);
                }
                else
                {
                    if (m_currentTime >= m_clip->duration)
                    {
                        m_currentTime = m_clip->duration;
                        m_finished = true;
                    }
                }
            }
            break;

        case StateMotionType::BlendTree:
            if (m_blendTree)
            {
                m_blendTree->Update(deltaTime * actualSpeed);
            }
            else if (m_blendNode)
            {
                BlendContext ctx = context;
                ctx.deltaTime = deltaTime * actualSpeed;
                m_blendNode->Update(ctx);
            }
            break;

        default:
            break;
    }

    if (m_onUpdate)
    {
        m_onUpdate(this);
    }
}

float AnimationState::Evaluate(const BlendContext& context, SkeletonPose& outPose)
{
    switch (m_motionType)
    {
        case StateMotionType::Clip:
            if (m_clip)
            {
                AnimationEvaluator evaluator;
                EvaluationOptions options;
                options.wrapModeOverride = m_loop ? WrapMode::Loop : WrapMode::ClampForever;
                evaluator.Evaluate(*m_clip, m_currentTime, outPose, options);
                return 1.0f;
            }
            break;

        case StateMotionType::BlendTree:
            if (m_blendTree)
            {
                // Copy output from blend tree
                outPose.CopyFrom(m_blendTree->GetOutputPose());
                return 1.0f;
            }
            else if (m_blendNode)
            {
                return m_blendNode->Evaluate(context, outPose);
            }
            break;

        default:
            break;
    }

    return 0.0f;
}

float AnimationState::GetNormalizedTime() const
{
    switch (m_motionType)
    {
        case StateMotionType::Clip:
            if (m_clip && m_clip->duration > 0)
            {
                return static_cast<float>(m_currentTime) / static_cast<float>(m_clip->duration);
            }
            break;
        default:
            break;
    }
    return 0.0f;
}

void AnimationState::SetNormalizedTime(float t)
{
    switch (m_motionType)
    {
        case StateMotionType::Clip:
            if (m_clip)
            {
                m_currentTime = static_cast<TimeUs>(t * static_cast<float>(m_clip->duration));
            }
            break;
        default:
            break;
    }
}

void AnimationState::Reset()
{
    m_currentTime = 0;
    m_finished = false;

    if (m_blendTree)
    {
        m_blendTree->Reset();
    }
    if (m_blendNode)
    {
        m_blendNode->Reset();
    }
}

bool AnimationState::HasTag(const std::string& tag) const
{
    return std::find(m_tags.begin(), m_tags.end(), tag) != m_tags.end();
}

float AnimationState::GetLength() const
{
    switch (m_motionType)
    {
        case StateMotionType::Clip:
            if (m_clip)
            {
                return static_cast<float>(TimeUsToSeconds(m_clip->duration));
            }
            break;
        case StateMotionType::BlendTree:
            if (m_blendTree)
            {
                return m_blendTree->GetDuration();
            }
            break;
        default:
            break;
    }
    return 0.0f;
}

} // namespace RVX::Animation
