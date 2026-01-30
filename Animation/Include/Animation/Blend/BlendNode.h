/**
 * @file BlendNode.h
 * @brief Base class for blend tree nodes
 * 
 * Blend nodes form a tree structure for combining animations.
 * Each node can have inputs and produces an output pose.
 */

#pragma once

#include "Animation/Runtime/SkeletonPose.h"
#include "Animation/Core/Types.h"
#include "Animation/Data/AnimationClip.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace RVX::Animation
{

class BlendNode;
using BlendNodePtr = std::shared_ptr<BlendNode>;

/**
 * @brief Context passed to blend nodes during evaluation
 */
struct BlendContext
{
    /// Delta time for this update
    float deltaTime = 0.0f;
    
    /// Parameters that can be used by nodes
    std::unordered_map<std::string, float> parameters;
    
    /// Get a parameter value with default
    float GetParameter(const std::string& name, float defaultValue = 0.0f) const
    {
        auto it = parameters.find(name);
        return (it != parameters.end()) ? it->second : defaultValue;
    }
    
    /// Set a parameter value
    void SetParameter(const std::string& name, float value)
    {
        parameters[name] = value;
    }
};

/**
 * @brief Base class for all blend tree nodes
 * 
 * Blend nodes form a directed acyclic graph (DAG) that produces
 * a final pose from multiple animation sources.
 */
class BlendNode
{
public:
    using Ptr = std::shared_ptr<BlendNode>;

    virtual ~BlendNode() = default;

    // =========================================================================
    // Identity
    // =========================================================================

    /// Get node type name
    virtual const char* GetTypeName() const = 0;

    /// Get instance name
    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }

    // =========================================================================
    // Evaluation
    // =========================================================================

    /**
     * @brief Evaluate this node to produce an output pose
     * @param context Evaluation context with parameters
     * @param outPose Output pose (modified in place)
     * @return Weight of this node's contribution (0-1)
     */
    virtual float Evaluate(const BlendContext& context, SkeletonPose& outPose) = 0;

    /**
     * @brief Update internal state (called before Evaluate)
     */
    virtual void Update(const BlendContext& context) { (void)context; }

    /**
     * @brief Reset node state
     */
    virtual void Reset() {}

    /**
     * @brief Get duration of this node's content in seconds
     */
    virtual float GetDuration() const { return 0.0f; }

    // =========================================================================
    // Properties
    // =========================================================================

    /// Get the output weight of this node
    float GetWeight() const { return m_weight; }
    void SetWeight(float weight) { m_weight = weight; }

    /// Is this node active
    bool IsActive() const { return m_active && m_weight > 0.001f; }
    void SetActive(bool active) { m_active = active; }

    // =========================================================================
    // Hierarchy (for composite nodes)
    // =========================================================================

    /// Get child nodes
    virtual std::vector<BlendNode*> GetChildren() { return {}; }

    /// Get child count
    virtual size_t GetChildCount() const { return 0; }

protected:
    std::string m_name;
    float m_weight = 1.0f;
    bool m_active = true;
};

/**
 * @brief Clip node - plays a single animation clip
 */
class ClipNode : public BlendNode
{
public:
    ClipNode() = default;
    explicit ClipNode(AnimationClip::ConstPtr clip);

    const char* GetTypeName() const override { return "ClipNode"; }

    // =========================================================================
    // Clip Control
    // =========================================================================

    void SetClip(AnimationClip::ConstPtr clip);
    AnimationClip::ConstPtr GetClip() const { return m_clip; }

    /// Set playback speed
    void SetSpeed(float speed) { m_speed = speed; }
    float GetSpeed() const { return m_speed; }

    /// Set wrap mode
    void SetWrapMode(WrapMode mode) { m_wrapMode = mode; }
    WrapMode GetWrapMode() const { return m_wrapMode; }

    /// Get current time
    TimeUs GetCurrentTime() const { return m_currentTime; }
    void SetCurrentTime(TimeUs time) { m_currentTime = time; }

    /// Get normalized time (0-1)
    float GetNormalizedTime() const;
    void SetNormalizedTime(float t);

    /// Is the animation finished (for Once mode)
    bool IsFinished() const { return m_finished; }

    // =========================================================================
    // BlendNode Interface
    // =========================================================================

    float Evaluate(const BlendContext& context, SkeletonPose& outPose) override;
    void Update(const BlendContext& context) override;
    void Reset() override;
    float GetDuration() const override;

private:
    AnimationClip::ConstPtr m_clip;
    TimeUs m_currentTime = 0;
    float m_speed = 1.0f;
    WrapMode m_wrapMode = WrapMode::Loop;
    bool m_finished = false;
};

} // namespace RVX::Animation
