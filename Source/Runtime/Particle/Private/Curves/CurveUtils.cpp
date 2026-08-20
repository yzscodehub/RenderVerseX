#include "Particle/Curves/AnimationCurve.h"
#include "Particle/Curves/GradientCurve.h"
#include "Core/Log.h"
#include <algorithm>
#include <cmath>

namespace RVX::Particle
{

// ============================================================================
// AnimationCurve Utilities
// ============================================================================

namespace CurveUtils
{
    /**
     * @brief Compress an animation curve by removing redundant keyframes
     * @param curve The curve to compress
     * @param tolerance Maximum allowed error (default 0.001)
     * @return Compressed curve with fewer keyframes
     */
    AnimationCurve CompressCurve(const AnimationCurve& curve, float tolerance = 0.001f)
    {
        const auto& keys = curve.GetKeys();
        if (keys.size() <= 2)
            return curve;

        AnimationCurve result;
        result.AddKey(keys.front());

        for (size_t i = 1; i < keys.size() - 1; ++i)
        {
            const auto& prev = result.GetKey(result.GetKeyCount() - 1);
            const auto& curr = keys[i];
            const auto& next = keys[i + 1];

            // Check if current keyframe can be removed
            // by linear interpolating between prev and next
            float t = (curr.time - prev.time) / (next.time - prev.time);
            float interpolated = prev.value + (next.value - prev.value) * t;
            float error = std::abs(curr.value - interpolated);

            if (error > tolerance)
            {
                result.AddKey(curr);
            }
        }

        result.AddKey(keys.back());
        return result;
    }

    /**
     * @brief Quantize curve values to reduce precision for storage
     * @param curve The curve to quantize
     * @param bits Number of bits for value quantization (default 16)
     * @return Quantized curve
     */
    AnimationCurve QuantizeCurve(const AnimationCurve& curve, uint32 bits = 16)
    {
        const auto& keys = curve.GetKeys();
        if (keys.empty())
            return curve;

        // Find min/max values
        float minVal = keys[0].value;
        float maxVal = keys[0].value;
        for (const auto& key : keys)
        {
            minVal = std::min(minVal, key.value);
            maxVal = std::max(maxVal, key.value);
        }

        float range = maxVal - minVal;
        if (range < 0.0001f)
            return curve;

        float maxQuant = static_cast<float>((1u << bits) - 1);

        AnimationCurve result;
        for (const auto& key : keys)
        {
            CurveKeyframe quantized = key;
            
            // Quantize value
            float normalized = (key.value - minVal) / range;
            float quantValue = std::round(normalized * maxQuant) / maxQuant;
            quantized.value = quantValue * range + minVal;

            // Quantize tangents
            quantized.inTangent = std::round(key.inTangent * 1000.0f) / 1000.0f;
            quantized.outTangent = std::round(key.outTangent * 1000.0f) / 1000.0f;

            result.AddKey(quantized);
        }

        return result;
    }

    /**
     * @brief Auto-generate smooth tangents for a curve
     * @param curve The curve to process
     * @return Curve with auto-calculated tangents
     */
    AnimationCurve AutoTangents(const AnimationCurve& curve)
    {
        auto& keys = const_cast<AnimationCurve&>(curve).GetKeys();
        if (keys.size() < 2)
            return curve;

        AnimationCurve result;
        
        for (size_t i = 0; i < keys.size(); ++i)
        {
            CurveKeyframe key = keys[i];

            if (i == 0)
            {
                // First key: use forward difference
                float dt = keys[i + 1].time - keys[i].time;
                float dv = keys[i + 1].value - keys[i].value;
                key.outTangent = dt > 0 ? dv / dt : 0.0f;
                key.inTangent = key.outTangent;
            }
            else if (i == keys.size() - 1)
            {
                // Last key: use backward difference
                float dt = keys[i].time - keys[i - 1].time;
                float dv = keys[i].value - keys[i - 1].value;
                key.inTangent = dt > 0 ? dv / dt : 0.0f;
                key.outTangent = key.inTangent;
            }
            else
            {
                // Middle keys: use central difference (Catmull-Rom style)
                float dt = keys[i + 1].time - keys[i - 1].time;
                float dv = keys[i + 1].value - keys[i - 1].value;
                float tangent = dt > 0 ? dv / dt : 0.0f;
                key.inTangent = tangent;
                key.outTangent = tangent;
            }

            result.AddKey(key);
        }

        return result;
    }

    /**
     * @brief Resample a curve to uniform keyframe spacing
     * @param curve The curve to resample
     * @param numSamples Number of uniform samples
     * @return Resampled curve
     */
    AnimationCurve Resample(const AnimationCurve& curve, uint32 numSamples)
    {
        if (numSamples < 2)
            return curve;

        AnimationCurve result;
        
        for (uint32 i = 0; i < numSamples; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(numSamples - 1);
            float value = curve.Evaluate(t);
            result.AddKey(t, value);
        }

        return AutoTangents(result);
    }

} // namespace CurveUtils

// ============================================================================
// GradientCurve Utilities
// ============================================================================

namespace GradientUtils
{
    /**
     * @brief Compress a gradient by removing redundant keys
     * @param gradient The gradient to compress
     * @param tolerance Color tolerance (per channel, 0-1)
     * @return Compressed gradient
     */
    GradientCurve CompressGradient(const GradientCurve& gradient, float tolerance = 0.01f)
    {
        const auto& keys = gradient.GetKeys();
        if (keys.size() <= 2)
            return gradient;

        GradientCurve result;
        result.AddKey(keys.front());

        for (size_t i = 1; i < keys.size() - 1; ++i)
        {
            const auto& prev = result.GetKey(result.GetKeyCount() - 1);
            const auto& curr = keys[i];
            const auto& next = keys[i + 1];

            // Interpolate color between prev and next
            float t = (curr.time - prev.time) / (next.time - prev.time);
            Vec4 interpolated = mix(prev.color, next.color, t);

            // Check error
            Vec4 diff = curr.color - interpolated;
            float maxError = std::max({
                std::abs(diff.x), std::abs(diff.y),
                std::abs(diff.z), std::abs(diff.w)
            });

            if (maxError > tolerance)
            {
                result.AddKey(curr);
            }
        }

        result.AddKey(keys.back());
        return result;
    }

    /**
     * @brief Blend two gradients together
     * @param a First gradient
     * @param b Second gradient
     * @param blendFactor 0 = a, 1 = b
     * @return Blended gradient
     */
    GradientCurve BlendGradients(const GradientCurve& a, const GradientCurve& b, float blendFactor)
    {
        GradientCurve result;
        
        // Sample at regular intervals
        constexpr uint32 numSamples = 16;
        for (uint32 i = 0; i < numSamples; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(numSamples - 1);
            Vec4 colorA = a.Evaluate(t);
            Vec4 colorB = b.Evaluate(t);
            Vec4 blended = mix(colorA, colorB, blendFactor);
            result.AddKey(t, blended);
        }

        return CompressGradient(result);
    }

    /**
     * @brief Invert a gradient (reverse direction)
     * @param gradient The gradient to invert
     * @return Inverted gradient
     */
    GradientCurve Invert(const GradientCurve& gradient)
    {
        const auto& keys = gradient.GetKeys();
        GradientCurve result;

        for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        {
            result.AddKey(1.0f - it->time, it->color);
        }

        return result;
    }

    /**
     * @brief Adjust gradient brightness
     * @param gradient The gradient to adjust
     * @param brightness Brightness multiplier (1.0 = no change)
     * @return Adjusted gradient
     */
    GradientCurve AdjustBrightness(const GradientCurve& gradient, float brightness)
    {
        GradientCurve result;
        
        for (const auto& key : gradient.GetKeys())
        {
            Vec4 adjusted = key.color;
            adjusted.x *= brightness;
            adjusted.y *= brightness;
            adjusted.z *= brightness;
            // Keep alpha unchanged
            result.AddKey(key.time, adjusted);
        }

        return result;
    }

    /**
     * @brief Adjust gradient saturation
     * @param gradient The gradient to adjust
     * @param saturation Saturation multiplier (0 = grayscale, 1 = no change)
     * @return Adjusted gradient
     */
    GradientCurve AdjustSaturation(const GradientCurve& gradient, float saturation)
    {
        GradientCurve result;
        
        for (const auto& key : gradient.GetKeys())
        {
            // Calculate luminance
            float lum = 0.299f * key.color.x + 0.587f * key.color.y + 0.114f * key.color.z;
            
            Vec4 adjusted;
            adjusted.x = lum + (key.color.x - lum) * saturation;
            adjusted.y = lum + (key.color.y - lum) * saturation;
            adjusted.z = lum + (key.color.z - lum) * saturation;
            adjusted.w = key.color.w;
            
            result.AddKey(key.time, adjusted);
        }

        return result;
    }

} // namespace GradientUtils

} // namespace RVX::Particle
