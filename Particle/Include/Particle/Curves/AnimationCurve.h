#pragma once

/**
 * @file AnimationCurve.h
 * @brief Animation curve for particle property modulation over lifetime
 */

#include "Core/Types.h"
#include <vector>
#include <algorithm>

namespace RVX::Particle
{
    /**
     * @brief Curve keyframe with tangent support
     */
    struct CurveKeyframe
    {
        float time = 0.0f;          ///< Normalized time (0-1)
        float value = 0.0f;         ///< Value at this keyframe
        float inTangent = 0.0f;     ///< Incoming tangent
        float outTangent = 0.0f;    ///< Outgoing tangent

        CurveKeyframe() = default;
        CurveKeyframe(float t, float v) : time(t), value(v) {}
        CurveKeyframe(float t, float v, float inTan, float outTan)
            : time(t), value(v), inTangent(inTan), outTangent(outTan) {}

        bool operator<(const CurveKeyframe& other) const { return time < other.time; }
    };

    /**
     * @brief Animation curve for value modulation over normalized time (0-1)
     * 
     * Used for properties like Size over Lifetime, Alpha over Lifetime, etc.
     * Supports Hermite interpolation between keyframes.
     */
    class AnimationCurve
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        AnimationCurve() = default;
        
        AnimationCurve(std::initializer_list<CurveKeyframe> keys)
            : m_keyframes(keys)
        {
            Sort();
        }

        // =====================================================================
        // Keyframe Management
        // =====================================================================

        /// Add a keyframe
        void AddKey(const CurveKeyframe& key)
        {
            m_keyframes.push_back(key);
            Sort();
        }

        /// Add a keyframe with just time and value
        void AddKey(float time, float value)
        {
            AddKey(CurveKeyframe(time, value));
        }

        /// Remove keyframe at index
        void RemoveKey(size_t index)
        {
            if (index < m_keyframes.size())
            {
                m_keyframes.erase(m_keyframes.begin() + index);
            }
        }

        /// Clear all keyframes
        void Clear() { m_keyframes.clear(); }

        /// Get number of keyframes
        size_t GetKeyCount() const { return m_keyframes.size(); }

        /// Get keyframe at index
        CurveKeyframe& GetKey(size_t index) { return m_keyframes[index]; }
        const CurveKeyframe& GetKey(size_t index) const { return m_keyframes[index]; }

        /// Get all keyframes
        std::vector<CurveKeyframe>& GetKeys() { return m_keyframes; }
        const std::vector<CurveKeyframe>& GetKeys() const { return m_keyframes; }

        // =====================================================================
        // Evaluation
        // =====================================================================

        /**
         * @brief Evaluate the curve at normalized time t (0-1)
         * @param t Normalized time
         * @return Interpolated value
         */
        float Evaluate(float t) const
        {
            if (m_keyframes.empty())
                return 0.0f;

            if (m_keyframes.size() == 1)
                return m_keyframes[0].value;

            // Clamp t to valid range
            t = std::clamp(t, 0.0f, 1.0f);

            // Find surrounding keyframes
            size_t i = 0;
            for (; i < m_keyframes.size() - 1; ++i)
            {
                if (t < m_keyframes[i + 1].time)
                    break;
            }

            if (i >= m_keyframes.size() - 1)
                return m_keyframes.back().value;

            const auto& k0 = m_keyframes[i];
            const auto& k1 = m_keyframes[i + 1];

            // Normalize t between keyframes
            float duration = k1.time - k0.time;
            if (duration <= 0.0f)
                return k0.value;

            float localT = (t - k0.time) / duration;

            // Hermite interpolation
            return HermiteInterpolate(k0.value, k0.outTangent * duration,
                                      k1.value, k1.inTangent * duration, localT);
        }

        // =====================================================================
        // GPU Export
        // =====================================================================

        /**
         * @brief Bake curve to a lookup table for GPU sampling
         * @param outData Output array
         * @param samples Number of samples (LUT size)
         */
        void BakeToLUT(float* outData, uint32 samples) const
        {
            for (uint32 i = 0; i < samples; ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(samples - 1);
                outData[i] = Evaluate(t);
            }
        }

        // =====================================================================
        // Presets
        // =====================================================================

        /// Linear curve from 0 to 1
        static AnimationCurve Linear()
        {
            return AnimationCurve({
                CurveKeyframe(0.0f, 0.0f, 0.0f, 1.0f),
                CurveKeyframe(1.0f, 1.0f, 1.0f, 0.0f)
            });
        }

        /// Constant value curve
        static AnimationCurve Constant(float value)
        {
            return AnimationCurve({
                CurveKeyframe(0.0f, value),
                CurveKeyframe(1.0f, value)
            });
        }

        /// Ease-in curve (slow start)
        static AnimationCurve EaseIn()
        {
            return AnimationCurve({
                CurveKeyframe(0.0f, 0.0f, 0.0f, 0.0f),
                CurveKeyframe(1.0f, 1.0f, 2.0f, 0.0f)
            });
        }

        /// Ease-out curve (slow end)
        static AnimationCurve EaseOut()
        {
            return AnimationCurve({
                CurveKeyframe(0.0f, 0.0f, 0.0f, 2.0f),
                CurveKeyframe(1.0f, 1.0f, 0.0f, 0.0f)
            });
        }

        /// Ease-in-out curve (slow start and end)
        static AnimationCurve EaseInOut()
        {
            return AnimationCurve({
                CurveKeyframe(0.0f, 0.0f, 0.0f, 0.0f),
                CurveKeyframe(1.0f, 1.0f, 0.0f, 0.0f)
            });
        }

        /// One (constant 1.0)
        static AnimationCurve One()
        {
            return Constant(1.0f);
        }

        /// Zero (constant 0.0)
        static AnimationCurve Zero()
        {
            return Constant(0.0f);
        }

        /// Fade out (1 to 0)
        static AnimationCurve FadeOut()
        {
            return AnimationCurve({
                CurveKeyframe(0.0f, 1.0f, 0.0f, -1.0f),
                CurveKeyframe(1.0f, 0.0f, -1.0f, 0.0f)
            });
        }

        /// Fade in (0 to 1)
        static AnimationCurve FadeIn()
        {
            return Linear();
        }

    private:
        void Sort()
        {
            std::sort(m_keyframes.begin(), m_keyframes.end());
        }

        /// Hermite interpolation
        static float HermiteInterpolate(float p0, float m0, float p1, float m1, float t)
        {
            float t2 = t * t;
            float t3 = t2 * t;
            
            float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            float h10 = t3 - 2.0f * t2 + t;
            float h01 = -2.0f * t3 + 3.0f * t2;
            float h11 = t3 - t2;

            return h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
        }

        std::vector<CurveKeyframe> m_keyframes;
    };

} // namespace RVX::Particle
