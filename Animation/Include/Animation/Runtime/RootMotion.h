/**
 * @file RootMotion.h
 * @brief Root motion extraction and application system
 * 
 * Root motion allows extracting movement from animation data and applying
 * it to entity transforms, enabling realistic character locomotion.
 * 
 * Supports:
 * - Translation extraction (XZ, XYZ)
 * - Rotation extraction (Y-axis, full)
 * - Delta mode and absolute mode
 * - Blending with multiple animations
 */

#pragma once

#include "Animation/Core/Types.h"
#include "Animation/Core/TransformSample.h"
#include "Animation/Data/AnimationClip.h"
#include "Animation/Data/Skeleton.h"
#include "Core/MathTypes.h"
#include <memory>
#include <string>

namespace RVX::Animation
{

// ============================================================================
// Root Motion Configuration
// ============================================================================

/**
 * @brief Root motion extraction mode for translation
 */
enum class RootMotionTranslationMode : uint8_t
{
    None,           ///< Don't extract translation
    XZ,             ///< Extract horizontal movement only (XZ plane)
    XYZ,            ///< Extract full 3D translation
    XOnly,          ///< Extract X-axis only
    ZOnly           ///< Extract Z-axis only
};

/**
 * @brief Root motion extraction mode for rotation
 */
enum class RootMotionRotationMode : uint8_t
{
    None,           ///< Don't extract rotation
    YawOnly,        ///< Extract Y-axis rotation only (most common)
    Full            ///< Extract full rotation
};

/**
 * @brief Root motion configuration
 */
struct RootMotionConfig
{
    /// Whether root motion extraction is enabled
    bool enabled = true;

    /// Translation extraction mode
    RootMotionTranslationMode translationMode = RootMotionTranslationMode::XZ;

    /// Rotation extraction mode
    RootMotionRotationMode rotationMode = RootMotionRotationMode::YawOnly;

    /// Name of the root bone (empty = use first root bone)
    std::string rootBoneName;

    /// Root bone index (-1 = auto-detect)
    int rootBoneIndex = -1;

    /// Scale factor for extracted motion
    float motionScale = 1.0f;

    /// Whether to zero out the root bone in the pose after extraction
    bool zeroRootBone = true;

    /// Apply motion relative to character facing
    bool applyRelativeToFacing = true;
};

// ============================================================================
// Root Motion Delta
// ============================================================================

/**
 * @brief Root motion delta for a frame
 */
struct RootMotionDelta
{
    /// Translation delta in world space
    Vec3 deltaTranslation{0.0f};

    /// Rotation delta (quaternion)
    Quat deltaRotation{1.0f, 0.0f, 0.0f, 0.0f};

    /// Whether this delta is valid
    bool valid = false;

    // ========================================================================
    // Construction
    // ========================================================================

    RootMotionDelta() = default;

    RootMotionDelta(const Vec3& translation, const Quat& rotation)
        : deltaTranslation(translation)
        , deltaRotation(rotation)
        , valid(true) {}

    // ========================================================================
    // Operations
    // ========================================================================

    /**
     * @brief Combine with another delta
     */
    RootMotionDelta operator+(const RootMotionDelta& other) const
    {
        if (!valid) return other;
        if (!other.valid) return *this;

        RootMotionDelta result;
        result.deltaTranslation = deltaTranslation + other.deltaTranslation;
        result.deltaRotation = normalize(deltaRotation * other.deltaRotation);
        result.valid = true;
        return result;
    }

    RootMotionDelta& operator+=(const RootMotionDelta& other)
    {
        if (other.valid)
        {
            deltaTranslation += other.deltaTranslation;
            deltaRotation = normalize(deltaRotation * other.deltaRotation);
            valid = true;
        }
        return *this;
    }

    /**
     * @brief Scale the delta
     */
    RootMotionDelta operator*(float scale) const
    {
        RootMotionDelta result;
        result.deltaTranslation = deltaTranslation * scale;
        
        // Slerp rotation with identity for scaling
        Quat identity{1.0f, 0.0f, 0.0f, 0.0f};
        result.deltaRotation = slerp(identity, deltaRotation, scale);
        result.valid = valid;
        return result;
    }

    /**
     * @brief Blend with another delta
     */
    static RootMotionDelta Blend(const RootMotionDelta& a, const RootMotionDelta& b, float t)
    {
        if (!a.valid) return b;
        if (!b.valid) return a;

        RootMotionDelta result;
        result.deltaTranslation = mix(a.deltaTranslation, b.deltaTranslation, t);
        result.deltaRotation = slerp(a.deltaRotation, b.deltaRotation, t);
        result.valid = true;
        return result;
    }

    /**
     * @brief Get yaw angle in radians
     */
    float GetYawAngle() const
    {
        Vec3 euler = glm::eulerAngles(deltaRotation);
        return euler.y;
    }

    /**
     * @brief Convert to transform sample
     */
    TransformSample ToTransformSample() const
    {
        return TransformSample(deltaTranslation, deltaRotation, Vec3(1.0f));
    }

    /**
     * @brief Check if delta is effectively zero
     */
    bool IsNearZero(float epsilon = 1e-6f) const
    {
        bool tZero = length(deltaTranslation) < epsilon;
        bool rIdentity = std::abs(deltaRotation.w - 1.0f) < epsilon;
        return tZero && rIdentity;
    }

    /**
     * @brief Create identity delta (no motion)
     */
    static RootMotionDelta Identity()
    {
        RootMotionDelta result;
        result.valid = true;
        return result;
    }
};

// ============================================================================
// Root Motion Extractor
// ============================================================================

/**
 * @brief Extracts root motion from animation clips
 * 
 * Usage:
 * @code
 * RootMotionExtractor extractor(skeleton);
 * extractor.SetConfig(config);
 * 
 * // During update
 * RootMotionDelta delta = extractor.ExtractDelta(clip, previousTime, currentTime);
 * 
 * // Apply to character
 * characterPosition += delta.deltaTranslation;
 * characterRotation *= delta.deltaRotation;
 * @endcode
 */
class RootMotionExtractor
{
public:
    using Ptr = std::shared_ptr<RootMotionExtractor>;

    // =========================================================================
    // Construction
    // =========================================================================

    RootMotionExtractor() = default;
    explicit RootMotionExtractor(Skeleton::ConstPtr skeleton);

    static Ptr Create(Skeleton::ConstPtr skeleton = nullptr)
    {
        return std::make_shared<RootMotionExtractor>(skeleton);
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    void SetSkeleton(Skeleton::ConstPtr skeleton);
    Skeleton::ConstPtr GetSkeleton() const { return m_skeleton; }

    void SetConfig(const RootMotionConfig& config);
    const RootMotionConfig& GetConfig() const { return m_config; }

    void SetEnabled(bool enabled) { m_config.enabled = enabled; }
    bool IsEnabled() const { return m_config.enabled; }

    void SetTranslationMode(RootMotionTranslationMode mode) { m_config.translationMode = mode; }
    void SetRotationMode(RootMotionRotationMode mode) { m_config.rotationMode = mode; }

    void SetMotionScale(float scale) { m_config.motionScale = scale; }
    float GetMotionScale() const { return m_config.motionScale; }

    // =========================================================================
    // Root Bone Management
    // =========================================================================

    /**
     * @brief Set root bone by name
     */
    void SetRootBone(const std::string& boneName);

    /**
     * @brief Set root bone by index
     */
    void SetRootBone(int boneIndex);

    /**
     * @brief Get the current root bone index
     */
    int GetRootBoneIndex() const { return m_config.rootBoneIndex; }

    /**
     * @brief Auto-detect root bone from skeleton
     */
    void AutoDetectRootBone();

    // =========================================================================
    // Extraction
    // =========================================================================

    /**
     * @brief Extract root motion delta between two times
     * @param clip Animation clip
     * @param previousTime Previous frame time
     * @param currentTime Current frame time
     * @return Root motion delta
     */
    RootMotionDelta ExtractDelta(const AnimationClip& clip,
                                  TimeUs previousTime,
                                  TimeUs currentTime) const;

    /**
     * @brief Extract root motion at a specific time (relative to start)
     * @param clip Animation clip
     * @param time Current time
     * @return Root motion from animation start to this time
     */
    RootMotionDelta ExtractAbsolute(const AnimationClip& clip, TimeUs time) const;

    /**
     * @brief Extract root motion for entire clip
     * @param clip Animation clip
     * @return Total root motion for the animation
     */
    RootMotionDelta ExtractTotal(const AnimationClip& clip) const;

    /**
     * @brief Sample root bone transform at a specific time
     * @param clip Animation clip
     * @param time Sample time
     * @return Root bone transform at time
     */
    TransformSample SampleRootTransform(const AnimationClip& clip, TimeUs time) const;

    // =========================================================================
    // Pose Modification
    // =========================================================================

    /**
     * @brief Remove root motion from a pose (zero out root bone motion)
     * @param pose Pose to modify
     * @param keepVertical Whether to keep vertical (Y) motion in pose
     */
    void ZeroRootMotion(TransformSample& rootTransform, bool keepVertical = true) const;

    /**
     * @brief Apply extracted root motion back to a pose
     * @param pose Pose to modify
     * @param delta Root motion to apply
     */
    void ApplyRootMotion(TransformSample& rootTransform, const RootMotionDelta& delta) const;

private:
    // Helpers
    TransformSample GetRootSampleFromClip(const AnimationClip& clip, TimeUs time) const;
    Vec3 FilterTranslation(const Vec3& translation) const;
    Quat FilterRotation(const Quat& rotation) const;
    int FindRootBoneIndex() const;

    Skeleton::ConstPtr m_skeleton;
    RootMotionConfig m_config;
    
    // Cached reference pose for delta calculations
    mutable TransformSample m_referenceRootPose;
    mutable bool m_referenceValid = false;
};

// ============================================================================
// Root Motion Accumulator
// ============================================================================

/**
 * @brief Accumulates root motion from multiple animations (for blending)
 */
class RootMotionAccumulator
{
public:
    RootMotionAccumulator() = default;

    /**
     * @brief Reset accumulator for new frame
     */
    void Reset()
    {
        m_accumulatedDelta = RootMotionDelta();
        m_totalWeight = 0.0f;
    }

    /**
     * @brief Accumulate delta with weight
     */
    void Accumulate(const RootMotionDelta& delta, float weight)
    {
        if (!delta.valid || weight <= 0.0f)
            return;

        if (m_totalWeight <= 0.0f)
        {
            m_accumulatedDelta = delta * weight;
            m_totalWeight = weight;
        }
        else
        {
            m_accumulatedDelta += delta * weight;
            m_totalWeight += weight;
        }
    }

    /**
     * @brief Get normalized accumulated delta
     */
    RootMotionDelta GetNormalizedDelta() const
    {
        if (m_totalWeight <= 0.0f)
            return RootMotionDelta();

        // Normalize by total weight
        return m_accumulatedDelta * (1.0f / m_totalWeight);
    }

    /**
     * @brief Get raw accumulated delta (not normalized)
     */
    const RootMotionDelta& GetRawDelta() const { return m_accumulatedDelta; }

    float GetTotalWeight() const { return m_totalWeight; }

private:
    RootMotionDelta m_accumulatedDelta;
    float m_totalWeight = 0.0f;
};

} // namespace RVX::Animation
