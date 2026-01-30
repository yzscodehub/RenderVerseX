#pragma once

/**
 * @file AnimatorComponent.h
 * @brief Animation state machine component
 * 
 * AnimatorComponent manages animation playback through a state machine,
 * allowing blend trees, transitions, and parameter-driven animation.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"
#include <memory>
#include <string>
#include <functional>

// Forward declarations
namespace RVX::Animation
{
    class AnimationStateMachine;
    class AnimationClip;
    struct Skeleton;
    class SkeletonPose;
}

namespace RVX
{

// Forward declarations
class SkeletonComponent;

/**
 * @brief Animation update mode
 */
enum class AnimatorUpdateMode : uint8_t
{
    Normal = 0,         // Update with deltaTime
    UnscaledTime,       // Update ignoring time scale
    AnimatePhysics      // Update in physics step
};

/**
 * @brief Animation culling mode
 */
enum class AnimatorCullingMode : uint8_t
{
    AlwaysAnimate = 0,  // Always update animations
    CullUpdateTransforms,  // Don't update transforms when culled
    CullCompletely      // Stop animation when culled
};

/**
 * @brief Animator component for state machine-based animation
 * 
 * Features:
 * - State machine with transitions
 * - Parameter-driven transitions (float, bool, trigger)
 * - Blend trees support
 * - Root motion extraction
 * - Animation events
 * - IK target support
 * 
 * Usage:
 * @code
 * auto* entity = scene->CreateEntity("Character");
 * entity->AddComponent<SkeletonComponent>()->SetSkeleton(skeleton);
 * 
 * auto* animator = entity->AddComponent<AnimatorComponent>();
 * animator->SetStateMachine(characterFSM);
 * animator->SetFloat("Speed", 5.0f);
 * animator->SetTrigger("Jump");
 * @endcode
 */
class AnimatorComponent : public Component
{
public:
    using AnimationEventCallback = std::function<void(const std::string& eventName)>;

    AnimatorComponent() = default;
    ~AnimatorComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "Animator"; }

    void OnAttach() override;
    void OnDetach() override;
    void Tick(float deltaTime) override;

    // =========================================================================
    // State Machine
    // =========================================================================

    /// Set the animation state machine
    void SetStateMachine(std::shared_ptr<Animation::AnimationStateMachine> fsm);
    std::shared_ptr<Animation::AnimationStateMachine> GetStateMachine() const { return m_stateMachine; }

    /// Start/stop the animator
    void Play();
    void Stop();
    bool IsPlaying() const { return m_playing; }

    // =========================================================================
    // Parameters (for state transitions)
    // =========================================================================

    /// Set a float parameter
    void SetFloat(const std::string& name, float value);
    float GetFloat(const std::string& name) const;

    /// Set a bool parameter
    void SetBool(const std::string& name, bool value);
    bool GetBool(const std::string& name) const;

    /// Set a trigger (auto-resets after evaluation)
    void SetTrigger(const std::string& name);
    void ResetTrigger(const std::string& name);

    /// Set an integer parameter
    void SetInteger(const std::string& name, int value);
    int GetInteger(const std::string& name) const;

    // =========================================================================
    // State Control
    // =========================================================================

    /// Get current state name
    std::string GetCurrentStateName() const;

    /// Force transition to a state
    void CrossFade(const std::string& stateName, float transitionDuration = 0.25f);
    void CrossFadeInFixedTime(const std::string& stateName, float transitionDuration = 0.25f);

    /// Check if in transition
    bool IsInTransition() const;
    float GetTransitionProgress() const;

    // =========================================================================
    // Playback Control
    // =========================================================================

    /// Playback speed multiplier
    float GetSpeed() const { return m_speed; }
    void SetSpeed(float speed) { m_speed = speed; }

    /// Get normalized time of current state (0-1)
    float GetCurrentStateNormalizedTime() const;

    /// Get current state info
    float GetCurrentStateLength() const;

    // =========================================================================
    // Root Motion
    // =========================================================================

    bool ApplyRootMotion() const { return m_applyRootMotion; }
    void SetApplyRootMotion(bool apply) { m_applyRootMotion = apply; }

    /// Get root motion delta since last frame
    Vec3 GetRootMotionDelta() const { return m_rootMotionDelta; }
    Quat GetRootRotationDelta() const { return m_rootRotationDelta; }

    /// Consume and reset root motion delta
    Vec3 ConsumeRootMotion();
    Quat ConsumeRootRotation();

    // =========================================================================
    // Update Settings
    // =========================================================================

    AnimatorUpdateMode GetUpdateMode() const { return m_updateMode; }
    void SetUpdateMode(AnimatorUpdateMode mode) { m_updateMode = mode; }

    AnimatorCullingMode GetCullingMode() const { return m_cullingMode; }
    void SetCullingMode(AnimatorCullingMode mode) { m_cullingMode = mode; }

    // =========================================================================
    // Layer Support (for animation layers/masks)
    // =========================================================================

    /// Set layer weight for blending
    void SetLayerWeight(int layer, float weight);
    float GetLayerWeight(int layer) const;

    /// Get number of layers
    int GetLayerCount() const;

    // =========================================================================
    // IK (Inverse Kinematics)
    // =========================================================================

    /// Set IK target position
    void SetIKPosition(const std::string& ikName, const Vec3& position);
    void SetIKPositionWeight(const std::string& ikName, float weight);

    /// Set IK target rotation
    void SetIKRotation(const std::string& ikName, const Quat& rotation);
    void SetIKRotationWeight(const std::string& ikName, float weight);

    /// Set look-at target
    void SetLookAtPosition(const Vec3& position);
    void SetLookAtWeight(float weight);

    // =========================================================================
    // Events
    // =========================================================================

    /// Register callback for animation events
    void SetOnAnimationEvent(AnimationEventCallback callback);

    /// Register callback for state changes
    void SetOnStateChange(std::function<void(const std::string&, const std::string&)> callback);

    // =========================================================================
    // Output Pose Access
    // =========================================================================

    /// Get the current output pose
    const Animation::SkeletonPose* GetOutputPose() const;

    /// Check if pose is valid
    bool HasValidPose() const;

private:
    void UpdateAnimation(float deltaTime);
    void ApplyPoseToSkeleton();
    void ExtractRootMotion();
    void ProcessAnimationEvents();

    // State machine
    std::shared_ptr<Animation::AnimationStateMachine> m_stateMachine;
    SkeletonComponent* m_skeletonComponent = nullptr;

    // Playback state
    bool m_playing = true;
    float m_speed = 1.0f;

    // Root motion
    bool m_applyRootMotion = false;
    Vec3 m_rootMotionDelta{0.0f};
    Quat m_rootRotationDelta{1.0f, 0.0f, 0.0f, 0.0f};

    // Update settings
    AnimatorUpdateMode m_updateMode = AnimatorUpdateMode::Normal;
    AnimatorCullingMode m_cullingMode = AnimatorCullingMode::AlwaysAnimate;
    bool m_isCulled = false;

    // IK state
    Vec3 m_lookAtPosition{0.0f};
    float m_lookAtWeight = 0.0f;

    // Callbacks
    AnimationEventCallback m_onAnimationEvent;
    std::function<void(const std::string&, const std::string&)> m_onStateChange;

    // Layer weights
    std::vector<float> m_layerWeights;
};

} // namespace RVX
