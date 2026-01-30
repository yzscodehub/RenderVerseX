/**
 * @file AnimationPlayer.cpp
 * @brief AnimationPlayer implementation
 */

#include "Animation/Runtime/AnimationPlayer.h"
#include "Animation/Core/Interpolation.h"
#include <algorithm>

namespace RVX::Animation
{

AnimationPlayer::AnimationPlayer(Skeleton::ConstPtr skeleton)
    : m_skeleton(skeleton)
    , m_currentPose(skeleton)
    , m_tempPose(skeleton)
{
    if (m_skeleton)
    {
        m_currentPose.ResetToBindPose();
        m_tempPose.ResetToBindPose();
    }
}

void AnimationPlayer::SetSkeleton(Skeleton::ConstPtr skeleton)
{
    m_skeleton = skeleton;
    m_currentPose.SetSkeleton(skeleton);
    m_tempPose.SetSkeleton(skeleton);
}

uint32_t AnimationPlayer::Play(AnimationClip::ConstPtr clip, float fadeInTime)
{
    return Play(clip, clip->defaultWrapMode, clip->defaultSpeed, fadeInTime);
}

uint32_t AnimationPlayer::Play(AnimationClip::ConstPtr clip, WrapMode wrapMode, 
                                float speed, float fadeInTime)
{
    if (!clip) return 0;

    PlaybackInstance instance;
    instance.clip = clip;
    instance.currentTime = 0;
    instance.speed = speed;
    instance.wrapMode = wrapMode;
    instance.state = PlaybackState::Playing;
    instance.weight = fadeInTime > 0.0f ? 0.0f : 1.0f;

    if (fadeInTime > 0.0f)
    {
        instance.fadeInDuration = fadeInTime;
        instance.isFadingIn = true;
        instance.fadeProgress = 0.0f;
    }

    uint32_t id = GenerateInstanceId();
    
    // If we're starting fresh, just add
    if (m_instances.empty())
    {
        m_instances.push_back(std::move(instance));
    }
    else
    {
        // Replace existing instances (single animation mode)
        if (fadeInTime <= 0.0f)
        {
            m_instances.clear();
        }
        m_instances.push_back(std::move(instance));
    }

    m_poseDirty = true;
    return id;
}

uint32_t AnimationPlayer::CrossFade(AnimationClip::ConstPtr clip, float fadeDuration)
{
    if (!clip) return 0;

    // Fade out current animations
    for (auto& instance : m_instances)
    {
        if (instance.IsPlaying() && !instance.isFadingOut)
        {
            instance.isFadingOut = true;
            instance.fadeOutDuration = fadeDuration;
            instance.fadeProgress = 0.0f;
        }
    }

    // Add new animation with fade in
    return Play(clip, fadeDuration);
}

void AnimationPlayer::Stop(float fadeOutTime)
{
    for (auto& instance : m_instances)
    {
        if (fadeOutTime > 0.0f)
        {
            instance.isFadingOut = true;
            instance.fadeOutDuration = fadeOutTime;
            instance.fadeProgress = 0.0f;
        }
        else
        {
            instance.state = PlaybackState::Stopped;
        }
    }
}

void AnimationPlayer::Stop(uint32_t instanceId, float fadeOutTime)
{
    // Find instance by id (simplified - just use index for now)
    for (auto& instance : m_instances)
    {
        if (fadeOutTime > 0.0f)
        {
            instance.isFadingOut = true;
            instance.fadeOutDuration = fadeOutTime;
            instance.fadeProgress = 0.0f;
        }
        else
        {
            instance.state = PlaybackState::Stopped;
        }
        break;  // Only affect first match
    }
}

void AnimationPlayer::Pause()
{
    for (auto& instance : m_instances)
    {
        if (instance.IsPlaying())
        {
            instance.state = PlaybackState::Paused;
        }
    }
}

void AnimationPlayer::Resume()
{
    for (auto& instance : m_instances)
    {
        if (instance.IsPaused())
        {
            instance.state = PlaybackState::Playing;
        }
    }
}

void AnimationPlayer::StopAll(float fadeOutTime)
{
    Stop(fadeOutTime);
    m_additiveInstances.clear();
}

bool AnimationPlayer::IsPlaying() const
{
    for (const auto& instance : m_instances)
    {
        if (instance.IsPlaying())
            return true;
    }
    return false;
}

bool AnimationPlayer::IsPlaying(uint32_t instanceId) const
{
    // Simplified
    return !m_instances.empty() && m_instances[0].IsPlaying();
}

PlaybackState AnimationPlayer::GetState() const
{
    if (m_instances.empty())
        return PlaybackState::Stopped;
    return m_instances[0].state;
}

TimeUs AnimationPlayer::GetCurrentTime() const
{
    if (m_instances.empty())
        return 0;
    return m_instances[0].currentTime;
}

float AnimationPlayer::GetNormalizedTime() const
{
    if (m_instances.empty())
        return 0.0f;
    return m_instances[0].GetNormalizedTime();
}

void AnimationPlayer::SetNormalizedTime(float normalizedTime)
{
    for (auto& instance : m_instances)
    {
        if (instance.clip)
        {
            instance.currentTime = static_cast<TimeUs>(
                normalizedTime * static_cast<float>(instance.clip->duration));
        }
    }
    m_poseDirty = true;
}

AnimationClip::ConstPtr AnimationPlayer::GetCurrentClip() const
{
    if (m_instances.empty())
        return nullptr;
    return m_instances[0].clip;
}

void AnimationPlayer::SetSpeed(float speed)
{
    m_globalSpeed = speed;
}

void AnimationPlayer::SetSpeed(uint32_t instanceId, float speed)
{
    if (!m_instances.empty())
    {
        m_instances[0].speed = speed;
    }
}

void AnimationPlayer::Update(float deltaTime)
{
    // Update all instances
    for (auto& instance : m_instances)
    {
        UpdateInstance(instance, deltaTime);
    }

    // Update additive instances
    for (auto& instance : m_additiveInstances)
    {
        UpdateInstance(instance, deltaTime);
    }

    // Clean up finished instances
    CleanupFinishedInstances();

    // Evaluate and blend poses
    EvaluateAndBlend();
}

void AnimationPlayer::UpdateInstance(PlaybackInstance& instance, float deltaTime)
{
    if (!instance.clip || !instance.IsPlaying())
        return;

    // Update time
    float speed = instance.speed * m_globalSpeed;
    TimeUs deltaUs = SecondsToTimeUs(static_cast<double>(deltaTime * speed));
    
    TimeUs previousTime = instance.currentTime;
    instance.currentTime += deltaUs;

    // Handle wrap mode
    if (instance.currentTime >= instance.clip->duration)
    {
        switch (instance.wrapMode)
        {
            case WrapMode::Once:
                instance.currentTime = instance.clip->duration;
                instance.state = PlaybackState::Stopped;
                if (instance.onComplete)
                    instance.onComplete();
                break;

            case WrapMode::Loop:
                instance.currentTime = instance.currentTime % instance.clip->duration;
                if (instance.onLoop)
                    instance.onLoop();
                break;

            case WrapMode::PingPong:
                instance.currentTime = ApplyWrapMode(
                    instance.currentTime, 
                    instance.clip->duration, 
                    WrapMode::PingPong);
                break;

            case WrapMode::ClampForever:
                instance.currentTime = instance.clip->duration;
                break;
        }
    }

    // Handle fade in
    if (instance.isFadingIn && instance.fadeInDuration > 0.0f)
    {
        instance.fadeProgress += deltaTime / instance.fadeInDuration;
        if (instance.fadeProgress >= 1.0f)
        {
            instance.fadeProgress = 1.0f;
            instance.isFadingIn = false;
            instance.weight = 1.0f;
        }
        else
        {
            // Smooth step for nicer fade
            float t = instance.fadeProgress;
            instance.weight = t * t * (3.0f - 2.0f * t);
        }
    }

    // Handle fade out
    if (instance.isFadingOut && instance.fadeOutDuration > 0.0f)
    {
        instance.fadeProgress += deltaTime / instance.fadeOutDuration;
        if (instance.fadeProgress >= 1.0f)
        {
            instance.fadeProgress = 1.0f;
            instance.isFadingOut = false;
            instance.state = PlaybackState::Stopped;
            instance.weight = 0.0f;
        }
        else
        {
            float t = 1.0f - instance.fadeProgress;
            instance.weight = t * t * (3.0f - 2.0f * t);
        }
    }

    m_poseDirty = true;
}

void AnimationPlayer::EvaluateAndBlend()
{
    if (!m_poseDirty)
        return;

    if (m_instances.empty())
    {
        m_currentPose.ResetToBindPose();
        m_poseDirty = false;
        return;
    }

    // Start with bind pose
    m_currentPose.ResetToBindPose();

    // Calculate total weight
    float totalWeight = 0.0f;
    for (const auto& instance : m_instances)
    {
        if (instance.IsPlaying() || instance.isFadingOut)
        {
            totalWeight += instance.weight;
        }
    }

    // Blend all instances
    for (const auto& instance : m_instances)
    {
        if (!instance.clip || instance.weight <= 0.001f)
            continue;

        if (!(instance.IsPlaying() || instance.isFadingOut))
            continue;

        float normalizedWeight = totalWeight > 0.0f 
            ? instance.weight / totalWeight 
            : instance.weight;

        EvaluationOptions options;
        options.wrapModeOverride = instance.wrapMode;
        options.speed = instance.speed;

        m_evaluator.EvaluateBlended(
            *instance.clip, 
            instance.currentTime, 
            normalizedWeight,
            m_currentPose, 
            options);
    }

    // Apply additive animations
    for (const auto& instance : m_additiveInstances)
    {
        if (!instance.clip || !instance.IsPlaying())
            continue;

        EvaluationOptions options;
        m_evaluator.EvaluateAdditive(
            *instance.clip,
            instance.currentTime,
            instance.weight,
            m_currentPose,
            options);
    }

    m_poseDirty = false;
}

void AnimationPlayer::CleanupFinishedInstances()
{
    m_instances.erase(
        std::remove_if(m_instances.begin(), m_instances.end(),
            [](const PlaybackInstance& instance) {
                return instance.IsStopped() && instance.weight <= 0.001f;
            }),
        m_instances.end());

    m_additiveInstances.erase(
        std::remove_if(m_additiveInstances.begin(), m_additiveInstances.end(),
            [](const PlaybackInstance& instance) {
                return instance.IsStopped();
            }),
        m_additiveInstances.end());
}

uint32_t AnimationPlayer::PlayAdditive(AnimationClip::ConstPtr clip, float weight)
{
    if (!clip) return 0;

    PlaybackInstance instance;
    instance.clip = clip;
    instance.currentTime = 0;
    instance.speed = 1.0f;
    instance.weight = weight;
    instance.wrapMode = WrapMode::Loop;
    instance.state = PlaybackState::Playing;
    instance.isAdditive = true;

    m_additiveInstances.push_back(std::move(instance));
    m_poseDirty = true;

    return GenerateInstanceId();
}

void AnimationPlayer::SetAdditiveWeight(uint32_t instanceId, float weight)
{
    if (!m_additiveInstances.empty())
    {
        m_additiveInstances[0].weight = weight;
        m_poseDirty = true;
    }
}

PlaybackInstance* AnimationPlayer::GetInstance(uint32_t instanceId)
{
    if (!m_instances.empty())
        return &m_instances[0];
    return nullptr;
}

const PlaybackInstance* AnimationPlayer::GetInstance(uint32_t instanceId) const
{
    if (!m_instances.empty())
        return &m_instances[0];
    return nullptr;
}

uint32_t AnimationPlayer::GenerateInstanceId()
{
    return m_nextInstanceId++;
}

} // namespace RVX::Animation
