#pragma once

/**
 * @file CurveSerialization.h
 * @brief Serialization utilities for animation and gradient curves
 */

#include "Particle/Curves/AnimationCurve.h"
#include "Particle/Curves/GradientCurve.h"
#include "Core/Types.h"
#include <vector>
#include <string>

namespace RVX::Particle
{
    // =========================================================================
    // Curve Utilities
    // =========================================================================

    namespace CurveUtils
    {
        /// Compress an animation curve by removing redundant keyframes
        AnimationCurve CompressCurve(const AnimationCurve& curve, float tolerance = 0.001f);
        
        /// Quantize curve values for storage efficiency
        AnimationCurve QuantizeCurve(const AnimationCurve& curve, uint32 bits = 16);
        
        /// Auto-generate smooth tangents
        AnimationCurve AutoTangents(const AnimationCurve& curve);
        
        /// Resample curve to uniform spacing
        AnimationCurve Resample(const AnimationCurve& curve, uint32 numSamples);
    }

    namespace GradientUtils
    {
        /// Compress gradient by removing redundant keys
        GradientCurve CompressGradient(const GradientCurve& gradient, float tolerance = 0.01f);
        
        /// Blend two gradients together
        GradientCurve BlendGradients(const GradientCurve& a, const GradientCurve& b, float blendFactor);
        
        /// Invert gradient direction
        GradientCurve Invert(const GradientCurve& gradient);
        
        /// Adjust brightness
        GradientCurve AdjustBrightness(const GradientCurve& gradient, float brightness);
        
        /// Adjust saturation
        GradientCurve AdjustSaturation(const GradientCurve& gradient, float saturation);
    }

    // =========================================================================
    // Binary Serialization
    // =========================================================================

    /**
     * @brief Serialize an AnimationCurve to binary data
     */
    std::vector<uint8> SerializeCurve(const AnimationCurve& curve);

    /**
     * @brief Deserialize an AnimationCurve from binary data
     */
    AnimationCurve DeserializeCurve(const uint8* data, size_t size);

    /**
     * @brief Serialize a GradientCurve to binary data
     */
    std::vector<uint8> SerializeGradient(const GradientCurve& gradient);

    /**
     * @brief Deserialize a GradientCurve from binary data
     */
    GradientCurve DeserializeGradient(const uint8* data, size_t size);

    // =========================================================================
    // JSON Serialization (for editor/debugging)
    // =========================================================================

    /**
     * @brief Serialize an AnimationCurve to JSON string
     */
    std::string CurveToJson(const AnimationCurve& curve);

    /**
     * @brief Deserialize an AnimationCurve from JSON string
     */
    AnimationCurve CurveFromJson(const std::string& json);

    /**
     * @brief Serialize a GradientCurve to JSON string
     */
    std::string GradientToJson(const GradientCurve& gradient);

    /**
     * @brief Deserialize a GradientCurve from JSON string
     */
    GradientCurve GradientFromJson(const std::string& json);

} // namespace RVX::Particle
