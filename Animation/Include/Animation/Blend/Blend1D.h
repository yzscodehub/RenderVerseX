/**
 * @file Blend1D.h
 * @brief 1D blend space node
 * 
 * Blends between animations based on a single parameter.
 * Common use: walk/run blending based on speed.
 */

#pragma once

#include "Animation/Blend/BlendNode.h"
#include <algorithm>

namespace RVX::Animation
{

/**
 * @brief Entry in a 1D blend space
 */
struct BlendEntry1D
{
    float position = 0.0f;      ///< Position on the blend axis
    BlendNodePtr node;           ///< Animation node at this position
    
    BlendEntry1D() = default;
    BlendEntry1D(float pos, BlendNodePtr n) : position(pos), node(std::move(n)) {}
};

/**
 * @brief 1D blend space
 * 
 * Blends between multiple animations arranged on a single axis.
 * The blend parameter determines which animations are active and
 * their relative weights.
 * 
 * Example: Speed-based locomotion
 * - Position 0: Idle
 * - Position 1: Walk
 * - Position 2: Run
 * - Parameter "Speed" controls the blend
 */
class Blend1D : public BlendNode
{
public:
    Blend1D() = default;
    explicit Blend1D(const std::string& parameterName);

    const char* GetTypeName() const override { return "Blend1D"; }

    // =========================================================================
    // Configuration
    // =========================================================================

    /// Set the parameter name to read for blending
    void SetParameterName(const std::string& name) { m_parameterName = name; }
    const std::string& GetParameterName() const { return m_parameterName; }

    /// Add an entry to the blend space
    void AddEntry(float position, BlendNodePtr node);

    /// Add a clip entry (convenience)
    void AddClip(float position, AnimationClip::ConstPtr clip);

    /// Remove an entry
    void RemoveEntry(size_t index);

    /// Clear all entries
    void ClearEntries();

    /// Get entries
    const std::vector<BlendEntry1D>& GetEntries() const { return m_entries; }

    /// Sort entries by position (call after adding entries)
    void SortEntries();

    // =========================================================================
    // BlendNode Interface
    // =========================================================================

    float Evaluate(const BlendContext& context, SkeletonPose& outPose) override;
    void Update(const BlendContext& context) override;
    void Reset() override;

    std::vector<BlendNode*> GetChildren() override;
    size_t GetChildCount() const override { return m_entries.size(); }

    // =========================================================================
    // Query
    // =========================================================================

    /// Get the current blend value
    float GetCurrentValue() const { return m_currentValue; }

    /// Get range
    float GetMinPosition() const;
    float GetMaxPosition() const;

private:
    void CalculateWeights(float value);

    std::string m_parameterName = "BlendValue";
    std::vector<BlendEntry1D> m_entries;
    std::vector<float> m_weights;  // Calculated weights per entry
    float m_currentValue = 0.0f;
    bool m_sorted = false;
};

} // namespace RVX::Animation
