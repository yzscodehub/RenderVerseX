#include "Scene/Components/AnimatorComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/Components/SkeletonComponent.h"
#include "Animation/State/AnimationStateMachine.h"
#include "Animation/Runtime/SkeletonPose.h"

namespace RVX
{

void AnimatorComponent::OnAttach()
{
    // Find skeleton component on same entity
    if (SceneEntity* owner = GetOwner())
    {
        m_skeletonComponent = owner->GetComponent<SkeletonComponent>();
    }

    // Initialize state machine if set
    if (m_stateMachine)
    {
        m_stateMachine->Start();
    }
}

void AnimatorComponent::OnDetach()
{
    if (m_stateMachine)
    {
        m_stateMachine->Stop();
    }
    m_skeletonComponent = nullptr;
}

void AnimatorComponent::Tick(float deltaTime)
{
    if (!m_playing || !m_stateMachine)
    {
        return;
    }

    // Handle culling
    if (m_isCulled)
    {
        if (m_cullingMode == AnimatorCullingMode::CullCompletely)
        {
            return;
        }
    }

    UpdateAnimation(deltaTime * m_speed);
}

void AnimatorComponent::SetStateMachine(std::shared_ptr<Animation::AnimationStateMachine> fsm)
{
    // Stop old state machine
    if (m_stateMachine && m_stateMachine->IsRunning())
    {
        m_stateMachine->Stop();
    }

    m_stateMachine = fsm;

    // Start new state machine if we're playing
    if (m_stateMachine && m_playing)
    {
        m_stateMachine->Start();
    }
}

void AnimatorComponent::Play()
{
    m_playing = true;
    if (m_stateMachine && !m_stateMachine->IsRunning())
    {
        m_stateMachine->Start();
    }
}

void AnimatorComponent::Stop()
{
    m_playing = false;
    if (m_stateMachine)
    {
        m_stateMachine->Stop();
    }
}

void AnimatorComponent::SetFloat(const std::string& name, float value)
{
    if (m_stateMachine)
    {
        m_stateMachine->SetFloat(name, value);
    }
}

float AnimatorComponent::GetFloat(const std::string& name) const
{
    if (m_stateMachine)
    {
        return m_stateMachine->GetFloat(name);
    }
    return 0.0f;
}

void AnimatorComponent::SetBool(const std::string& name, bool value)
{
    if (m_stateMachine)
    {
        m_stateMachine->SetBool(name, value);
    }
}

bool AnimatorComponent::GetBool(const std::string& name) const
{
    if (m_stateMachine)
    {
        return m_stateMachine->GetBool(name);
    }
    return false;
}

void AnimatorComponent::SetTrigger(const std::string& name)
{
    if (m_stateMachine)
    {
        m_stateMachine->SetTrigger(name);
    }
}

void AnimatorComponent::ResetTrigger(const std::string& name)
{
    if (m_stateMachine)
    {
        m_stateMachine->ResetTrigger(name);
    }
}

void AnimatorComponent::SetInteger(const std::string& name, int value)
{
    // Store as float internally (state machine uses floats)
    SetFloat(name, static_cast<float>(value));
}

int AnimatorComponent::GetInteger(const std::string& name) const
{
    return static_cast<int>(GetFloat(name));
}

std::string AnimatorComponent::GetCurrentStateName() const
{
    if (m_stateMachine && m_stateMachine->GetCurrentState())
    {
        return m_stateMachine->GetCurrentState()->GetName();
    }
    return "";
}

void AnimatorComponent::CrossFade(const std::string& stateName, float transitionDuration)
{
    if (m_stateMachine)
    {
        m_stateMachine->ForceState(stateName, transitionDuration);
    }
}

void AnimatorComponent::CrossFadeInFixedTime(const std::string& stateName, float transitionDuration)
{
    // Same as CrossFade for now - fixed time means duration is absolute, not relative
    CrossFade(stateName, transitionDuration);
}

bool AnimatorComponent::IsInTransition() const
{
    if (m_stateMachine)
    {
        return m_stateMachine->IsInTransition();
    }
    return false;
}

float AnimatorComponent::GetTransitionProgress() const
{
    if (m_stateMachine)
    {
        return m_stateMachine->GetTransitionProgress();
    }
    return 0.0f;
}

float AnimatorComponent::GetCurrentStateNormalizedTime() const
{
    if (m_stateMachine && m_stateMachine->GetCurrentState())
    {
        return m_stateMachine->GetCurrentState()->GetNormalizedTime();
    }
    return 0.0f;
}

float AnimatorComponent::GetCurrentStateLength() const
{
    if (m_stateMachine && m_stateMachine->GetCurrentState())
    {
        return m_stateMachine->GetCurrentState()->GetLength();
    }
    return 0.0f;
}

Vec3 AnimatorComponent::ConsumeRootMotion()
{
    Vec3 delta = m_rootMotionDelta;
    m_rootMotionDelta = Vec3(0.0f);
    return delta;
}

Quat AnimatorComponent::ConsumeRootRotation()
{
    Quat delta = m_rootRotationDelta;
    m_rootRotationDelta = Quat(1.0f, 0.0f, 0.0f, 0.0f);
    return delta;
}

void AnimatorComponent::SetLayerWeight(int layer, float weight)
{
    if (layer >= static_cast<int>(m_layerWeights.size()))
    {
        m_layerWeights.resize(layer + 1, 1.0f);
    }
    m_layerWeights[layer] = weight;
}

float AnimatorComponent::GetLayerWeight(int layer) const
{
    if (layer < static_cast<int>(m_layerWeights.size()))
    {
        return m_layerWeights[layer];
    }
    return 1.0f;
}

int AnimatorComponent::GetLayerCount() const
{
    return static_cast<int>(m_layerWeights.size());
}

void AnimatorComponent::SetIKPosition(const std::string& ikName, const Vec3& position)
{
    // TODO: Implement IK system integration
    (void)ikName;
    (void)position;
}

void AnimatorComponent::SetIKPositionWeight(const std::string& ikName, float weight)
{
    (void)ikName;
    (void)weight;
}

void AnimatorComponent::SetIKRotation(const std::string& ikName, const Quat& rotation)
{
    (void)ikName;
    (void)rotation;
}

void AnimatorComponent::SetIKRotationWeight(const std::string& ikName, float weight)
{
    (void)ikName;
    (void)weight;
}

void AnimatorComponent::SetLookAtPosition(const Vec3& position)
{
    m_lookAtPosition = position;
}

void AnimatorComponent::SetLookAtWeight(float weight)
{
    m_lookAtWeight = weight;
}

void AnimatorComponent::SetOnAnimationEvent(AnimationEventCallback callback)
{
    m_onAnimationEvent = std::move(callback);
}

void AnimatorComponent::SetOnStateChange(std::function<void(const std::string&, const std::string&)> callback)
{
    m_onStateChange = std::move(callback);

    // Register with state machine
    if (m_stateMachine)
    {
        m_stateMachine->SetOnStateChange([this](Animation::AnimationState* from, Animation::AnimationState* to) {
            if (m_onStateChange)
            {
                std::string fromName = from ? from->GetName() : "";
                std::string toName = to ? to->GetName() : "";
                m_onStateChange(fromName, toName);
            }
        });
    }
}

const Animation::SkeletonPose* AnimatorComponent::GetOutputPose() const
{
    if (m_stateMachine)
    {
        return &m_stateMachine->GetOutputPose();
    }
    return nullptr;
}

bool AnimatorComponent::HasValidPose() const
{
    return m_stateMachine != nullptr && m_stateMachine->GetSkeleton() != nullptr;
}

void AnimatorComponent::UpdateAnimation(float deltaTime)
{
    if (!m_stateMachine)
    {
        return;
    }

    // Update state machine
    m_stateMachine->Update(deltaTime);

    // Extract root motion if enabled
    if (m_applyRootMotion)
    {
        ExtractRootMotion();
    }

    // Apply pose to skeleton
    if (!m_isCulled || m_cullingMode != AnimatorCullingMode::CullUpdateTransforms)
    {
        ApplyPoseToSkeleton();
    }

    // Process animation events
    ProcessAnimationEvents();
}

void AnimatorComponent::ApplyPoseToSkeleton()
{
    if (!m_skeletonComponent || !m_stateMachine)
    {
        return;
    }

    // Get output pose and apply to skeleton component
    const Animation::SkeletonPose& pose = m_stateMachine->GetOutputPose();
    m_skeletonComponent->SetPose(pose);
}

void AnimatorComponent::ExtractRootMotion()
{
    // TODO: Extract root motion from the current animation
    // This would involve:
    // 1. Getting the root bone's transform delta
    // 2. Storing it in m_rootMotionDelta and m_rootRotationDelta
    // 3. Optionally zeroing out root bone movement in the pose
}

void AnimatorComponent::ProcessAnimationEvents()
{
    // TODO: Check for animation events in current clip
    // and fire callbacks
}

} // namespace RVX
