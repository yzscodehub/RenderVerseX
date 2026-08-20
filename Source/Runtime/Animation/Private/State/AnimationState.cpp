/**
 * @file AnimationState.cpp
 * @brief AnimationState implementation
 */

#include "Animation/State/AnimationState.h"
#include "Animation/Core/Interpolation.h"
#include "Animation/Runtime/AnimationEvaluator.h"
#include <algorithm>
#include <limits>

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

void AnimationState::EnableJobifiedPoseEvaluation(bool enable,
                                                  size_t minTransformTrackCount,
                                                  size_t batchSize)
{
    m_jobifiedPoseEvaluation = enable;
    m_jobifiedMinTransformTrackCount = minTransformTrackCount;
    m_jobifiedBatchSize = batchSize;
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
                const TimeUs previousTime = m_currentTime;
                const TimeUs duration = m_clip->duration;
                const TimeUs deltaUs = SecondsToTimeUs(static_cast<double>(deltaTime * actualSpeed));
                const TimeUs rawTime = m_currentTime + deltaUs;
                const bool reversePlayback = deltaUs < 0;
                bool looped = false;

                if (duration <= 0)
                {
                    m_currentTime = 0;
                }
                else if (m_loop)
                {
                    looped = rawTime >= duration || rawTime < 0;
                    m_currentTime = ApplyWrapMode(rawTime, duration, WrapMode::Loop);
                }
                else if (rawTime >= duration)
                {
                    m_currentTime = duration;
                    m_finished = true;
                }
                else if (rawTime <= 0)
                {
                    m_currentTime = 0;
                    if (reversePlayback)
                    {
                        m_finished = true;
                    }
                }
                else
                {
                    m_currentTime = rawTime;
                }

                DispatchAnimationEvents(previousTime, m_currentTime, looped, reversePlayback);
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

bool AnimationState::CanUpdateTimeUs(const BlendContext& context, TimeUs deltaTimeUs) const
{
    if (deltaTimeUs < 0 || m_motionType != StateMotionType::Clip || !m_clip ||
        m_currentTime > std::numeric_limits<TimeUs>::max() - deltaTimeUs)
    {
        return false;
    }

    float actualSpeed = m_speed;
    if (!m_speedParameter.empty())
    {
        actualSpeed *= context.GetParameter(m_speedParameter, 1.0f);
    }

    return actualSpeed == 1.0f;
}

float AnimationState::GetNormalizedTimeAfterUpdateUs(TimeUs deltaTimeUs) const
{
    if (!m_clip || m_clip->duration <= 0)
    {
        return 0.0f;
    }

    const TimeUs rawTime = m_currentTime + deltaTimeUs;
    TimeUs updatedTime = rawTime;
    if (m_loop)
    {
        updatedTime = ApplyWrapMode(rawTime, m_clip->duration, WrapMode::Loop);
    }
    else if (rawTime >= m_clip->duration)
    {
        updatedTime = m_clip->duration;
    }

    return static_cast<float>(updatedTime) / static_cast<float>(m_clip->duration);
}

bool AnimationState::UpdateTimeUs(const BlendContext& context, TimeUs deltaTimeUs)
{
    if (!CanUpdateTimeUs(context, deltaTimeUs))
    {
        return false;
    }

    const TimeUs previousTime = m_currentTime;
    const TimeUs duration = m_clip->duration;
    const TimeUs rawTime = m_currentTime + deltaTimeUs;
    bool looped = false;

    if (duration <= 0)
    {
        m_currentTime = 0;
    }
    else if (m_loop)
    {
        looped = rawTime >= duration;
        m_currentTime = ApplyWrapMode(rawTime, duration, WrapMode::Loop);
    }
    else if (rawTime >= duration)
    {
        m_currentTime = duration;
        m_finished = true;
    }
    else
    {
        m_currentTime = rawTime;
    }

    DispatchAnimationEvents(previousTime, m_currentTime, looped, false);

    if (m_onUpdate)
    {
        m_onUpdate(this);
    }

    return true;
}

float AnimationState::Evaluate(const BlendContext& context, SkeletonPose& outPose)
{
    m_lastEvaluationUsedJobified = false;

    switch (m_motionType)
    {
        case StateMotionType::Clip:
            if (m_clip)
            {
                AnimationEvaluator evaluator;
                EvaluationOptions options;
                options.wrapModeOverride = m_loop ? WrapMode::Loop : WrapMode::ClampForever;
                options.jobifiedTransformEvaluation = m_jobifiedPoseEvaluation;
                options.jobifiedMinTransformTrackCount = m_jobifiedMinTransformTrackCount;
                options.jobifiedBatchSize = m_jobifiedBatchSize;
                EvaluationResult result = evaluator.Evaluate(*m_clip, m_currentTime, outPose, options);
                m_lastEvaluationUsedJobified = result.usedJobifiedEvaluation;
                return result.success ? 1.0f : 0.0f;
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

void AnimationState::DispatchAnimationEvents(TimeUs previousTime,
                                             TimeUs currentTime,
                                             bool looped,
                                             bool reversePlayback)
{
    if (!m_onAnimationEvent || !m_clip || m_clip->eventTrack.IsEmpty())
    {
        return;
    }

    if (previousTime == currentTime && !looped)
    {
        return;
    }

    const TimeUs duration = m_clip->duration;
    if (duration <= 0)
    {
        return;
    }

    AnimationEventDispatcher dispatcher;
    dispatcher.SetGlobalHandler(m_onAnimationEvent);

    if (reversePlayback)
    {
        if (looped)
        {
            dispatcher.DispatchReverse(m_clip->eventTrack, previousTime, 0);
            dispatcher.DispatchReverse(m_clip->eventTrack, duration, currentTime);
        }
        else
        {
            dispatcher.DispatchReverse(m_clip->eventTrack, previousTime, currentTime);
        }
    }
    else
    {
        dispatcher.Dispatch(m_clip->eventTrack, previousTime, currentTime, looped, duration);
    }
}

} // namespace RVX::Animation
