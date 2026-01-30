/**
 * @file Blend2D.h
 * @brief 2D blend space node
 * 
 * Blends between animations based on two parameters.
 * Common use: directional locomotion (forward/strafe blend).
 */

#pragma once

#include "Animation/Blend/BlendNode.h"
#include "Core/MathTypes.h"
#include <algorithm>

namespace RVX::Animation
{

/**
 * @brief Entry in a 2D blend space
 */
struct BlendEntry2D
{
    Vec2 position{0.0f};        ///< Position in 2D blend space
    BlendNodePtr node;           ///< Animation node at this position
    float cachedWeight = 0.0f;   ///< Cached weight from last evaluation
    
    BlendEntry2D() = default;
    BlendEntry2D(Vec2 pos, BlendNodePtr n) : position(pos), node(std::move(n)) {}
    BlendEntry2D(float x, float y, BlendNodePtr n) : position(x, y), node(std::move(n)) {}
};

/**
 * @brief 2D blend space types
 */
enum class Blend2DType
{
    FreeformDirectional,    ///< Based on direction, normalized
    FreeformCartesian,      ///< Based on position, not normalized
    SimpleDirectional       ///< 4/8 direction simplified blend
};

/**
 * @brief 2D blend space
 * 
 * Blends between animations arranged in a 2D space.
 * Uses Delaunay triangulation or gradient band interpolation.
 * 
 * Example: Directional locomotion
 * - (0, 1): Walk forward
 * - (0, -1): Walk backward
 * - (1, 0): Strafe right
 * - (-1, 0): Strafe left
 * - Parameters "DirectionX", "DirectionY" control the blend
 */
class Blend2D : public BlendNode
{
public:
    Blend2D() = default;
    Blend2D(const std::string& paramX, const std::string& paramY);

    const char* GetTypeName() const override { return "Blend2D"; }

    // =========================================================================
    // Configuration
    // =========================================================================

    /// Set parameter names
    void SetParameterNames(const std::string& paramX, const std::string& paramY);
    const std::string& GetParameterNameX() const { return m_parameterX; }
    const std::string& GetParameterNameY() const { return m_parameterY; }

    /// Set blend type
    void SetBlendType(Blend2DType type) { m_blendType = type; }
    Blend2DType GetBlendType() const { return m_blendType; }

    /// Add an entry
    void AddEntry(Vec2 position, BlendNodePtr node);
    void AddEntry(float x, float y, BlendNodePtr node);

    /// Add a clip entry (convenience)
    void AddClip(Vec2 position, AnimationClip::ConstPtr clip);
    void AddClip(float x, float y, AnimationClip::ConstPtr clip);

    /// Remove an entry
    void RemoveEntry(size_t index);

    /// Clear all entries
    void ClearEntries();

    /// Get entries
    const std::vector<BlendEntry2D>& GetEntries() const { return m_entries; }

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

    /// Get current position
    Vec2 GetCurrentPosition() const { return m_currentPosition; }

    /// Get bounds
    Vec2 GetMinBounds() const;
    Vec2 GetMaxBounds() const;

private:
    void CalculateWeightsFreeform(Vec2 position);
    void CalculateWeightsSimpleDirectional(Vec2 position);
    float CalculateGradientBandWeight(Vec2 samplePoint, Vec2 entryPos, size_t entryIndex);

    std::string m_parameterX = "BlendX";
    std::string m_parameterY = "BlendY";
    Blend2DType m_blendType = Blend2DType::FreeformDirectional;
    
    std::vector<BlendEntry2D> m_entries;
    Vec2 m_currentPosition{0.0f};
};

} // namespace RVX::Animation
